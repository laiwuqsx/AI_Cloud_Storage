#ifndef AI_CLOUD_CHUNK_UPLOAD_REPOSITORY_H
#define AI_CLOUD_CHUNK_UPLOAD_REPOSITORY_H

#include <stddef.h>
#include <stdint.h>

#define CHUNK_STORED_PATH_CAPACITY 512
#define CHUNK_UPLOAD_MAX_PARTS 10000U
#define CHUNK_UPLOAD_FILE_NAME_CAPACITY 129
#define CHUNK_UPLOAD_MD5_CAPACITY 33
#define CHUNK_UPLOAD_STATUS_CAPACITY 16

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
    GET_CHUNK_STATUS_OK = 0,
    GET_CHUNK_STATUS_UNAVAILABLE = 1,
    GET_CHUNK_STATUS_DATABASE_ERROR = -1
} GetChunkStatusResult;

typedef struct {
    char file_name[CHUNK_UPLOAD_FILE_NAME_CAPACITY];
    char file_md5[CHUNK_UPLOAD_MD5_CAPACITY];
    char status[CHUNK_UPLOAD_STATUS_CAPACITY];
    uint64_t total_size;
    uint64_t expires_in_seconds;
    unsigned int chunk_size;
    unsigned int total_chunks;
    unsigned int ready_chunks[CHUNK_UPLOAD_MAX_PARTS];
    size_t ready_count;
} ChunkUploadStatus;

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

typedef struct {
    char stored_path[CHUNK_STORED_PATH_CAPACITY];
    uint64_t size;
} ChunkUploadCompletionPart;

typedef struct {
    char file_name[CHUNK_UPLOAD_FILE_NAME_CAPACITY];
    char file_md5[CHUNK_UPLOAD_MD5_CAPACITY];
    uint64_t total_size;
    unsigned int total_chunks;
    ChunkUploadCompletionPart *parts;
} ChunkUploadCompletion;

typedef enum {
    CLAIM_CHUNK_COMPLETION_OK = 0,
    CLAIM_CHUNK_COMPLETION_INCOMPLETE = 1,
    CLAIM_CHUNK_COMPLETION_UNAVAILABLE = 2,
    CLAIM_CHUNK_COMPLETION_ALREADY_COMPLETED = 3,
    CLAIM_CHUNK_COMPLETION_DATABASE_ERROR = -1
} ClaimChunkCompletionResult;

CreateChunkSessionResult create_chunk_upload_session(
    const NewChunkUploadSession *session);

FindChunkSessionResult find_receiving_chunk_session(
    const char *upload_id, const char *user_name, ChunkUploadSessionPlan *plan);

GetChunkStatusResult get_chunk_upload_status(
    const char *upload_id, const char *user_name, ChunkUploadStatus *status);

ReserveChunkPartResult reserve_chunk_upload_part(const ChunkUploadPart *part);

/* 0: ready, 1: reservation no longer matches, -1: database error. */
int mark_chunk_upload_part_ready(const ChunkUploadPart *part);

/* Atomically changes a complete receiving session to completing and snapshots its parts. */
ClaimChunkCompletionResult claim_chunk_upload_completion(
    const char *upload_id, const char *user_name, ChunkUploadCompletion *completion);

/* success != 0 marks completed; success == 0 returns the session to receiving. */
int finish_chunk_upload_completion(const char *upload_id, const char *user_name,
                                   int success);
void free_chunk_upload_completion(ChunkUploadCompletion *completion);

#endif
