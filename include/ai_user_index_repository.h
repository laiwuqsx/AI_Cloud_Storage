#ifndef AI_CLOUD_AI_USER_INDEX_REPOSITORY_H
#define AI_CLOUD_AI_USER_INDEX_REPOSITORY_H

#include <stddef.h>

#include "ai_content_repository.h"
#include "ai_user_index_task.h"

typedef struct {
    float embedding[AI_CONTENT_EXPECTED_DIMENSION];
    unsigned long long vector_id;
    unsigned long long current_index_version;
    unsigned long long lease_generation;
    unsigned int retry_count;
} AiUserIndexSource;

typedef struct {
    float embedding[AI_CONTENT_EXPECTED_DIMENSION];
    unsigned long long vector_id;
} AiUserIndexVector;

typedef struct {
    AiUserIndexVector *vectors;
    size_t count;
    unsigned long long next_index_version;
} AiUserIndexSnapshot;

typedef enum {
    AI_USER_INDEX_CLAIM_ERROR = -1,
    AI_USER_INDEX_CLAIMED = 0,
    AI_USER_INDEX_ALREADY_APPLIED = 1,
    AI_USER_INDEX_BUSY = 2,
    AI_USER_INDEX_WAITING_CONTENT = 3,
    AI_USER_INDEX_STALE = 4,
    AI_USER_INDEX_MISSING = 5,
    AI_USER_INDEX_TERMINAL = 6,
    AI_USER_INDEX_DEFERRED = 7
} AiUserIndexClaimResult;

AiUserIndexClaimResult claim_ai_user_index_task(const AiUserIndexTask *task,
                                                AiUserIndexSource *source);
int load_ai_user_index_snapshot(const AiUserIndexTask *task,
                                unsigned long long lease_generation,
                                AiUserIndexSnapshot *snapshot);
void free_ai_user_index_snapshot(AiUserIndexSnapshot *snapshot);
int complete_ai_user_index_add(const AiUserIndexTask *task,
                               unsigned long long index_version,
                               unsigned long long lease_generation);
int complete_ai_user_index_remove(const AiUserIndexTask *task,
                                  unsigned long long index_version,
                                  unsigned long long lease_generation);
int retry_ai_user_index_task_after(const AiUserIndexTask *task,
                                   const char *last_error,
                                   unsigned int retry_after_seconds,
                                   unsigned long long lease_generation);
int fail_ai_user_index_task(const AiUserIndexTask *task, const char *last_error,
                            unsigned long long lease_generation);
int requeue_stale_ai_user_index_tasks(unsigned int stale_after_seconds);

#endif
