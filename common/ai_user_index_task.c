#include "ai_user_index_task.h"

#include <stdint.h>
#include <string.h>

#include "json_util.h"

int parse_ai_user_index_task(unsigned long long event_id, const char *payload,
                             AiUserIndexTask *task)
{
    char event_type[65], user_name[33], md5[33];
    uint64_t user_file_id, version;
    AiIndexEvent event;

    if (event_id == 0 || !payload || !task ||
        json_get_string(payload, "event_type", event_type,
                        sizeof(event_type)) != 0 ||
        json_get_string(payload, "user_name", user_name,
                        sizeof(user_name)) != 0 ||
        json_get_string(payload, "md5", md5, sizeof(md5)) != 0 ||
        json_get_uint64(payload, "user_file_id", &user_file_id) != 0 ||
        json_get_uint64(payload, "embedding_version", &version) != 0 ||
        user_file_id == 0 || version == 0 || version > 4294967295ULL)
        return -1;
    event.type = parse_ai_index_event_type(event_type);
    event.md5 = md5;
    event.user_name = user_name;
    event.user_file_id = (unsigned long long)user_file_id;
    event.embedding_version = (unsigned int)version;
    if ((event.type != AI_INDEX_EVENT_USER_FILE_ADDED &&
         event.type != AI_INDEX_EVENT_USER_FILE_REMOVED) ||
        !validate_ai_index_event(&event)) return -1;
    memset(task, 0, sizeof(*task));
    task->event_id = event_id;
    task->type = event.type;
    memcpy(task->user_name, user_name, sizeof(task->user_name));
    memcpy(task->md5, md5, sizeof(task->md5));
    task->user_file_id = (unsigned long long)user_file_id;
    task->embedding_version = (unsigned int)version;
    return 0;
}
