#ifndef AI_CLOUD_CHUNK_UPLOAD_REPOSITORY_H
#define AI_CLOUD_CHUNK_UPLOAD_REPOSITORY_H

#include <stdint.h>

#define CHUNK_STORED_PATH_CAPACITY 512

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

typedef struct {
    uint64_t total_size;
    unsigned int chunk_size;
    unsigned int total_chunks;
} ChunkUploadSessionPlan;

typedef enum {
    FIND_CHUNK_SESSION_OK = 0,
    FIND_CHUNK_SESSION_UNAVAILABLE = 1,
    FIND_CHUNK_SESSION_DATABASE_ERROR = -1
} FindChunkSessionResult;

typedef enum {
    RESERVE_CHUNK_PART_NEW = 0,
    RESERVE_CHUNK_PART_STAGING = 1,
    RESERVE_CHUNK_PART_READY = 2,
    RESERVE_CHUNK_PART_CONFLICT = 3,
    RESERVE_CHUNK_PART_DATABASE_ERROR = -1
} ReserveChunkPartResult;

typedef struct {
    const char *upload_id;
    unsigned int chunk_index;
    uint64_t size;
    const char *chunk_md5;
    const char *stored_path;
} ChunkUploadPart;

CreateChunkSessionResult create_chunk_upload_session(
    const NewChunkUploadSession *session);

FindChunkSessionResult find_receiving_chunk_session(
    const char *upload_id, const char *user_name, ChunkUploadSessionPlan *plan);

ReserveChunkPartResult reserve_chunk_upload_part(const ChunkUploadPart *part);

/* 0: ready, 1: reservation no longer matches, -1: database error. */
int mark_chunk_upload_part_ready(const ChunkUploadPart *part);

#endif
