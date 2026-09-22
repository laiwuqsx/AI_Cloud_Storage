#define _POSIX_C_SOURCE 200809L

#include "chunk_storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "upload_id.h"

static int ensure_directory(const char *path)
{
    struct stat info;

    if (mkdir(path, 0700) == 0) return 0;
    if (errno != EEXIST || stat(path, &info) != 0 || !S_ISDIR(info.st_mode)) return -1;
    return 0;
}

int chunk_storage_path(const char *root, const char *upload_id,
                       unsigned int chunk_index, char *output, size_t output_size)
{
    char session_directory[512];
    int length;

    if (!root || root[0] == '\0' || !validate_upload_id(upload_id) ||
        !output || output_size == 0) return -1;
    if (ensure_directory(root) != 0) return -1;
    length = snprintf(session_directory, sizeof(session_directory), "%s%s%s", root,
                      root[strlen(root) - 1] == '/' ? "" : "/", upload_id);
    if (length < 0 || (size_t)length >= sizeof(session_directory) ||
        ensure_directory(session_directory) != 0) return -1;
    length = snprintf(output, output_size, "%s/%u.part", session_directory, chunk_index);
    if (length < 0 || (size_t)length >= output_size) return -1;
    return 0;
}

int chunk_storage_install(const char *temp_path, const char *final_path)
{
    int result;

    if (!temp_path || temp_path[0] == '\0' || !final_path || final_path[0] == '\0') {
        return -1;
    }
    if (link(temp_path, final_path) == 0) {
        result = 0;
    } else if (errno == EEXIST) {
        result = 1;
    } else {
        return -1;
    }
    if (unlink(temp_path) != 0 && errno != ENOENT) return -1;
    return result;
}
