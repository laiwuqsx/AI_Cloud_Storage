#include "ai_content_repository.h"

#include <mysql/mysql.h>
#include <string.h>

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

static int valid_task(const AiContentTask *task)
{
    size_t index;

    if (!task || task->event_id == 0 || task->embedding_version == 0 ||
        strlen(task->md5) != 32) return 0;
    for (index = 0; index < 32; ++index) {
        if (!((task->md5[index] >= '0' && task->md5[index] <= '9') ||
              (task->md5[index] >= 'a' && task->md5[index] <= 'f'))) return 0;
    }
    return 1;
}

AiContentClaimResult claim_ai_content_task(const AiContentTask *task,
                                           AiContentSource *source)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *select_statement = NULL;
    MYSQL_STMT *update_statement = NULL;
    MYSQL_BIND parameter[1], result_bind[8], update_bind[3];
    unsigned long md5_length, text_lengths[3];
    char storage_key[AI_CONTENT_STORAGE_KEY_CAPACITY];
    char type[AI_CONTENT_TYPE_CAPACITY];
    char status[16];
    my_ulonglong size, generation;
    unsigned int version, is_due, retry_count;
    my_ulonglong event_id;
    const char *select_sql =
        "SELECT f.storage_key, f.type, f.size, a.status, a.embedding_version, "
        "a.next_attempt_at <= CURRENT_TIMESTAMP, a.processing_generation, "
        "a.retry_count "
        "FROM file_ai_metadata a JOIN file_info f ON f.md5 = a.md5 "
        "WHERE a.md5 = ? FOR UPDATE";
    const char *update_sql =
        "UPDATE file_ai_metadata SET status = 'processing', processing_event_id = ?, "
        "processing_started_at = CURRENT_TIMESTAMP, "
        "processing_generation = processing_generation + 1, last_error = '' "
        "WHERE md5 = ? AND embedding_version = ?";
    AiContentClaimResult result = AI_CONTENT_CLAIM_ERROR;
    int fetch_result;

    if (!valid_task(task) || !source) return AI_CONTENT_CLAIM_ERROR;
    memset(source, 0, sizeof(*source));
    memset(storage_key, 0, sizeof(storage_key));
    memset(type, 0, sizeof(type));
    memset(status, 0, sizeof(status));
    connection = connect_database();
    if (!connection || mysql_autocommit(connection, 0) != 0) goto done;
    select_statement = mysql_stmt_init(connection);
    if (!select_statement || mysql_stmt_prepare(select_statement, select_sql,
                                                (unsigned long)strlen(select_sql)) != 0)
        goto rollback;
    memset(parameter, 0, sizeof(parameter));
    md5_length = (unsigned long)strlen(task->md5);
    parameter[0].buffer_type = MYSQL_TYPE_STRING;
    parameter[0].buffer = (void *)task->md5;
    parameter[0].length = &md5_length;
    if (mysql_stmt_bind_param(select_statement, parameter) != 0 ||
        mysql_stmt_execute(select_statement) != 0) goto rollback;
    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = storage_key;
    result_bind[0].buffer_length = sizeof(storage_key) - 1;
    result_bind[0].length = &text_lengths[0];
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = type;
    result_bind[1].buffer_length = sizeof(type) - 1;
    result_bind[1].length = &text_lengths[1];
    result_bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[2].buffer = &size;
    result_bind[2].is_unsigned = 1;
    result_bind[3].buffer_type = MYSQL_TYPE_STRING;
    result_bind[3].buffer = status;
    result_bind[3].buffer_length = sizeof(status) - 1;
    result_bind[3].length = &text_lengths[2];
    result_bind[4].buffer_type = MYSQL_TYPE_LONG;
    result_bind[4].buffer = &version;
    result_bind[4].is_unsigned = 1;
    result_bind[5].buffer_type = MYSQL_TYPE_LONG;
    result_bind[5].buffer = &is_due;
    result_bind[5].is_unsigned = 1;
    result_bind[6].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[6].buffer = &generation;
    result_bind[6].is_unsigned = 1;
    result_bind[7].buffer_type = MYSQL_TYPE_LONG;
    result_bind[7].buffer = &retry_count;
    result_bind[7].is_unsigned = 1;
    if (mysql_stmt_bind_result(select_statement, result_bind) != 0 ||
        mysql_stmt_store_result(select_statement) != 0) goto rollback;
    fetch_result = mysql_stmt_fetch(select_statement);
    if (fetch_result == MYSQL_NO_DATA) {
        result = AI_CONTENT_MISSING;
        goto commit;
    }
    if (fetch_result != 0 || text_lengths[0] >= sizeof(storage_key) ||
        text_lengths[1] >= sizeof(type) || text_lengths[2] >= sizeof(status)) goto rollback;
    storage_key[text_lengths[0]] = '\0';
    type[text_lengths[1]] = '\0';
    status[text_lengths[2]] = '\0';
    if (version != task->embedding_version) {
        result = AI_CONTENT_STALE;
        goto commit;
    }
    if (strcmp(status, "ready") == 0) {
        result = AI_CONTENT_ALREADY_READY;
        goto commit;
    }
    if (strcmp(status, "processing") == 0) {
        result = AI_CONTENT_BUSY;
        goto commit;
    }
    if (strcmp(status, "failed") == 0) {
        result = AI_CONTENT_TERMINAL;
        goto commit;
    }
    if (!is_due) {
        result = AI_CONTENT_DEFERRED;
        goto commit;
    }
    if (strcmp(status, "pending") != 0) goto rollback;

    update_statement = mysql_stmt_init(connection);
    if (!update_statement || mysql_stmt_prepare(update_statement, update_sql,
                                                (unsigned long)strlen(update_sql)) != 0)
        goto rollback;
    memset(update_bind, 0, sizeof(update_bind));
    event_id = (my_ulonglong)task->event_id;
    update_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    update_bind[0].buffer = &event_id;
    update_bind[0].is_unsigned = 1;
    update_bind[1].buffer_type = MYSQL_TYPE_STRING;
    update_bind[1].buffer = (void *)task->md5;
    update_bind[1].length = &md5_length;
    update_bind[2].buffer_type = MYSQL_TYPE_LONG;
    update_bind[2].buffer = &version;
    update_bind[2].is_unsigned = 1;
    if (mysql_stmt_bind_param(update_statement, update_bind) != 0 ||
        mysql_stmt_execute(update_statement) != 0 ||
        mysql_stmt_affected_rows(update_statement) != 1) goto rollback;
    memcpy(source->storage_key, storage_key, text_lengths[0] + 1);
    memcpy(source->type, type, text_lengths[1] + 1);
    source->size = (unsigned long long)size;
    source->lease_generation = (unsigned long long)generation + 1ULL;
    source->retry_count = retry_count;
    result = AI_CONTENT_CLAIMED;

