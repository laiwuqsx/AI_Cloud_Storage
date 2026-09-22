#ifndef AI_CLOUD_CHUNK_STORAGE_H
#define AI_CLOUD_CHUNK_STORAGE_H

#include <stddef.h>

/* Creates the session directory and returns its deterministic chunk path. */
int chunk_storage_path(const char *root, const char *upload_id,
                       unsigned int chunk_index, char *output, size_t output_size);

/* Atomically installs temp_path. Returns 0 if installed, 1 if already present. */
int chunk_storage_install(const char *temp_path, const char *final_path);

#endif
