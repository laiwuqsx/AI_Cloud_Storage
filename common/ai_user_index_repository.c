#include "ai_user_index_repository.h"

#include <mysql/mysql.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ai_index_event.h"
#include "runtime_config.h"

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

static int valid_task(const AiUserIndexTask *task)
{
    AiIndexEvent event;

    if (!task || task->event_id == 0 ||
        (task->type != AI_INDEX_EVENT_USER_FILE_ADDED &&
         task->type != AI_INDEX_EVENT_USER_FILE_REMOVED)) return 0;
    event.type = task->type;
    event.md5 = task->md5;
    event.user_name = task->user_name;
    event.user_file_id = task->user_file_id;
    event.embedding_version = task->embedding_version;
    return validate_ai_index_event(&event);
}

static int relation_exists(MYSQL *connection, const AiUserIndexTask *task,
                           int *exists)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[3];
    unsigned long lengths[2];
    my_ulonglong relation_id = (my_ulonglong)task->user_file_id;
    const char *sql =
        "SELECT id FROM user_file_list WHERE id = ? AND user_name = ? AND md5 = ?";
    int fetch_result;
    int result = -1;

    *exists = 0;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(task->user_name);
    lengths[1] = (unsigned long)strlen(task->md5);
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &relation_id;
    bind[0].is_unsigned = 1;
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)task->user_name;
    bind[1].length = &lengths[0];
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)task->md5;
    bind[2].length = &lengths[1];
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto done;
    fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result != 0 && fetch_result != MYSQL_NO_DATA) goto done;
    *exists = fetch_result == 0;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

static int update_simple_state(MYSQL *connection, const AiUserIndexTask *task,
                               const char *status, const char *last_error)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[5];
    unsigned long lengths[4];
    my_ulonglong relation_id = (my_ulonglong)task->user_file_id;
    const char *sql =
        "UPDATE user_ai_index_entry SET status = ?, last_error = ?, "
        "next_attempt_at = CURRENT_TIMESTAMP "
        "WHERE user_name = ? AND md5 = ? AND user_file_id = ? "
        "AND processing_event_id IS NULL";
    int result = -1;

    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(status);
    lengths[1] = (unsigned long)strlen(last_error);
    lengths[2] = (unsigned long)strlen(task->user_name);
    lengths[3] = (unsigned long)strlen(task->md5);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)status;
    bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)last_error;
    bind[1].length = &lengths[1];
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)task->user_name;
    bind[2].length = &lengths[2];
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = (void *)task->md5;
    bind[3].length = &lengths[3];
    bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[4].buffer = &relation_id;
    bind[4].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0 ||
        mysql_stmt_affected_rows(statement) > 1) goto done;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