commit:
    if (mysql_commit(connection) != 0) {
        result = AI_CONTENT_CLAIM_ERROR;
        goto rollback;
    }
    goto done;

rollback:
    mysql_rollback(connection);
done:
    if (select_statement) mysql_stmt_close(select_statement);
    if (update_statement) mysql_stmt_close(update_statement);
    if (connection) mysql_close(connection);
    return result;
}

int complete_ai_content_task(const AiContentTask *task, const char *description,
                             const void *embedding, size_t embedding_bytes,
                             const char *model, unsigned int dimension,
                             unsigned long long lease_generation)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[9];
    unsigned long lengths[4];
    my_ulonglong event_id, generation;
    unsigned int version;
    const char *sql =
        "UPDATE file_ai_metadata SET status = 'ready', description = ?, embedding = ?, "
        "embedding_model = ?, embedding_dimension = ?, completed_event_id = ?, "
        "processing_event_id = NULL, retry_count = 0, last_error = '', "
        "ready_at = CURRENT_TIMESTAMP, processing_started_at = NULL "
        "WHERE md5 = ? AND embedding_version = ? AND status = 'processing' "
        "AND processing_event_id = ? AND processing_generation = ?";
    int result = -1;

    if (!valid_task(task) || !description || description[0] == '\0' ||
        strlen(description) > 65535 || !embedding ||
        dimension != AI_CONTENT_EXPECTED_DIMENSION ||
        embedding_bytes != (size_t)dimension * sizeof(float) ||
        !model || model[0] == '\0' || strlen(model) > 64 ||
        lease_generation == 0) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(description);
    lengths[1] = (unsigned long)embedding_bytes;
    lengths[2] = (unsigned long)strlen(model);
    lengths[3] = (unsigned long)strlen(task->md5);
    event_id = (my_ulonglong)task->event_id;
    generation = (my_ulonglong)lease_generation;
    version = task->embedding_version;
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)description;
    bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_BLOB;
    bind[1].buffer = (void *)embedding;
    bind[1].buffer_length = lengths[1];
    bind[1].length = &lengths[1];
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)model;
    bind[2].length = &lengths[2];
    bind[3].buffer_type = MYSQL_TYPE_LONG;
    bind[3].buffer = &dimension;
    bind[3].is_unsigned = 1;
    bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[4].buffer = &event_id;
    bind[4].is_unsigned = 1;
    bind[5].buffer_type = MYSQL_TYPE_STRING;
    bind[5].buffer = (void *)task->md5;
    bind[5].length = &lengths[3];
    bind[6].buffer_type = MYSQL_TYPE_LONG;
    bind[6].buffer = &version;
    bind[6].is_unsigned = 1;
    bind[7].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[7].buffer = &event_id;
    bind[7].is_unsigned = 1;
    bind[8].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[8].buffer = &generation;
    bind[8].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = mysql_stmt_affected_rows(statement) == 1 ? 0 : 1;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

