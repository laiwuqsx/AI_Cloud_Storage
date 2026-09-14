#include <stdio.h>
#include <string.h>

#include "fastdfs_storage_client.h"

typedef struct {
    int result;
    int upload_calls;
    int delete_calls;
    const char *upload_output;
    const char *expected_local_path;
} FakeRunnerState;

static int fake_runner(void *opaque, const char *const arguments[],
                       char *output, size_t output_size)
{
    FakeRunnerState *state = opaque;

    if (strcmp(arguments[1], "/etc/fdfs/client.conf") != 0 || arguments[3] != NULL) return -1;
    if (strcmp(arguments[0], "fdfs_upload_file") == 0) {
        ++state->upload_calls;
        if (strcmp(arguments[2], state->expected_local_path) != 0) return -1;
        if (state->result != 0) return state->result;
        snprintf(output, output_size, "%s", state->upload_output);
        return 0;
    }
    if (strcmp(arguments[0], "fdfs_delete_file") == 0) {
        ++state->delete_calls;
        if (strcmp(arguments[2], "group1/M00/00/00/demo.txt") != 0) return -1;
        return state->result;
    }
    return -1;
}

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const char *local_path = "/tmp/report name;$(ignored).txt";
    FastDfsStorageContext context;
    FakeRunnerState state;
    StorageClient client;
    StoredObject stored;

    memset(&state, 0, sizeof(state));
    state.upload_output = "  group1/M00/00/00/demo.txt\n";
    state.expected_local_path = local_path;
    fastdfs_storage_context_init(&context, "/etc/fdfs/client.conf", "http://files.local:8888/");
    context.command_runner = fake_runner;
    context.command_context = &state;
    EXPECT(fastdfs_storage_client_init(&client, &context) == 0, "client initialization");

    EXPECT(storage_client_upload(&client, local_path, &stored) == 0, "upload command");
    EXPECT(state.upload_calls == 1, "upload call count");
    EXPECT(strcmp(stored.storage_key, "group1/M00/00/00/demo.txt") == 0,
           "trimmed storage key");
    EXPECT(strcmp(stored.url, "http://files.local:8888/group1/M00/00/00/demo.txt") == 0,
           "public URL");
    EXPECT(storage_client_remove(&client, stored.storage_key) == 0, "delete command");
    EXPECT(state.delete_calls == 1, "delete call count");

    state.result = -1;
    EXPECT(storage_client_upload(&client, local_path, &stored) != 0,
           "command failure is propagated");

    state.result = 0;
    state.upload_output = "../unsafe-file\n";
    EXPECT(storage_client_upload(&client, local_path, &stored) != 0,
           "unsafe storage key is rejected");
    EXPECT(storage_client_remove(&client, "../unsafe-file") != 0,
           "unsafe delete key is rejected");

    fastdfs_storage_context_init(&context, NULL, "http://files.local");
    EXPECT(fastdfs_storage_client_init(&client, &context) != 0,
           "missing client configuration is rejected");

    puts("fastdfs_storage_client tests passed");
    return 0;
}