static int load_ready_embedding(MYSQL *connection, const AiUserIndexTask *task,
                                AiUserIndexSource *source, int *content_state)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND parameter[1], result_bind[4];
    unsigned long md5_length, status_length, embedding_length = 0;
    char status[16] = "";
    unsigned int version = 0, dimension = 0;
    bool embedding_is_null = false;
    const char *sql =
        "SELECT status, embedding_version, COALESCE(embedding_dimension, 0), "
        "embedding FROM file_ai_metadata WHERE md5 = ?";
    int fetch_result;
    int result = -1;

    *content_state = 0;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(parameter, 0, sizeof(parameter));
    md5_length = (unsigned long)strlen(task->md5);
    parameter[0].buffer_type = MYSQL_TYPE_STRING;
    parameter[0].buffer = (void *)task->md5;
    parameter[0].length = &md5_length;
    if (mysql_stmt_bind_param(statement, parameter) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = status;
    result_bind[0].buffer_length = sizeof(status) - 1;
    result_bind[0].length = &status_length;
    result_bind[1].buffer_type = MYSQL_TYPE_LONG;
    result_bind[1].buffer = &version;
    result_bind[1].is_unsigned = 1;
    result_bind[2].buffer_type = MYSQL_TYPE_LONG;
    result_bind[2].buffer = &dimension;
    result_bind[2].is_unsigned = 1;
    result_bind[3].buffer_type = MYSQL_TYPE_BLOB;
    result_bind[3].buffer = source->embedding;
    result_bind[3].buffer_length = sizeof(source->embedding);
    result_bind[3].length = &embedding_length;
    result_bind[3].is_null = &embedding_is_null;
    if (mysql_stmt_bind_result(statement, result_bind) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto done;
    fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result == MYSQL_NO_DATA) {
        result = 0;
        goto done;
    }
    if (fetch_result != 0 || status_length >= sizeof(status)) goto done;
    status[status_length] = '\0';
    if (version != task->embedding_version) {
        *content_state = 3;
    } else if (strcmp(status, "failed") == 0) {
        *content_state = 2;
    } else if (strcmp(status, "ready") == 0) {
        if (embedding_is_null || dimension != AI_CONTENT_EXPECTED_DIMENSION ||
            embedding_length != sizeof(source->embedding)) {
            *content_state = 2;
        } else {
            *content_state = 1;
        }
    }
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

static int claim_row(MYSQL *connection, const AiUserIndexTask *task,
                     const char *status, unsigned long long *lease_generation)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[5];
    unsigned long lengths[3];
    my_ulonglong event_id = (my_ulonglong)task->event_id;
    my_ulonglong relation_id = (my_ulonglong)task->user_file_id;
    const char *sql =
        "UPDATE user_ai_index_entry SET status = ?, processing_event_id = ?, "
        "processing_started_at = CURRENT_TIMESTAMP, "
        "processing_generation = processing_generation + 1, last_error = '' "
        "WHERE user_name = ? AND md5 = ? AND user_file_id = ? "
        "AND processing_event_id IS NULL";
    int result = -1;

    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(status);
    lengths[1] = (unsigned long)strlen(task->user_name);
    lengths[2] = (unsigned long)strlen(task->md5);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)status;
    bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[1].buffer = &event_id;
    bind[1].is_unsigned = 1;
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)task->user_name;
    bind[2].length = &lengths[1];
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = (void *)task->md5;
    bind[3].length = &lengths[2];
    bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[4].buffer = &relation_id;
    bind[4].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0 ||
        mysql_stmt_affected_rows(statement) != 1) goto done;
    ++*lease_generation;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

