#include "ai_index_event.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "user_validation.h"

static int valid_md5(const char *value)
{
    size_t index;

    if (!value || strlen(value) != 32) return 0;
    for (index = 0; index < 32; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return 0;
    }
    return 1;
}

const char *ai_index_event_type_name(AiIndexEventType type)
{
    switch (type) {
    case AI_INDEX_EVENT_FILE_CONTENT_READY:
        return "FILE_CONTENT_READY";
    case AI_INDEX_EVENT_USER_FILE_ADDED:
        return "USER_FILE_ADDED";
    case AI_INDEX_EVENT_USER_FILE_REMOVED:
        return "USER_FILE_REMOVED";
    default:
        return NULL;
    }
}

AiIndexEventType parse_ai_index_event_type(const char *name)
{
    if (!name) return AI_INDEX_EVENT_INVALID;
    if (strcmp(name, "FILE_CONTENT_READY") == 0) {
        return AI_INDEX_EVENT_FILE_CONTENT_READY;
    }
    if (strcmp(name, "USER_FILE_ADDED") == 0) {
        return AI_INDEX_EVENT_USER_FILE_ADDED;
    }
    if (strcmp(name, "USER_FILE_REMOVED") == 0) {
        return AI_INDEX_EVENT_USER_FILE_REMOVED;
    }
    return AI_INDEX_EVENT_INVALID;
}

int validate_ai_index_event(const AiIndexEvent *event)
{
    if (!event || !valid_md5(event->md5) || event->embedding_version == 0 ||
        !ai_index_event_type_name(event->type)) return 0;
    if (event->type == AI_INDEX_EVENT_FILE_CONTENT_READY) {
        return (!event->user_name || event->user_name[0] == '\0') &&
               event->user_file_id == 0;
    }
    return validate_username(event->user_name) && event->user_file_id > 0;
}

int ai_index_event_idempotency_key(const AiIndexEvent *event,
                                   char *output, size_t output_size)
{
    const char *type;
    int length;

    if (!output || output_size == 0 || !validate_ai_index_event(event)) return -1;
    type = ai_index_event_type_name(event->type);
    if (event->type == AI_INDEX_EVENT_FILE_CONTENT_READY) {
        length = snprintf(output, output_size, "%s:%s:v%u", type, event->md5,
                          event->embedding_version);
    } else {
        length = snprintf(output, output_size, "%s:%s:%s:%llu:v%u", type,
                          event->user_name, event->md5, event->user_file_id,
                          event->embedding_version);
    }
    return length < 0 || (size_t)length >= output_size ? -1 : 0;
}
