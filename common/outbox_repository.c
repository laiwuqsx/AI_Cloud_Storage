#include "outbox_repository.h"

#include <stdio.h>
#include <string.h>

#include "runtime_config.h"

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
        "processing_started_at = NULL, processing_event_id = NULL, "
        "completed_event_id = NULL, processing_generation = processing_generation + 1, "
        "indexed_at = NULL";
    const char *removed_sql =
        "INSERT INTO user_ai_index_entry "
        "(user_name, md5, user_file_id, status, embedding_version) "
        "VALUES (?, ?, ?, 'removing', ?) "
        "ON DUPLICATE KEY UPDATE "
        "status = IF(user_file_id = VALUES(user_file_id), 'removing', status), "
        "retry_count = IF(user_file_id = VALUES(user_file_id), 0, retry_count), "
        "last_error = IF(user_file_id = VALUES(user_file_id), '', last_error), "
        "processing_started_at = IF(user_file_id = VALUES(user_file_id), "
        "NULL, processing_started_at), "
        "processing_event_id = IF(user_file_id = VALUES(user_file_id), "
        "NULL, processing_event_id), "
        "completed_event_id = IF(user_file_id = VALUES(user_file_id), "
        "NULL, completed_event_id), "
        "processing_generation = IF(user_file_id = VALUES(user_file_id), "
        "processing_generation + 1, processing_generation), "
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

static MYSQL *connect_database(void)
{
    MYSQL *connection = mysql_init(NULL);

    if (!connection) return NULL;
    if (!mysql_real_connect(connection,
                            runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) {
        mysql_close(connection);
        return NULL;
    }
    return connection;
}

int claim_next_outbox_event(OutboxPublishEvent *event)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *select_statement = NULL;
    MYSQL_STMT *update_statement = NULL;
    MYSQL_BIND result_bind[4], update_bind[1];
    unsigned long lengths[2];
    my_ulonglong id;
    unsigned int retry_count;
    char event_type[OUTBOX_EVENT_TYPE_CAPACITY];
    char payload[OUTBOX_EVENT_PAYLOAD_CAPACITY];
    const char *select_sql =
        "SELECT id, event_type, CAST(payload AS CHAR), retry_count FROM outbox_event "
        "WHERE status = 'pending' AND next_attempt_at <= CURRENT_TIMESTAMP "
        "ORDER BY next_attempt_at, id LIMIT 1 FOR UPDATE SKIP LOCKED";
    const char *update_sql =
        "UPDATE outbox_event SET status = 'publishing', last_error = '' "
        "WHERE id = ? AND status = 'pending'";
    int fetch_result;
    int result = -1;

    if (!event) return -1;
    memset(event, 0, sizeof(*event));
    memset(event_type, 0, sizeof(event_type));
    memset(payload, 0, sizeof(payload));
    connection = connect_database();
    if (!connection || mysql_autocommit(connection, 0) != 0) goto done;
    select_statement = mysql_stmt_init(connection);
    if (!select_statement ||
        mysql_stmt_prepare(select_statement, select_sql,
                           (unsigned long)strlen(select_sql)) != 0 ||
        mysql_stmt_execute(select_statement) != 0) goto rollback;

    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &id;
    result_bind[0].is_unsigned = 1;
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = event_type;
    result_bind[1].buffer_length = sizeof(event_type) - 1;
    result_bind[1].length = &lengths[0];
    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = payload;
    result_bind[2].buffer_length = sizeof(payload) - 1;
    result_bind[2].length = &lengths[1];
    result_bind[3].buffer_type = MYSQL_TYPE_LONG;
    result_bind[3].buffer = &retry_count;
    result_bind[3].is_unsigned = 1;
    if (mysql_stmt_bind_result(select_statement, result_bind) != 0 ||
        mysql_stmt_store_result(select_statement) != 0) goto rollback;
    fetch_result = mysql_stmt_fetch(select_statement);
    if (fetch_result == MYSQL_NO_DATA) {
        if (mysql_commit(connection) != 0) goto rollback;
        result = 1;
        goto done;
    }
    if (fetch_result != 0 || lengths[0] >= sizeof(event_type) ||
        lengths[1] >= sizeof(payload)) goto rollback;
    event_type[lengths[0]] = '\0';
    payload[lengths[1]] = '\0';

    update_statement = mysql_stmt_init(connection);
    if (!update_statement ||
        mysql_stmt_prepare(update_statement, update_sql,
                           (unsigned long)strlen(update_sql)) != 0) goto rollback;
    memset(update_bind, 0, sizeof(update_bind));
    update_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    update_bind[0].buffer = &id;
    update_bind[0].is_unsigned = 1;
    if (mysql_stmt_bind_param(update_statement, update_bind) != 0 ||
        mysql_stmt_execute(update_statement) != 0 ||
        mysql_stmt_affected_rows(update_statement) != 1 ||
        mysql_commit(connection) != 0) goto rollback;

    event->id = (unsigned long long)id;
    memcpy(event->event_type, event_type, lengths[0] + 1);
    memcpy(event->payload, payload, lengths[1] + 1);
    event->retry_count = retry_count;
    result = 0;
    goto done;

rollback:
    mysql_rollback(connection);
done:
    if (select_statement) mysql_stmt_close(select_statement);
    if (update_statement) mysql_stmt_close(update_statement);
    if (connection) mysql_close(connection);
    return result;
}

