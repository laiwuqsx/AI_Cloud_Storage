#include "ai_content_task.h"

#include <stdint.h>
#include <string.h>

#include "ai_index_event.h"
#include "json_util.h"

int parse_ai_content_task(unsigned long long event_id, const char *payload,
                          AiContentTask *task)
{
    char event_type[65];
    char md5[33];
    uint64_t version;
    AiIndexEvent event;

    if (event_id == 0 || !payload || !task ||
        json_get_string(payload, "event_type", event_type,
                        sizeof(event_type)) != 0 ||
        json_get_string(payload, "md5", md5, sizeof(md5)) != 0 ||
        json_get_uint64(payload, "embedding_version", &version) != 0 ||
        version == 0 || version > 4294967295ULL ||
        strcmp(event_type, "FILE_CONTENT_READY") != 0) return -1;
    event.type = AI_INDEX_EVENT_FILE_CONTENT_READY;
    event.md5 = md5;
    event.user_name = NULL;
    event.user_file_id = 0;
    event.embedding_version = (unsigned int)version;
    if (!validate_ai_index_event(&event)) return -1;
    memset(task, 0, sizeof(*task));
    task->event_id = event_id;
    memcpy(task->md5, md5, sizeof(task->md5));
    task->embedding_version = (unsigned int)version;
    return 0;
}