AiUserIndexClaimResult claim_ai_user_index_task(const AiUserIndexTask *task,
                                                AiUserIndexSource *source)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND parameter[2], result_bind[9];
    unsigned long lengths[2], status_length;
    char status[24] = "";
    my_ulonglong relation_id, generation, index_version, vector_id;
    unsigned int version, has_lease, retry_count, is_due;
    int fetch_result, relation_found, content_state;
    const char *sql =
        "SELECT user_file_id, status, embedding_version, "
        "processing_event_id IS NOT NULL, processing_generation, retry_count, "
        "next_attempt_at <= CURRENT_TIMESTAMP, index_version, "
        "COALESCE(vector_id, 0) "
        "FROM user_ai_index_entry WHERE user_name = ? AND md5 = ? FOR UPDATE";
    AiUserIndexClaimResult result = AI_USER_INDEX_CLAIM_ERROR;

    if (!valid_task(task) || !source) return AI_USER_INDEX_CLAIM_ERROR;
    memset(source, 0, sizeof(*source));
    connection = connect_database();
    if (!connection || mysql_autocommit(connection, 0) != 0) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto rollback;
    memset(parameter, 0, sizeof(parameter));
    lengths[0] = (unsigned long)strlen(task->user_name);
    lengths[1] = (unsigned long)strlen(task->md5);
    parameter[0].buffer_type = MYSQL_TYPE_STRING;
    parameter[0].buffer = (void *)task->user_name;
    parameter[0].length = &lengths[0];
    parameter[1].buffer_type = MYSQL_TYPE_STRING;
    parameter[1].buffer = (void *)task->md5;
    parameter[1].length = &lengths[1];
    if (mysql_stmt_bind_param(statement, parameter) != 0 ||
        mysql_stmt_execute(statement) != 0) goto rollback;
    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &relation_id;
    result_bind[0].is_unsigned = 1;
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = status;
    result_bind[1].buffer_length = sizeof(status) - 1;
    result_bind[1].length = &status_length;
    result_bind[2].buffer_type = MYSQL_TYPE_LONG;
    result_bind[2].buffer = &version;
    result_bind[2].is_unsigned = 1;
    result_bind[3].buffer_type = MYSQL_TYPE_LONG;
    result_bind[3].buffer = &has_lease;
    result_bind[3].is_unsigned = 1;
    result_bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[4].buffer = &generation;
    result_bind[4].is_unsigned = 1;
    result_bind[5].buffer_type = MYSQL_TYPE_LONG;
    result_bind[5].buffer = &retry_count;
    result_bind[5].is_unsigned = 1;
    result_bind[6].buffer_type = MYSQL_TYPE_LONG;
    result_bind[6].buffer = &is_due;
    result_bind[6].is_unsigned = 1;
    result_bind[7].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[7].buffer = &index_version;
    result_bind[7].is_unsigned = 1;
    result_bind[8].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[8].buffer = &vector_id;
    result_bind[8].is_unsigned = 1;
    if (mysql_stmt_bind_result(statement, result_bind) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto rollback;
    fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result == MYSQL_NO_DATA) {
        result = AI_USER_INDEX_MISSING;
        goto commit;
    }
    if (fetch_result != 0 || status_length >= sizeof(status)) goto rollback;
    status[status_length] = '\0';
    if ((unsigned long long)relation_id != task->user_file_id ||
        version != task->embedding_version) {
        result = AI_USER_INDEX_STALE;
        goto commit;
    }
    if (has_lease) {
        result = AI_USER_INDEX_BUSY;
        goto commit;
    }
    if (strcmp(status, "failed") == 0) {
        result = AI_USER_INDEX_TERMINAL;
        goto commit;
    }
    if (!is_due) {
        result = AI_USER_INDEX_DEFERRED;
        goto commit;
    }
    if (relation_exists(connection, task, &relation_found) != 0) goto rollback;
    source->vector_id = task->user_file_id;
    source->current_index_version = (unsigned long long)index_version;
    source->lease_generation = (unsigned long long)generation;
    source->retry_count = retry_count;

    if (task->type == AI_INDEX_EVENT_USER_FILE_REMOVED) {
        if (relation_found) {
            result = AI_USER_INDEX_STALE;
            goto commit;
        }
        if (strcmp(status, "removing") != 0) goto rollback;
        if (claim_row(connection, task, "removing",
                      &source->lease_generation) != 0) goto rollback;
        result = AI_USER_INDEX_CLAIMED;
        goto commit;
    }

    if (!relation_found) {
        result = AI_USER_INDEX_STALE;
        goto commit;
    }
    if (strcmp(status, "indexed") == 0) {
        result = AI_USER_INDEX_ALREADY_APPLIED;
        goto commit;
    }
    if (strcmp(status, "pending") != 0 &&
        strcmp(status, "waiting_content") != 0) goto rollback;
    if (load_ready_embedding(connection, task, source, &content_state) != 0)
        goto rollback;
    if (content_state == 3) {
        result = AI_USER_INDEX_STALE;
        goto commit;
    }
    if (content_state == 2) {
        if (update_simple_state(connection, task, "failed",
                                "content embedding unavailable") != 0) goto rollback;
        result = AI_USER_INDEX_TERMINAL;
        goto commit;
    }
    if (content_state == 0) {
        if (update_simple_state(connection, task, "waiting_content",
                                "waiting for content embedding") != 0) goto rollback;
        result = AI_USER_INDEX_WAITING_CONTENT;
        goto commit;
    }
    if (claim_row(connection, task, "indexing",
                  &source->lease_generation) != 0) goto rollback;
    result = AI_USER_INDEX_CLAIMED;

commit:
    if (mysql_commit(connection) != 0) {
        result = AI_USER_INDEX_CLAIM_ERROR;
        goto rollback;
    }
    goto done;

rollback:
    mysql_rollback(connection);
