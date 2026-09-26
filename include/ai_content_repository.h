#ifndef AI_CLOUD_AI_CONTENT_REPOSITORY_H
#define AI_CLOUD_AI_CONTENT_REPOSITORY_H

#include <stddef.h>

#include "ai_content_task.h"

#define AI_CONTENT_STORAGE_KEY_CAPACITY 257
#define AI_CONTENT_TYPE_CAPACITY 33
#define AI_CONTENT_EXPECTED_DIMENSION 1024U

typedef struct {
    char storage_key[AI_CONTENT_STORAGE_KEY_CAPACITY];
    char type[AI_CONTENT_TYPE_CAPACITY];
    unsigned long long size;
    unsigned long long lease_generation;
    unsigned int retry_count;
} AiContentSource;

typedef enum {
    AI_CONTENT_CLAIM_ERROR = -1,
    AI_CONTENT_CLAIMED = 0,
    AI_CONTENT_ALREADY_READY = 1,
    AI_CONTENT_BUSY = 2,
    AI_CONTENT_DEFERRED = 3,
    AI_CONTENT_STALE = 4,
    AI_CONTENT_MISSING = 5,
    AI_CONTENT_TERMINAL = 6
} AiContentClaimResult;

AiContentClaimResult claim_ai_content_task(const AiContentTask *task,
                                           AiContentSource *source);

/* All transitions require the exact event lease currently stored as processing. */
int complete_ai_content_task(const AiContentTask *task, const char *description,
                             const void *embedding, size_t embedding_bytes,
                             const char *model, unsigned int dimension,
                             unsigned long long lease_generation);
int retry_ai_content_task_after(const AiContentTask *task, const char *last_error,
                                unsigned int retry_after_seconds,
                                unsigned long long lease_generation);
int fail_ai_content_task(const AiContentTask *task, const char *last_error,
                         unsigned long long lease_generation);

/* Returns the number of interrupted processing rows returned to pending. */
int requeue_stale_ai_content_tasks(unsigned int stale_after_seconds);

#endif
