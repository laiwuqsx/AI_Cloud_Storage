#include <ctype.h>
#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cleanup_repository.h"

static int valid_suffix(const char *value)
{
    size_t index;

    if (!value || strlen(value) != 32) return 0;
    for (index = 0; index < 32; ++index) {
        if (!isxdigit((unsigned char)value[index])) return 0;
    }
    return 1;
}

static MYSQL *connect_database(void)
{
    MYSQL *connection = mysql_init(NULL);

    if (!connection) return NULL;
    if (!mysql_real_connect(connection,
                            getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1",
                            getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root",
                            getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "",
                            getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") :
                                                       "ai_cloud_storage",
                            3306, NULL, 0)) {
        mysql_close(connection);
        return NULL;
    }
    return connection;
}

static int run_storage_key_sql(const char *format, const char *storage_key)
{
    MYSQL *connection = connect_database();
    char sql[1024];
    int result;

    if (!connection) return 0;
    snprintf(sql, sizeof(sql), format, storage_key);
    result = mysql_query(connection, sql) == 0;
    mysql_close(connection);
    return result;
}

static int verify_state(const char *storage_key, const char *expected_status,
                        const char *expected_retries, const char *expected_reason)
{
    MYSQL *connection = NULL;
    MYSQL_RES *result_set = NULL;
    MYSQL_ROW row;
    char sql[1024];
    int verified = 0;

    connection = connect_database();
    if (!connection) return 0;
    snprintf(sql, sizeof(sql),
             "SELECT status, retry_count, reason, COUNT(*) "
             "FROM storage_cleanup_job WHERE storage_key = '%s' "
             "GROUP BY status, retry_count, reason",
             storage_key);
    if (mysql_query(connection, sql) == 0 &&
        (result_set = mysql_store_result(connection)) != NULL &&
        (row = mysql_fetch_row(result_set)) != NULL &&
        row[0] && strcmp(row[0], expected_status) == 0 &&
        row[1] && strcmp(row[1], expected_retries) == 0 &&
        row[2] && strcmp(row[2], expected_reason) == 0 &&
        row[3] && strcmp(row[3], "1") == 0) {
        verified = 1;
    }
    if (result_set) mysql_free_result(result_set);
    mysql_close(connection);
    return verified;
}

int main(int argc, char **argv)
{
    StorageCleanupJob first, second, third;
    char storage_key[128];
    int verified;

    if (argc != 2 || !valid_suffix(argv[1])) return 2;
    snprintf(storage_key, sizeof(storage_key), "probe/cleanup/%s", argv[1]);
    if (enqueue_storage_cleanup(storage_key, "probe_initial", "initial failure") != 0 ||
        enqueue_storage_cleanup(storage_key, "probe_refresh", "refreshed failure") != 0) {
        fprintf(stderr, "cleanup repository probe could not enqueue\n");
        return 1;
    }
    if (claim_next_storage_cleanup(&first) != 0 ||
        strcmp(first.storage_key, storage_key) != 0 || first.retry_count != 0 ||
        strcmp(first.reason, "probe_refresh") != 0) {
        fprintf(stderr, "cleanup repository probe could not claim initial job\n");
        return 1;
    }
    if (retry_storage_cleanup_after(first.id, "probe delayed retry", 3600) != 0 ||
        claim_next_storage_cleanup(&second) != 1 ||
        !run_storage_key_sql("UPDATE storage_cleanup_job SET next_attempt_at = "
                             "CURRENT_TIMESTAMP WHERE storage_key = '%s'", storage_key) ||
        claim_next_storage_cleanup(&second) != 0 || second.id != first.id ||
        second.retry_count != 1 || strcmp(second.storage_key, storage_key) != 0) {
        fprintf(stderr, "cleanup repository probe could not retry job\n");
        return 1;
    }
    if (fail_storage_cleanup(second.id, "probe terminal failure") != 0 ||
        !verify_state(storage_key, "failed", "2", "probe_refresh")) {
        fprintf(stderr, "cleanup repository probe could not fail job\n");
        return 1;
    }
    if (enqueue_storage_cleanup(storage_key, "probe_reopen", "new cleanup request") != 0 ||
        claim_next_storage_cleanup(&third) != 0 || third.id != first.id ||
        third.retry_count != 0 || strcmp(third.reason, "probe_reopen") != 0 ||
        complete_storage_cleanup(third.id) != 0) {
        fprintf(stderr, "cleanup repository probe could not complete job\n");
        return 1;
    }
    verified = verify_state(storage_key, "done", "0", "probe_reopen");
    if (!run_storage_key_sql("DELETE FROM storage_cleanup_job WHERE storage_key = '%s'",
                             storage_key)) verified = 0;
    if (!verified) {
        fprintf(stderr, "cleanup repository probe database state mismatch\n");
        return 1;
    }
    puts("cleanup repository probe passed");
    return 0;
}