done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int load_ai_user_index_snapshot(const AiUserIndexTask *task,
                                unsigned long long lease_generation,
                                AiUserIndexSnapshot *snapshot)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *lease_statement = NULL, *snapshot_statement = NULL;
    MYSQL_BIND lease_parameter[5], lease_result[1];
    MYSQL_BIND snapshot_parameter[1], row[3];
    unsigned long lengths[3], embedding_length = 0;
    float embedding[AI_CONTENT_EXPECTED_DIMENSION];
    my_ulonglong event_id, relation_id, generation, lease_version = 0;
    my_ulonglong vector_id = 0, index_version = 0, maximum_version = 0;
    bool embedding_is_null = false;
    const char *lease_sql =
        "SELECT index_version FROM user_ai_index_entry WHERE user_name = ? AND md5 = ? "
        "AND user_file_id = ? AND processing_event_id = ? "
        "AND processing_generation = ? AND status IN ('indexing', 'removing') "
        "FOR UPDATE";
    const char *snapshot_sql =
        "SELECT a.user_file_id, m.embedding, a.index_version "
        "FROM user_ai_index_entry a "
        "JOIN user_file_list u ON u.id = a.user_file_id "
        "AND u.user_name = a.user_name AND u.md5 = a.md5 "
        "JOIN file_ai_metadata m ON m.md5 = a.md5 "
        "AND m.embedding_version = a.embedding_version "
        "WHERE a.user_name = ? AND a.status IN ('indexed', 'indexing') "
        "AND m.status = 'ready' AND m.embedding_dimension = 1024 "
        "AND OCTET_LENGTH(m.embedding) = 4096 ORDER BY a.user_file_id";
    size_t capacity, position = 0;
    int fetch_result;
    int result = -1;

    if (!valid_task(task) || lease_generation == 0 || !snapshot) return -1;
    memset(snapshot, 0, sizeof(*snapshot));
    connection = connect_database();
    if (!connection || mysql_autocommit(connection, 0) != 0) goto done;
    lease_statement = mysql_stmt_init(connection);
    if (!lease_statement || mysql_stmt_prepare(lease_statement, lease_sql,
            (unsigned long)strlen(lease_sql)) != 0) goto rollback;
    memset(lease_parameter, 0, sizeof(lease_parameter));
    lengths[0] = (unsigned long)strlen(task->user_name);
    lengths[1] = (unsigned long)strlen(task->md5);
    relation_id = (my_ulonglong)task->user_file_id;
    event_id = (my_ulonglong)task->event_id;
    generation = (my_ulonglong)lease_generation;
    lease_parameter[0].buffer_type = MYSQL_TYPE_STRING;
    lease_parameter[0].buffer = (void *)task->user_name;
    lease_parameter[0].length = &lengths[0];
    lease_parameter[1].buffer_type = MYSQL_TYPE_STRING;
    lease_parameter[1].buffer = (void *)task->md5;
    lease_parameter[1].length = &lengths[1];
    lease_parameter[2].buffer_type = MYSQL_TYPE_LONGLONG;
    lease_parameter[2].buffer = &relation_id;
    lease_parameter[2].is_unsigned = 1;
    lease_parameter[3].buffer_type = MYSQL_TYPE_LONGLONG;
    lease_parameter[3].buffer = &event_id;
    lease_parameter[3].is_unsigned = 1;
    lease_parameter[4].buffer_type = MYSQL_TYPE_LONGLONG;
    lease_parameter[4].buffer = &generation;
    lease_parameter[4].is_unsigned = 1;
    memset(lease_result, 0, sizeof(lease_result));
    lease_result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    lease_result[0].buffer = &lease_version;
    lease_result[0].is_unsigned = 1;
    if (mysql_stmt_bind_param(lease_statement, lease_parameter) != 0 ||
        mysql_stmt_execute(lease_statement) != 0 ||
        mysql_stmt_bind_result(lease_statement, lease_result) != 0 ||
        mysql_stmt_store_result(lease_statement) != 0 ||
        mysql_stmt_fetch(lease_statement) != 0) goto rollback;
    maximum_version = lease_version;

    snapshot_statement = mysql_stmt_init(connection);
    if (!snapshot_statement || mysql_stmt_prepare(snapshot_statement, snapshot_sql,
            (unsigned long)strlen(snapshot_sql)) != 0) goto rollback;
    memset(snapshot_parameter, 0, sizeof(snapshot_parameter));
    snapshot_parameter[0].buffer_type = MYSQL_TYPE_STRING;
    snapshot_parameter[0].buffer = (void *)task->user_name;
    snapshot_parameter[0].length = &lengths[0];
    if (mysql_stmt_bind_param(snapshot_statement, snapshot_parameter) != 0 ||
        mysql_stmt_execute(snapshot_statement) != 0 ||
        mysql_stmt_store_result(snapshot_statement) != 0) goto rollback;
    capacity = (size_t)mysql_stmt_num_rows(snapshot_statement);
    if (capacity > 0) {
        snapshot->vectors = calloc(capacity, sizeof(*snapshot->vectors));
        if (!snapshot->vectors) goto rollback;
    }
    memset(row, 0, sizeof(row));
    row[0].buffer_type = MYSQL_TYPE_LONGLONG;
    row[0].buffer = &vector_id;
    row[0].is_unsigned = 1;
    row[1].buffer_type = MYSQL_TYPE_BLOB;
    row[1].buffer = embedding;
    row[1].buffer_length = sizeof(embedding);
    row[1].length = &embedding_length;
    row[1].is_null = &embedding_is_null;
    row[2].buffer_type = MYSQL_TYPE_LONGLONG;
    row[2].buffer = &index_version;
    row[2].is_unsigned = 1;
    if (mysql_stmt_bind_result(snapshot_statement, row) != 0) goto rollback;
    while ((fetch_result = mysql_stmt_fetch(snapshot_statement)) == 0) {
        if (position >= capacity || embedding_is_null ||
            embedding_length != sizeof(snapshot->vectors[position].embedding) ||
            vector_id == 0) goto rollback;
        memcpy(snapshot->vectors[position].embedding, embedding, sizeof(embedding));
        snapshot->vectors[position].vector_id = (unsigned long long)vector_id;
        if ((unsigned long long)index_version > maximum_version)
            maximum_version = index_version;
        ++position;
    }
    if (fetch_result != MYSQL_NO_DATA || position != capacity ||
        maximum_version == ULLONG_MAX) goto rollback;
    snapshot->count = position;
    snapshot->next_index_version = (unsigned long long)maximum_version + 1ULL;
    if (snapshot->next_index_version == 0) snapshot->next_index_version = 1;
    if (mysql_commit(connection) != 0) goto rollback;
    result = 0;
    goto done;