static int update_processing_task(const AiContentTask *task, const char *error,
                                  unsigned int retry_after_seconds, int terminal,
                                  unsigned long long lease_generation)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[6];
    unsigned long lengths[2];
    my_ulonglong event_id, generation;
    unsigned int version, delay = retry_after_seconds;
    const char *retry_sql =
        "UPDATE file_ai_metadata SET status = 'pending', retry_count = retry_count + 1, "
        "last_error = ?, next_attempt_at = TIMESTAMPADD(SECOND, ?, CURRENT_TIMESTAMP), "
        "processing_event_id = NULL, processing_started_at = NULL "
        "WHERE md5 = ? AND embedding_version = ? AND status = 'processing' "
        "AND processing_event_id = ? AND processing_generation = ?";
    const char *fail_sql =
        "UPDATE file_ai_metadata SET status = 'failed', retry_count = retry_count + 1, "
        "last_error = ?, processing_event_id = NULL, processing_started_at = NULL "
        "WHERE md5 = ? AND embedding_version = ? AND status = 'processing' "
        "AND processing_event_id = ? AND processing_generation = ?";
    int result = -1;

    if (!valid_task(task) || !error || error[0] == '\0' || strlen(error) > 512 ||
        lease_generation == 0)
        return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, terminal ? fail_sql : retry_sql,
                                         (unsigned long)strlen(terminal ? fail_sql : retry_sql)) != 0)
        goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(error);
    lengths[1] = (unsigned long)strlen(task->md5);
    version = task->embedding_version;
    event_id = (my_ulonglong)task->event_id;
    generation = (my_ulonglong)lease_generation;
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)error;
    bind[0].length = &lengths[0];
    if (terminal) {
        bind[1].buffer_type = MYSQL_TYPE_STRING;
        bind[1].buffer = (void *)task->md5;
        bind[1].length = &lengths[1];
        bind[2].buffer_type = MYSQL_TYPE_LONG;
        bind[2].buffer = &version;
        bind[2].is_unsigned = 1;
        bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[3].buffer = &event_id;
        bind[3].is_unsigned = 1;
        bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[4].buffer = &generation;
        bind[4].is_unsigned = 1;
    } else {
        bind[1].buffer_type = MYSQL_TYPE_LONG;
        bind[1].buffer = &delay;
        bind[1].is_unsigned = 1;
        bind[2].buffer_type = MYSQL_TYPE_STRING;
        bind[2].buffer = (void *)task->md5;
        bind[2].length = &lengths[1];
        bind[3].buffer_type = MYSQL_TYPE_LONG;
        bind[3].buffer = &version;
        bind[3].is_unsigned = 1;
        bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[4].buffer = &event_id;
        bind[4].is_unsigned = 1;
        bind[5].buffer_type = MYSQL_TYPE_LONGLONG;
        bind[5].buffer = &generation;
        bind[5].is_unsigned = 1;
    }
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = mysql_stmt_affected_rows(statement) == 1 ? 0 : 1;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int retry_ai_content_task_after(const AiContentTask *task, const char *last_error,
                                unsigned int retry_after_seconds,
                                unsigned long long lease_generation)
{
    return update_processing_task(task, last_error, retry_after_seconds, 0,
                                  lease_generation);
}

int fail_ai_content_task(const AiContentTask *task, const char *last_error,
                         unsigned long long lease_generation)
{
    return update_processing_task(task, last_error, 0, 1, lease_generation);
}

int requeue_stale_ai_content_tasks(unsigned int stale_after_seconds)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[1];
    unsigned int seconds = stale_after_seconds;
    my_ulonglong changed;
    const char *sql =
        "UPDATE file_ai_metadata SET status = 'pending', retry_count = retry_count + 1, "
        "last_error = 'content worker lease expired', next_attempt_at = CURRENT_TIMESTAMP, "
        "processing_event_id = NULL, processing_started_at = NULL "
        "WHERE status = 'processing' AND processing_started_at < "
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
