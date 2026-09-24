#include "ai_index_event.h"

#include <stdio.h>
#include <string.h>

#define EXPECT(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } \
} while (0)

int main(void)
{
    const char *md5 = "5eb63bbbe01eeed093cb22bb8f5acdc3";
    char key[256];
    AiIndexEvent content = {
        AI_INDEX_EVENT_FILE_CONTENT_READY, md5, NULL, 0, 1
    };
    AiIndexEvent added = {
        AI_INDEX_EVENT_USER_FILE_ADDED, md5, "alice", 42, 1
    };
    AiIndexEvent removed = {
        AI_INDEX_EVENT_USER_FILE_REMOVED, md5, "alice", 42, 1
    };

    EXPECT(strcmp(ai_index_event_type_name(content.type),
                  "FILE_CONTENT_READY") == 0, "content event name");
    EXPECT(parse_ai_index_event_type("USER_FILE_ADDED") ==
               AI_INDEX_EVENT_USER_FILE_ADDED, "parse added event");
    EXPECT(parse_ai_index_event_type("unknown") == AI_INDEX_EVENT_INVALID,
           "reject unknown event");
    EXPECT(validate_ai_index_event(&content), "valid content event");
    EXPECT(validate_ai_index_event(&added), "valid user added event");
    EXPECT(validate_ai_index_event(&removed), "valid user removed event");

    EXPECT(ai_index_event_idempotency_key(&content, key, sizeof(key)) == 0 &&
               strcmp(key, "FILE_CONTENT_READY:5eb63bbbe01eeed093cb22bb8f5acdc3:v1") == 0,
           "content idempotency key");
    EXPECT(ai_index_event_idempotency_key(&added, key, sizeof(key)) == 0 &&
               strcmp(key, "USER_FILE_ADDED:alice:5eb63bbbe01eeed093cb22bb8f5acdc3:42:v1") == 0,
           "user relation idempotency key");
    EXPECT(ai_index_event_idempotency_key(&removed, key, sizeof(key)) == 0 &&
               strcmp(key, "USER_FILE_REMOVED:alice:5eb63bbbe01eeed093cb22bb8f5acdc3:42:v1") == 0,
           "add and remove are distinct events");

    content.user_name = "alice";
    EXPECT(!validate_ai_index_event(&content),
           "content event is global rather than user scoped");
    content.user_name = NULL;
    added.user_file_id = 0;
    EXPECT(!validate_ai_index_event(&added), "user event requires relation id");
    added.user_file_id = 42;
    added.md5 = "ABCDEF0123456789ABCDEF0123456789";
    EXPECT(!validate_ai_index_event(&added), "canonical lowercase md5 required");
    EXPECT(ai_index_event_idempotency_key(&removed, key, 8) != 0,
           "reject truncated idempotency key");

    puts("ai_index_event tests passed");
    return 0;
}