rollback:
    mysql_rollback(connection);
    free_ai_user_index_snapshot(snapshot);
done:
    if (snapshot_statement) mysql_stmt_close(snapshot_statement);
    if (lease_statement) mysql_stmt_close(lease_statement);
    if (connection) mysql_close(connection);
    return result;
}

void free_ai_user_index_snapshot(AiUserIndexSnapshot *snapshot)
{
    if (!snapshot) return;
    free(snapshot->vectors);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int update_user_index_versions(MYSQL *connection, const char *user_name,
                                      unsigned long long index_version)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[2];
    unsigned long user_length;
    my_ulonglong version = (my_ulonglong)index_version;
    const char *sql =
        "UPDATE user_ai_index_entry SET index_version = ? "
        "WHERE user_name = ? AND status = 'indexed'";
    int result = -1;

    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    user_length = (unsigned long)strlen(user_name);
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &version;
    bind[0].is_unsigned = 1;
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)user_name;
    bind[1].length = &user_length;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

int complete_ai_user_index_add(const AiUserIndexTask *task,
                               unsigned long long index_version,
                               unsigned long long lease_generation)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[9];
    unsigned long lengths[2];
    my_ulonglong vector_id, new_index_version, event_id, relation_id, generation;
    unsigned int version;
    const char *sql =
        "UPDATE user_ai_index_entry SET status = 'indexed', vector_id = ?, "
        "index_version = ?, completed_event_id = ?, processing_event_id = NULL, "
        "processing_started_at = NULL, retry_count = 0, last_error = '', "
        "indexed_at = CURRENT_TIMESTAMP "
        "WHERE user_name = ? AND md5 = ? AND user_file_id = ? "
        "AND embedding_version = ? AND status = 'indexing' "
        "AND processing_event_id = ? AND processing_generation = ?";
    int result = -1;

    if (!valid_task(task) || task->type != AI_INDEX_EVENT_USER_FILE_ADDED ||
        index_version == 0 || lease_generation == 0) return -1;
    connection = connect_database();
    if (!connection || mysql_autocommit(connection, 0) != 0) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    vector_id = relation_id = (my_ulonglong)task->user_file_id;
    new_index_version = (my_ulonglong)index_version;
    event_id = (my_ulonglong)task->event_id;
    generation = (my_ulonglong)lease_generation;
    version = task->embedding_version;
    lengths[0] = (unsigned long)strlen(task->user_name);
    lengths[1] = (unsigned long)strlen(task->md5);
    bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[0].buffer = &vector_id; bind[0].is_unsigned = 1;
    bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[1].buffer = &new_index_version; bind[1].is_unsigned = 1;
    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &event_id; bind[2].is_unsigned = 1;
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = (void *)task->user_name; bind[3].length = &lengths[0];
    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = (void *)task->md5; bind[4].length = &lengths[1];
    bind[5].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[5].buffer = &relation_id; bind[5].is_unsigned = 1;
    bind[6].buffer_type = MYSQL_TYPE_LONG;
    bind[6].buffer = &version; bind[6].is_unsigned = 1;
    bind[7].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[7].buffer = &event_id; bind[7].is_unsigned = 1;
    bind[8].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[8].buffer = &generation; bind[8].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto rollback;
    if (mysql_stmt_affected_rows(statement) != 1) {
        result = 1;
        goto rollback;
    }
    if (update_user_index_versions(connection, task->user_name,
                                   index_version) != 0 ||
        mysql_commit(connection) != 0) goto rollback;
    result = 0;
    goto done;

