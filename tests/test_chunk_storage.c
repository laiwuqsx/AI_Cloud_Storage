#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chunk_storage.h"

int main(void)
{
    const char *upload_id =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char root[] = "/tmp/ai-cloud-chunks-XXXXXX";
    char first_temp[512];
    char second_temp[512];
    char final_path[512];
    char session_directory[512];
    char content[8] = {0};
    int first_descriptor;
    int second_descriptor;
    int final_descriptor;
    int root_descriptor;

    root_descriptor = mkstemp(root);
    assert(root_descriptor >= 0);
    assert(close(root_descriptor) == 0);
    assert(unlink(root) == 0);
    assert(mkdir(root, 0700) == 0);
    assert(chunk_storage_path(root, upload_id, 7, final_path, sizeof(final_path)) == 0);
    assert(snprintf(first_temp, sizeof(first_temp), "%s/first-XXXXXX", root) > 0);
    first_descriptor = mkstemp(first_temp);
    assert(first_descriptor >= 0);
    assert(write(first_descriptor, "first", 5) == 5);
    assert(close(first_descriptor) == 0);
    assert(chunk_storage_install(first_temp, final_path) == 0);

    assert(snprintf(second_temp, sizeof(second_temp), "%s/second-XXXXXX", root) > 0);
    second_descriptor = mkstemp(second_temp);
    assert(second_descriptor >= 0);
    assert(write(second_descriptor, "second", 6) == 6);
    assert(close(second_descriptor) == 0);
    assert(chunk_storage_install(second_temp, final_path) == 1);
    final_descriptor = open(final_path, O_RDONLY);
    assert(final_descriptor >= 0);
    assert(read(final_descriptor, content, sizeof(content)) == 5);
    assert(close(final_descriptor) == 0);
    assert(strcmp(content, "first") == 0);

    assert(snprintf(session_directory, sizeof(session_directory), "%s/%s",
                    root, upload_id) > 0);
    assert(unlink(final_path) == 0);
    assert(rmdir(session_directory) == 0);
    assert(rmdir(root) == 0);
    puts("chunk storage tests passed");
    return 0;
}
