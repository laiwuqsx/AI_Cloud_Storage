#include <ctype.h>
#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_content_repository.h"
#include "runtime_config.h"

static int valid_md5(const char *value)
{
    size_t index;

    if (!value || strlen(value) != 32) return 0;
    for (index = 0; index < 32; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return 0;
    }
    return 1;
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

static int execute_md5_sql(const char *format, const char *md5)
{
    MYSQL *connection = connect_database();
    char sql[1024];
    int length;
    int result;

    if (!connection) return 0;
    length = snprintf(sql, sizeof(sql), format, md5);
    result = length > 0 && (size_t)length < sizeof(sql) &&
             mysql_query(connection, sql) == 0;
    mysql_close(connection);
    return result;
}

static int verify_ready(const char *md5, unsigned long long event_id)
{
    MYSQL *connection = connect_database();
    MYSQL_RES *result_set = NULL;
    MYSQL_ROW row;
    char sql[512];
    int verified = 0;

    if (!connection) return 0;
    snprintf(sql, sizeof(sql),
             "SELECT status, embedding_dimension, embedding_model, OCTET_LENGTH(embedding), "
             "completed_event_id FROM file_ai_metadata WHERE md5 = '%s'", md5);
    if (mysql_query(connection, sql) == 0 &&
        (result_set = mysql_store_result(connection)) != NULL &&
        (row = mysql_fetch_row(result_set)) != NULL && row[0] && row[1] && row[2] &&
        row[3] && row[4] && strcmp(row[0], "ready") == 0 &&
        strcmp(row[1], "1024") == 0 && strcmp(row[2], "probe-model") == 0 &&
        strcmp(row[3], "4096") == 0 && strtoull(row[4], NULL, 10) == event_id) {
        verified = 1;
    }
    if (result_set) mysql_free_result(result_set);
    mysql_close(connection);
    return verified;
}

int main(int argc, char **argv)
{
    AiContentTask first, second, third;
    AiContentSource source;
    unsigned long long first_generation, second_generation, third_generation;
    float embedding[AI_CONTENT_EXPECTED_DIMENSION];
    unsigned long long base_event;
    size_t index;
    int recovered;
    int ok = 0;

    if (argc != 2 || !valid_md5(argv[1])) return 2;
    runtime_config_init();
    base_event = 1000000ULL + strtoull(argv[1], NULL, 16) % 1000000000ULL;
    first.event_id = base_event;
    memcpy(first.md5, argv[1], 33);
    first.embedding_version = 1;
    if (!execute_md5_sql("DELETE FROM file_info WHERE md5 = '%s'", argv[1]) ||
        !execute_md5_sql("INSERT INTO file_info "
            "(md5, storage_key, url, size, type, reference_count) VALUES "
            "('%s', 'probe/ai-content', 'http://storage/probe', 42, 'jpg', 1)",
            argv[1]) ||
        !execute_md5_sql("INSERT INTO file_ai_metadata "
            "(md5, status, embedding_version) VALUES ('%s', 'pending', 1)",
            argv[1])) goto cleanup;

    if (claim_ai_content_task(&first, &source) != AI_CONTENT_CLAIMED ||
        strcmp(source.storage_key, "probe/ai-content") != 0 ||
        strcmp(source.type, "jpg") != 0 || source.size != 42) goto cleanup;
    first_generation = source.lease_generation;
    if (first_generation == 0 ||
        claim_ai_content_task(&first, &source) != AI_CONTENT_BUSY) goto cleanup;
    for (index = 0; index < AI_CONTENT_EXPECTED_DIMENSION; ++index) {
        embedding[index] = (float)index / (float)AI_CONTENT_EXPECTED_DIMENSION;
    }
    if (complete_ai_content_task(&first, "probe description", embedding,
                                 sizeof(embedding), "probe-model",
                                 AI_CONTENT_EXPECTED_DIMENSION,
                                 first_generation) != 0 ||
        !verify_ready(argv[1], first.event_id)) goto cleanup;
    second = first;
    ++second.event_id;
    if (claim_ai_content_task(&second, &source) != AI_CONTENT_ALREADY_READY) goto cleanup;

    if (!execute_md5_sql("UPDATE file_ai_metadata SET status = 'pending', "
            "description = NULL, embedding = NULL, embedding_model = '', "
            "embedding_dimension = NULL, processing_event_id = NULL, "
            "completed_event_id = NULL, ready_at = NULL, retry_count = 0, "
            "next_attempt_at = CURRENT_TIMESTAMP WHERE md5 = '%s'", argv[1]) ||
        claim_ai_content_task(&second, &source) != AI_CONTENT_CLAIMED ||
        (second_generation = source.lease_generation) == 0 ||
        retry_ai_content_task_after(&second, "temporary probe failure", 3600,
                                    second_generation) != 0 ||
        claim_ai_content_task(&second, &source) != AI_CONTENT_DEFERRED ||
        !execute_md5_sql("UPDATE file_ai_metadata SET next_attempt_at = CURRENT_TIMESTAMP "
                         "WHERE md5 = '%s'", argv[1])) goto cleanup;

    third = second;
    ++third.event_id;
    if (claim_ai_content_task(&third, &source) != AI_CONTENT_CLAIMED ||
        (third_generation = source.lease_generation) == 0 ||
        fail_ai_content_task(&third, "terminal probe failure", third_generation) != 0 ||
        claim_ai_content_task(&third, &source) != AI_CONTENT_TERMINAL ||
        !execute_md5_sql("UPDATE file_ai_metadata SET status = 'processing', "
            "processing_event_id = 999, processing_started_at = "
            "TIMESTAMPADD(SECOND, -600, CURRENT_TIMESTAMP) WHERE md5 = '%s'",
            argv[1])) goto cleanup;
    recovered = requeue_stale_ai_content_tasks(300);
    if (recovered < 1 ||
        claim_ai_content_task(&third, &source) != AI_CONTENT_CLAIMED ||
        complete_ai_content_task(&third, "zombie result", embedding,
                                 sizeof(embedding), "probe-model",
                                 AI_CONTENT_EXPECTED_DIMENSION,
                                 third_generation) != 1) goto cleanup;
    ok = 1;

cleanup:
    if (!execute_md5_sql("DELETE FROM file_info WHERE md5 = '%s'", argv[1])) ok = 0;
    if (!ok) {
        fprintf(stderr, "ai content repository probe failed\n");
        return 1;
    }
    puts("ai content repository probe passed");
    return 0;
}