rollback:
    mysql_rollback(connection);

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int complete_ai_user_index_remove(const AiUserIndexTask *task,
                                  unsigned long long index_version,
                                  unsigned long long lease_generation)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[5];
    unsigned long lengths[2];
    my_ulonglong relation_id, event_id, generation;
    const char *sql =
        "DELETE FROM user_ai_index_entry WHERE user_name = ? AND md5 = ? "
        "AND user_file_id = ? AND status = 'removing' "
        "AND processing_event_id = ? AND processing_generation = ?";
    int result = -1;

    if (!valid_task(task) || task->type != AI_INDEX_EVENT_USER_FILE_REMOVED ||
        index_version == 0 || lease_generation == 0) return -1;
    connection = connect_database();
    if (!connection || mysql_autocommit(connection, 0) != 0) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(task->user_name);
    lengths[1] = (unsigned long)strlen(task->md5);
    relation_id = (my_ulonglong)task->user_file_id;
    event_id = (my_ulonglong)task->event_id;
    generation = (my_ulonglong)lease_generation;
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)task->user_name; bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)task->md5; bind[1].length = &lengths[1];
    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &relation_id; bind[2].is_unsigned = 1;
    bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[3].buffer = &event_id; bind[3].is_unsigned = 1;
    bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[4].buffer = &generation; bind[4].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto rollback;
    if (mysql_stmt_affected_rows(statement) != 1) {
        result = 1;
        goto rollback;
    }
    if (update_user_index_versions(connection, task->user_name,
                                   index_version) != 0 ||
        mysql_commit(connection) != 0) goto rollback;
    result = 0;
    goto done;

rollback:
    mysql_rollback(connection);

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

