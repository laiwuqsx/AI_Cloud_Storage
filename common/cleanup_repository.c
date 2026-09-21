#include "cleanup_repository.h"

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

static int valid_text(const char *value, size_t maximum)
{
    size_t length;

    if (!value) return 0;
    length = strlen(value);
    return length > 0 && length <= maximum;
}

int enqueue_storage_cleanup_in_transaction(MYSQL *connection,
                                           const char *storage_key,
                                           const char *reason,
                                           const char *last_error)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[3];
    unsigned long lengths[3];
    const char *sql =
        "INSERT INTO storage_cleanup_job "
        "(storage_key, reason, status, retry_count, last_error) "
        "VALUES (?, ?, 'pending', 0, ?) "
        "ON DUPLICATE KEY UPDATE reason = VALUES(reason), status = 'pending', "
        "last_error = VALUES(last_error), completed_at = NULL";
    int result = -1;

    if (!connection || !valid_text(storage_key, 256) || !valid_text(reason, 64) ||
        !valid_text(last_error, 512)) return -1;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;

    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(storage_key);
    lengths[1] = (unsigned long)strlen(reason);
    lengths[2] = (unsigned long)strlen(last_error);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)storage_key;
    bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)reason;
    bind[1].length = &lengths[1];
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)last_error;
    bind[2].length = &lengths[2];
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

int enqueue_storage_cleanup(const char *storage_key, const char *reason,
                            const char *last_error)
{
    MYSQL *connection = connect_database();
    int result;

    if (!connection) return -1;
    result = enqueue_storage_cleanup_in_transaction(connection, storage_key,
                                                    reason, last_error);
    mysql_close(connection);
    return result;
}

int claim_next_storage_cleanup(StorageCleanupJob *job)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *select_statement = NULL;
    MYSQL_STMT *update_statement = NULL;
    MYSQL_BIND result_bind[4], update_bind[1];
    unsigned long text_lengths[2];
    my_ulonglong id;
    unsigned int retry_count;
    char storage_key[CLEANUP_STORAGE_KEY_CAPACITY];
    char reason[CLEANUP_REASON_CAPACITY];
    const char *select_sql =
        "SELECT id, storage_key, reason, retry_count FROM storage_cleanup_job "
        "WHERE status = 'pending' ORDER BY updated_at, id LIMIT 1 "
        "FOR UPDATE SKIP LOCKED";
    const char *update_sql =
        "UPDATE storage_cleanup_job SET status = 'running' WHERE id = ?";
    int fetch_result;
    int result = -1;

    if (!job) return -1;
    memset(job, 0, sizeof(*job));
    connection = connect_database();
    if (!connection || mysql_autocommit(connection, 0) != 0) goto done;
    select_statement = mysql_stmt_init(connection);
    if (!select_statement || mysql_stmt_prepare(select_statement, select_sql,
                                                (unsigned long)strlen(select_sql)) != 0 ||
        mysql_stmt_execute(select_statement) != 0) goto rollback;

    memset(storage_key, 0, sizeof(storage_key));
    memset(reason, 0, sizeof(reason));
    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[0].buffer = &id;
    result_bind[0].is_unsigned = 1;
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = storage_key;
    result_bind[1].buffer_length = sizeof(storage_key) - 1;
    result_bind[1].length = &text_lengths[0];
    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = reason;
    result_bind[2].buffer_length = sizeof(reason) - 1;
    result_bind[2].length = &text_lengths[1];
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
    if (fetch_result != 0 || text_lengths[0] >= sizeof(storage_key) ||
        text_lengths[1] >= sizeof(reason)) goto rollback;
    storage_key[text_lengths[0]] = '\0';
    reason[text_lengths[1]] = '\0';

    update_statement = mysql_stmt_init(connection);
    if (!update_statement || mysql_stmt_prepare(update_statement, update_sql,
                                                (unsigned long)strlen(update_sql)) != 0) {
        goto rollback;
    }
    memset(update_bind, 0, sizeof(update_bind));
    update_bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
    update_bind[0].buffer = &id;
    update_bind[0].is_unsigned = 1;
    if (mysql_stmt_bind_param(update_statement, update_bind) != 0 ||
        mysql_stmt_execute(update_statement) != 0 ||
        mysql_stmt_affected_rows(update_statement) != 1 ||
        mysql_commit(connection) != 0) goto rollback;

    job->id = (unsigned long long)id;
    memcpy(job->storage_key, storage_key, text_lengths[0] + 1);
    memcpy(job->reason, reason, text_lengths[1] + 1);
    job->retry_count = retry_count;
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

static int update_running_job(unsigned long long job_id, const char *sql,
                              const char *last_error)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[2];
    my_ulonglong id = (my_ulonglong)job_id;
    unsigned long error_length = 0;
    int parameter_count = last_error ? 2 : 1;
    int result = -1;

    if (job_id == 0 || (last_error && !valid_text(last_error, 512))) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;
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
        mysql_stmt_param_count(statement) != (unsigned long)parameter_count ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = mysql_stmt_affected_rows(statement) == 1 ? 0 : 1;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int complete_storage_cleanup(unsigned long long job_id)
{
    const char *sql =
        "UPDATE storage_cleanup_job SET status = 'done', last_error = '', "
        "completed_at = CURRENT_TIMESTAMP WHERE id = ? AND status = 'running'";

    return update_running_job(job_id, sql, NULL);
}

int retry_storage_cleanup(unsigned long long job_id, const char *last_error)
{
    const char *sql =
        "UPDATE storage_cleanup_job SET status = 'pending', retry_count = retry_count + 1, "
        "last_error = ?, completed_at = NULL WHERE id = ? AND status = 'running'";

    return update_running_job(job_id, sql, last_error);
}

int requeue_stale_storage_cleanups(unsigned int stale_after_seconds)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[1];
    unsigned int seconds = stale_after_seconds;
    const char *sql =
        "UPDATE storage_cleanup_job SET status = 'pending', retry_count = retry_count + 1, "
        "last_error = 'worker lease expired' "
        "WHERE status = 'running' AND updated_at < "
        "TIMESTAMPADD(SECOND, -?, CURRENT_TIMESTAMP)";
    my_ulonglong changed;
    int result = -1;

    if (stale_after_seconds == 0) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;
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
