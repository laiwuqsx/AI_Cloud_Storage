#include "outbox_repository.h"

#include <stdio.h>
#include <string.h>

#define EVENT_KEY_CAPACITY 256
#define AGGREGATE_KEY_CAPACITY 161
#define EVENT_PAYLOAD_CAPACITY 512

static int execute_content_state(MYSQL *connection, const AiIndexEvent *event)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[2];
    unsigned long md5_length;
    unsigned int version = event->embedding_version;
    const char *sql =
        "INSERT INTO file_ai_metadata (md5, status, embedding_version) "
        "VALUES (?, 'pending', ?) ON DUPLICATE KEY UPDATE md5 = VALUES(md5)";
    int result = -1;

    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0)
        goto done;
    memset(bind, 0, sizeof(bind));
    md5_length = (unsigned long)strlen(event->md5);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)event->md5;
    bind[0].length = &md5_length;
    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = &version;
    bind[1].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

static int execute_user_state(MYSQL *connection, const AiIndexEvent *event)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[4];
    unsigned long user_length, md5_length;
    my_ulonglong relation_id = (my_ulonglong)event->user_file_id;
    unsigned int version = event->embedding_version;
    const char *added_sql =
        "INSERT INTO user_ai_index_entry "
        "(user_name, md5, user_file_id, status, embedding_version) "
        "VALUES (?, ?, ?, 'pending', ?) "
        "ON DUPLICATE KEY UPDATE user_file_id = VALUES(user_file_id), "
        "status = 'pending', vector_id = NULL, "
        "embedding_version = VALUES(embedding_version), index_version = 0, "
        "retry_count = 0, last_error = '', next_attempt_at = CURRENT_TIMESTAMP, "
        "indexed_at = NULL";
    const char *removed_sql =
        "INSERT INTO user_ai_index_entry "
        "(user_name, md5, user_file_id, status, embedding_version) "
        "VALUES (?, ?, ?, 'removing', ?) "
        "ON DUPLICATE KEY UPDATE "
        "status = IF(user_file_id = VALUES(user_file_id), 'removing', status), "
        "retry_count = IF(user_file_id = VALUES(user_file_id), 0, retry_count), "
        "last_error = IF(user_file_id = VALUES(user_file_id), '', last_error), "
        "next_attempt_at = IF(user_file_id = VALUES(user_file_id), "
        "CURRENT_TIMESTAMP, next_attempt_at)";
    const char *sql = event->type == AI_INDEX_EVENT_USER_FILE_ADDED
        ? added_sql : removed_sql;
    int result = -1;

    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0)
        goto done;
    memset(bind, 0, sizeof(bind));
    user_length = (unsigned long)strlen(event->user_name);
    md5_length = (unsigned long)strlen(event->md5);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)event->user_name;
    bind[0].length = &user_length;
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)event->md5;
    bind[1].length = &md5_length;
    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &relation_id;
    bind[2].is_unsigned = 1;
    bind[3].buffer_type = MYSQL_TYPE_LONG;
    bind[3].buffer = &version;
    bind[3].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

static int build_event_document(const AiIndexEvent *event,
                                char aggregate[AGGREGATE_KEY_CAPACITY],
                                char payload[EVENT_PAYLOAD_CAPACITY])
{
    const char *type = ai_index_event_type_name(event->type);
    int aggregate_length, payload_length;

    if (event->type == AI_INDEX_EVENT_FILE_CONTENT_READY) {
        aggregate_length = snprintf(aggregate, AGGREGATE_KEY_CAPACITY,
                                    "content:%s", event->md5);
        payload_length = snprintf(payload, EVENT_PAYLOAD_CAPACITY,
            "{\"event_type\":\"%s\",\"md5\":\"%s\","
            "\"embedding_version\":%u}",
            type, event->md5, event->embedding_version);
    } else {
        aggregate_length = snprintf(aggregate, AGGREGATE_KEY_CAPACITY,
                                    "user:%s:%s", event->user_name, event->md5);
        payload_length = snprintf(payload, EVENT_PAYLOAD_CAPACITY,
            "{\"event_type\":\"%s\",\"user_name\":\"%s\","
            "\"md5\":\"%s\",\"user_file_id\":%llu,"
            "\"embedding_version\":%u}",
            type, event->user_name, event->md5, event->user_file_id,
            event->embedding_version);
    }
    return aggregate_length < 0 || aggregate_length >= AGGREGATE_KEY_CAPACITY ||
           payload_length < 0 || payload_length >= EVENT_PAYLOAD_CAPACITY ? -1 : 0;
}

static int insert_outbox_event(MYSQL *connection, const AiIndexEvent *event)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[4];
    unsigned long lengths[4];
    char key[EVENT_KEY_CAPACITY];
    char aggregate[AGGREGATE_KEY_CAPACITY];
    char payload[EVENT_PAYLOAD_CAPACITY];
    const char *type = ai_index_event_type_name(event->type);
    const char *sql =
        "INSERT INTO outbox_event "
        "(event_type, aggregate_key, idempotency_key, payload) VALUES (?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE id = id";
    int result = -1;

    if (ai_index_event_idempotency_key(event, key, sizeof(key)) != 0 ||
        build_event_document(event, aggregate, payload) != 0) return -1;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0)
        goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(type);
    lengths[1] = (unsigned long)strlen(aggregate);
    lengths[2] = (unsigned long)strlen(key);
    lengths[3] = (unsigned long)strlen(payload);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)type;
    bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = aggregate;
    bind[1].length = &lengths[1];
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = key;
    bind[2].length = &lengths[2];
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = payload;
    bind[3].length = &lengths[3];
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

int enqueue_ai_index_event_in_transaction(MYSQL *connection,
                                          const AiIndexEvent *event)
{
    if (!connection || !validate_ai_index_event(event)) return -1;
    if (event->type == AI_INDEX_EVENT_FILE_CONTENT_READY) {
        if (execute_content_state(connection, event) != 0) return -1;
    } else if (execute_user_state(connection, event) != 0) {
        return -1;
    }
    return insert_outbox_event(connection, event);
}