static int update_processing_task(const AiUserIndexTask *task, const char *error,
                                  unsigned int retry_after_seconds, int terminal,
                                  unsigned long long lease_generation)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[8];
    unsigned long lengths[4];
    my_ulonglong relation_id, event_id, generation;
    unsigned int delay = retry_after_seconds;
    const char *next_status = task->type == AI_INDEX_EVENT_USER_FILE_ADDED
        ? "pending" : "removing";
    const char *retry_sql =
        "UPDATE user_ai_index_entry SET status = ?, retry_count = retry_count + 1, "
        "last_error = ?, next_attempt_at = TIMESTAMPADD(SECOND, ?, CURRENT_TIMESTAMP), "
        "processing_event_id = NULL, processing_started_at = NULL "
        "WHERE user_name = ? AND md5 = ? AND user_file_id = ? "
        "AND processing_event_id = ? AND processing_generation = ?";
    const char *fail_sql =
        "UPDATE user_ai_index_entry SET status = 'failed', "
        "retry_count = retry_count + 1, last_error = ?, "
        "processing_event_id = NULL, processing_started_at = NULL "
        "WHERE user_name = ? AND md5 = ? AND user_file_id = ? "
        "AND processing_event_id = ? AND processing_generation = ?";
    int result = -1;

    if (!valid_task(task) || !error || error[0] == '\0' || strlen(error) > 512 ||
        lease_generation == 0) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, terminal ? fail_sql : retry_sql,
            (unsigned long)strlen(terminal ? fail_sql : retry_sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(next_status);
    lengths[1] = (unsigned long)strlen(error);
    lengths[2] = (unsigned long)strlen(task->user_name);
    lengths[3] = (unsigned long)strlen(task->md5);
    relation_id = (my_ulonglong)task->user_file_id;
    event_id = (my_ulonglong)task->event_id;
    generation = (my_ulonglong)lease_generation;
    if (terminal) {
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = (void *)error; bind[0].length = &lengths[1];
        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = (void *)task->user_name; bind[1].length = &lengths[2];
        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = (void *)task->md5; bind[2].length = &lengths[3];
        bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[3].buffer = &relation_id; bind[3].is_unsigned = 1;
        bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[4].buffer = &event_id; bind[4].is_unsigned = 1;
        bind[5].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[5].buffer = &generation; bind[5].is_unsigned = 1;
    } else {
        bind[0].buffer_type = MYSQL_TYPE_STRING;
        bind[0].buffer = (void *)next_status; bind[0].length = &lengths[0];
        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = (void *)error; bind[1].length = &lengths[1];
        bind[2].buffer_type = MYSQL_TYPE_LONG;
        bind[2].buffer = &delay; bind[2].is_unsigned = 1;
        bind[3].buffer_type = MYSQL_TYPE_STRING;
        bind[3].buffer = (void *)task->user_name; bind[3].length = &lengths[2];
        bind[4].buffer_type = MYSQL_TYPE_STRING;
        bind[4].buffer = (void *)task->md5; bind[4].length = &lengths[3];
        bind[5].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[5].buffer = &relation_id; bind[5].is_unsigned = 1;
        bind[6].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[6].buffer = &event_id; bind[6].is_unsigned = 1;
        bind[7].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[7].buffer = &generation; bind[7].is_unsigned = 1;
    }
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = mysql_stmt_affected_rows(statement) == 1 ? 0 : 1;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int retry_ai_user_index_task_after(const AiUserIndexTask *task,
                                   const char *last_error,
                                   unsigned int retry_after_seconds,
                                   unsigned long long lease_generation)
{
    return update_processing_task(task, last_error, retry_after_seconds, 0,
                                  lease_generation);
}

int fail_ai_user_index_task(const AiUserIndexTask *task, const char *last_error,
                            unsigned long long lease_generation)
{
    return update_processing_task(task, last_error, 0, 1, lease_generation);
}

int requeue_stale_ai_user_index_tasks(unsigned int stale_after_seconds)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[1];
    unsigned int seconds = stale_after_seconds;
    my_ulonglong changed;
    const char *sql =
        "UPDATE user_ai_index_entry SET "
        "status = CASE WHEN status = 'indexing' THEN 'pending' ELSE 'removing' END, "
        "retry_count = retry_count + 1, last_error = 'index worker lease expired', "
        "next_attempt_at = CURRENT_TIMESTAMP, processing_event_id = NULL, "
        "processing_started_at = NULL WHERE processing_event_id IS NOT NULL "
        "AND status IN ('indexing', 'removing') AND processing_started_at < "
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
