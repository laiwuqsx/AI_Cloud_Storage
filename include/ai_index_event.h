#ifndef AI_CLOUD_AI_INDEX_EVENT_H
#define AI_CLOUD_AI_INDEX_EVENT_H

#include <stddef.h>

typedef enum {
    AI_INDEX_EVENT_INVALID = 0,
    AI_INDEX_EVENT_FILE_CONTENT_READY,
    AI_INDEX_EVENT_USER_FILE_ADDED,
    AI_INDEX_EVENT_USER_FILE_REMOVED
} AiIndexEventType;

typedef struct {
    AiIndexEventType type;
    const char *md5;
    const char *user_name;
    unsigned long long user_file_id;
    unsigned int embedding_version;
} AiIndexEvent;

const char *ai_index_event_type_name(AiIndexEventType type);
AiIndexEventType parse_ai_index_event_type(const char *name);
int validate_ai_index_event(const AiIndexEvent *event);

/* Stable key used by the future Outbox UNIQUE constraint to deduplicate retries. */
int ai_index_event_idempotency_key(const AiIndexEvent *event,
                                   char *output, size_t output_size);

#endif
