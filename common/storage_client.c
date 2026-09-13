#include "storage_client.h"

#include <string.h>

int storage_client_upload(const StorageClient *client, const char *local_path,
                          StoredObject *stored_object)
{
    if (!client || !client->upload || !local_path || local_path[0] == '\0' || !stored_object) {
        return -1;
    }
    memset(stored_object, 0, sizeof(*stored_object));
    if (client->upload(client->context, local_path, stored_object) != 0) return -1;
    if (stored_object->storage_key[0] == '\0' || stored_object->url[0] == '\0') return -1;
    return 0;
}

int storage_client_remove(const StorageClient *client, const char *storage_key)
{
    if (!client || !client->remove || !storage_key || storage_key[0] == '\0') return -1;
    return client->remove(client->context, storage_key);
}