static int update_publishing_event(unsigned long long event_id, const char *sql,
                                   const char *last_error)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[2];
    my_ulonglong id = (my_ulonglong)event_id;
    unsigned long error_length = 0;
    int result = -1;

    if (event_id == 0 || (last_error &&
        (last_error[0] == '\0' || strlen(last_error) > 512))) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    if (last_error) {
        error_length = (unsigned long)strlen(last_error);
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = (void *)last_error;
        bind[0].length = &error_length;
        bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[1].buffer = &id;
        bind[1].is_unsigned = 1;
    } else {
        bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[0].buffer = &id;
        bind[0].is_unsigned = 1;
    }
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = mysql_stmt_affected_rows(statement) == 1 ? 0 : 1;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int mark_outbox_event_published(unsigned long long event_id)
{
    const char *sql =
        "UPDATE outbox_event SET status = 'published', last_error = '', "
        "published_at = CURRENT_TIMESTAMP "
        "WHERE id = ? AND status = 'publishing'";

    return update_publishing_event(event_id, sql, NULL);
}

int retry_outbox_event_after(unsigned long long event_id, const char *last_error,
                             unsigned int retry_after_seconds)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[3];
    my_ulonglong id = (my_ulonglong)event_id;
    unsigned int delay = retry_after_seconds;
    unsigned long error_length;
    const char *sql =
        "UPDATE outbox_event SET status = 'pending', retry_count = retry_count + 1, "
        "last_error = ?, next_attempt_at = TIMESTAMPADD(SECOND, ?, CURRENT_TIMESTAMP) "
        "WHERE id = ? AND status = 'publishing'";
    int result = -1;

    if (event_id == 0 || !last_error || last_error[0] == '\0' ||
        strlen(last_error) > 512) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    error_length = (unsigned long)strlen(last_error);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)last_error;
    bind[0].length = &error_length;
    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = &delay;
    bind[1].is_unsigned = 1;
    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &id;
    bind[2].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = mysql_stmt_affected_rows(statement) == 1 ? 0 : 1;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int fail_outbox_event(unsigned long long event_id, const char *last_error)
{
    const char *sql =
        "UPDATE outbox_event SET status = 'failed', retry_count = retry_count + 1, "
        "last_error = ? WHERE id = ? AND status = 'publishing'";

    return update_publishing_event(event_id, sql, last_error);
}

int requeue_stale_outbox_events(unsigned int stale_after_seconds)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[1];
    unsigned int seconds = stale_after_seconds;
    my_ulonglong changed;
    const char *sql =
        "UPDATE outbox_event SET status = 'pending', retry_count = retry_count + 1, "
        "last_error = 'publisher lease expired', next_attempt_at = CURRENT_TIMESTAMP "
        "WHERE status = 'publishing' AND updated_at < "
        "TIMESTAMPADD(SECOND, -?, CURRENT_TIMESTAMP)";
    int result = -1;

    if (stale_after_seconds == 0) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_LONG;
    bind[0].buffer = &seconds;
    bind[0].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    changed = mysql_stmt_affected_rows(statement);
    result = changed > 2147483647ULL ? 2147483647 : (int)changed;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int get_outbox_metrics(OutboxMetrics *metrics)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[7];
    my_ulonglong values[7];
    const char *sql =
        "SELECT "
        "COALESCE(SUM(status = 'pending'), 0), "
        "COALESCE(SUM(status = 'pending' AND next_attempt_at <= CURRENT_TIMESTAMP), 0), "
        "COALESCE(SUM(status = 'publishing'), 0), "
        "COALESCE(SUM(status = 'published'), 0), "
        "COALESCE(SUM(status = 'failed'), 0), "
        "COALESCE(TIMESTAMPDIFF(SECOND, "
        "MIN(CASE WHEN status = 'pending' THEN created_at END), CURRENT_TIMESTAMP), 0), "
        "COALESCE(GREATEST(0, TIMESTAMPDIFF(SECOND, "
        "MIN(CASE WHEN status = 'pending' AND next_attempt_at <= CURRENT_TIMESTAMP "
        "THEN next_attempt_at END), CURRENT_TIMESTAMP)), 0) "
        "FROM outbox_event";
    int fetch_result;
    int result = -1;
    size_t index;

    if (!metrics) return -1;
    memset(metrics, 0, sizeof(*metrics));
    memset(values, 0, sizeof(values));
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    for (index = 0; index < sizeof(bind) / sizeof(bind[0]); ++index) {
        bind[index].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[index].buffer = &values[index];
        bind[index].is_unsigned = 1;
    }
    if (mysql_stmt_bind_result(statement, bind) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto done;
    fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result != 0) goto done;
    metrics->pending_count = (unsigned long long)values[0];
    metrics->ready_count = (unsigned long long)values[1];
    metrics->publishing_count = (unsigned long long)values[2];
    metrics->published_count = (unsigned long long)values[3];
    metrics->failed_count = (unsigned long long)values[4];
    metrics->oldest_pending_age_seconds = (unsigned long long)values[5];
    metrics->oldest_ready_age_seconds = (unsigned long long)values[6];
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}
