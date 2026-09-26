#ifndef AI_CLOUD_AI_USER_INDEX_TASK_H
#define AI_CLOUD_AI_USER_INDEX_TASK_H

#include "ai_index_event.h"

typedef struct {
    unsigned long long event_id;
    AiIndexEventType type;
    char user_name[33];
    char md5[33];
    unsigned long long user_file_id;
    unsigned int embedding_version;
} AiUserIndexTask;

int parse_ai_user_index_task(unsigned long long event_id, const char *payload,
                             AiUserIndexTask *task);

#endif
