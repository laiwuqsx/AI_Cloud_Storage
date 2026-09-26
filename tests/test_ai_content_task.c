#include "ai_content_task.h"

#include <stdio.h>
#include <string.h>

#define EXPECT(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } \
} while (0)

int main(void)
{
    const char *valid =
        "{\"event_type\":\"FILE_CONTENT_READY\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"embedding_version\":1}";
    AiContentTask task;

    EXPECT(parse_ai_content_task(42, valid, &task) == 0, "parse valid task");
    EXPECT(task.event_id == 42, "preserve event id");
    EXPECT(strcmp(task.md5, "5eb63bbbe01eeed093cb22bb8f5acdc3") == 0,
           "parse md5");
    EXPECT(task.embedding_version == 1, "parse version");
    EXPECT(parse_ai_content_task(0, valid, &task) != 0, "reject missing event id");
    EXPECT(parse_ai_content_task(1,
        "{\"event_type\":\"USER_FILE_ADDED\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"embedding_version\":1}", &task) != 0, "reject wrong event type");
    EXPECT(parse_ai_content_task(1,
        "{\"event_type\":\"FILE_CONTENT_READY\","
        "\"md5\":\"INVALID\",\"embedding_version\":1}", &task) != 0,
        "reject invalid md5");
    EXPECT(parse_ai_content_task(1,
        "{\"event_type\":\"FILE_CONTENT_READY\","
        "\"md5\":\"5eb63bbbe01eeed093cb22bb8f5acdc3\","
        "\"embedding_version\":0}", &task) != 0, "reject zero version");
    puts("ai_content_task tests passed");
    return 0;
}
