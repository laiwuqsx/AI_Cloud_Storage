#include "fastdfs_storage_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_process(void *unused, const char *const arguments[],
                       char *output, size_t output_size)
{
    int descriptors[2];
    pid_t child;
    pid_t waited;
    size_t used = 0;
    int status = 0;
    int truncated = 0;
    int read_failed = 0;
    ssize_t count;
    char buffer[256];

    (void)unused;
    if (!arguments || !arguments[0] || (output && output_size == 0)) return -1;
    if (pipe(descriptors) != 0) return -1;
    child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(127);
        close(descriptors[1]);
        execvp(arguments[0], (char *const *)arguments);
        _exit(127);
    }

    close(descriptors[1]);
    while ((count = read(descriptors[0], buffer, sizeof(buffer))) != 0) {
        size_t available;
        size_t copy_size;

        if (count < 0) {
            if (errno == EINTR) continue;
            read_failed = 1;
            break;
        }
        if (!output) continue;
        available = output_size - 1 - used;
        copy_size = (size_t)count < available ? (size_t)count : available;
        if (copy_size > 0) {
            memcpy(output + used, buffer, copy_size);
            used += copy_size;
        }
        if (copy_size < (size_t)count) truncated = 1;
    }
    close(descriptors[0]);
    if (output) output[used] = '\0';
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || read_failed || truncated ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static void trim_output(char *value)
{
    char *begin;
    size_t length;

    if (!value) return;
    begin = value;
    while (*begin && isspace((unsigned char)*begin)) ++begin;
    if (begin != value) memmove(value, begin, strlen(begin) + 1);
    length = strlen(value);
    while (length > 0 && isspace((unsigned char)value[length - 1])) {
        value[--length] = '\0';
    }
}

static int valid_storage_key(const char *storage_key)
{
    size_t index;
    size_t length;

    if (!storage_key || storage_key[0] == '/' || strstr(storage_key, "..")) return 0;
    length = strlen(storage_key);
    if (length == 0 || length >= STORAGE_KEY_CAPACITY) return 0;
    for (index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)storage_key[index];
        if (!isalnum(character) && character != '/' && character != '_' &&
            character != '-' && character != '.') return 0;
    }
    return 1;
}

static int make_public_url(const char *base_url, const char *storage_key,
                           char *url, size_t url_size)
{
    size_t base_length;
    size_t key_length;

    if (!base_url || !storage_key || !url) return -1;
    base_length = strlen(base_url);
    while (base_length > 0 && base_url[base_length - 1] == '/') --base_length;
    key_length = strlen(storage_key);
    if (base_length == 0 || base_length + 1 + key_length >= url_size) return -1;
    memcpy(url, base_url, base_length);
    url[base_length] = '/';
    memcpy(url + base_length + 1, storage_key, key_length + 1);
    return 0;
}

static int run_context_command(FastDfsStorageContext *context,
                               const char *const arguments[],
                               char *output, size_t output_size)
{
    FastDfsCommandRunner runner = context->command_runner ? context->command_runner : run_process;
    void *runner_context = context->command_runner ? context->command_context : NULL;

    return runner(runner_context, arguments, output, output_size);
}

static int upload_file(void *opaque, const char *local_path, StoredObject *stored_object)
{
    FastDfsStorageContext *context = opaque;
    const char *arguments[4];
    char output[STORAGE_KEY_CAPACITY];

    arguments[0] = context->upload_command;
    arguments[1] = context->client_config_path;
    arguments[2] = local_path;
    arguments[3] = NULL;
    if (run_context_command(context, arguments, output, sizeof(output)) != 0) return -1;
    trim_output(output);
    if (!valid_storage_key(output)) return -1;
    memcpy(stored_object->storage_key, output, strlen(output) + 1);
    if (make_public_url(context->public_base_url, output,
                        stored_object->url, sizeof(stored_object->url)) != 0) return -1;
    return 0;
}

static int remove_file(void *opaque, const char *storage_key)
{
    FastDfsStorageContext *context = opaque;
    const char *arguments[4];

    if (!valid_storage_key(storage_key)) return -1;
    arguments[0] = context->delete_command;
    arguments[1] = context->client_config_path;
    arguments[2] = storage_key;
    arguments[3] = NULL;
    return run_context_command(context, arguments, NULL, 0);
}

void fastdfs_storage_context_init(FastDfsStorageContext *context,
                                  const char *client_config_path,
                                  const char *public_base_url)
{
    if (!context) return;
    memset(context, 0, sizeof(*context));
    context->client_config_path = client_config_path;
    context->public_base_url = public_base_url;
    context->upload_command = "fdfs_upload_file";
    context->delete_command = "fdfs_delete_file";
}

int fastdfs_storage_client_init(StorageClient *client, FastDfsStorageContext *context)
{
    if (!client || !context || !context->client_config_path ||
        context->client_config_path[0] == '\0' || !context->public_base_url ||
        context->public_base_url[0] == '\0' || !context->upload_command ||
        !context->delete_command) return -1;
    client->context = context;
    client->upload = upload_file;
    client->remove = remove_file;
    return 0;
}
