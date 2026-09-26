#include <stdio.h>
#include <string.h>

#include "ai_user_index_task.h"

#define EXPECT(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } \
} while (0)

int main(void)
{
    const char *added =
        "{\"event_type\":\"USER_FILE_ADDED\",\"user_name\":\"alice_1\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"user_file_id\":42,\"embedding_version\":1}";
    const char *removed =
        "{\"event_type\":\"USER_FILE_REMOVED\",\"user_name\":\"alice_1\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"user_file_id\":42,\"embedding_version\":1}";
    AiUserIndexTask task;

    EXPECT(parse_ai_user_index_task(101, added, &task) == 0, "parse add");
    EXPECT(task.event_id == 101 && task.type == AI_INDEX_EVENT_USER_FILE_ADDED,
           "add identity");
    EXPECT(strcmp(task.user_name, "alice_1") == 0 && task.user_file_id == 42,
           "add owner and relation");
    EXPECT(parse_ai_user_index_task(102, removed, &task) == 0, "parse remove");
    EXPECT(task.type == AI_INDEX_EVENT_USER_FILE_REMOVED, "remove type");
    EXPECT(parse_ai_user_index_task(0, added, &task) != 0, "reject zero event");
    EXPECT(parse_ai_user_index_task(1,
        "{\"event_type\":\"FILE_CONTENT_READY\",\"user_name\":\"alice_1\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"user_file_id\":42,\"embedding_version\":1}", &task) != 0,
        "reject content event");
    EXPECT(parse_ai_user_index_task(1,
        "{\"event_type\":\"USER_FILE_ADDED\",\"user_name\":\"a!\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"user_file_id\":42,\"embedding_version\":1}", &task) != 0,
        "reject invalid user");
    EXPECT(parse_ai_user_index_task(1,
        "{\"event_type\":\"USER_FILE_ADDED\",\"user_name\":\"alice_1\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"user_file_id\":0,\"embedding_version\":1}", &task) != 0,
        "reject zero relation");
    puts("ai_user_index_task tests passed");
    return 0;
}
