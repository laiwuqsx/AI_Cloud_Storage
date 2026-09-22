#ifndef AI_CLOUD_CHUNK_UPLOAD_REPOSITORY_H
#define AI_CLOUD_CHUNK_UPLOAD_REPOSITORY_H

#include <stdint.h>

typedef enum {
    CREATE_CHUNK_SESSION_OK = 0,
    CREATE_CHUNK_SESSION_DUPLICATE = 1,
    CREATE_CHUNK_SESSION_DATABASE_ERROR = -1
} CreateChunkSessionResult;

typedef struct {
    const char *upload_id;
    const char *user_name;
    const char *file_name;
    const char *file_md5;
    uint64_t total_size;
    unsigned int chunk_size;
    unsigned int total_chunks;
    unsigned int expires_in_seconds;
} NewChunkUploadSession;

CreateChunkSessionResult create_chunk_upload_session(
    const NewChunkUploadSession *session);

#endif
