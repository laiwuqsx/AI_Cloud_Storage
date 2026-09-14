#ifndef AI_CLOUD_FASTDFS_STORAGE_CLIENT_H
#define AI_CLOUD_FASTDFS_STORAGE_CLIENT_H

#include <stddef.h>

#include "storage_client.h"

typedef int (*FastDfsCommandRunner)(void *context, const char *const arguments[],
                                    char *output, size_t output_size);

typedef struct {
    const char *client_config_path;
    const char *public_base_url;
    const char *upload_command;
    const char *delete_command;
    FastDfsCommandRunner command_runner;
    void *command_context;
} FastDfsStorageContext;

void fastdfs_storage_context_init(FastDfsStorageContext *context,
                                  const char *client_config_path,
                                  const char *public_base_url);
int fastdfs_storage_client_init(StorageClient *client, FastDfsStorageContext *context);

#endif
