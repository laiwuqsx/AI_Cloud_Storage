#ifndef AI_CLOUD_STORAGE_CLIENT_H
#define AI_CLOUD_STORAGE_CLIENT_H

#include <stddef.h>

#define STORAGE_KEY_CAPACITY 257
#define STORAGE_URL_CAPACITY 513

typedef struct {
    char storage_key[STORAGE_KEY_CAPACITY];
    char url[STORAGE_URL_CAPACITY];
} StoredObject;

typedef struct {
    void *context;
    int (*upload)(void *context, const char *local_path, StoredObject *stored_object);
    int (*remove)(void *context, const char *storage_key);
} StorageClient;

int storage_client_upload(const StorageClient *client, const char *local_path,
                          StoredObject *stored_object);
int storage_client_remove(const StorageClient *client, const char *storage_key);

#endif
