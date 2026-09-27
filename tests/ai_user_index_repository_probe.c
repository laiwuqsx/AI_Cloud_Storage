#include <ctype.h>
#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_user_index_repository.h"
#include "ai_search_repository.h"
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

static int execute_sql(const char *sql)
{
    MYSQL *connection = connect_database();
    int result;

    if (!connection) return 0;
    result = mysql_query(connection, sql) == 0;
    mysql_close(connection);
    return result;
}

static unsigned long long scalar_sql(const char *sql)
{
    MYSQL *connection = connect_database();
    MYSQL_RES *result_set = NULL;
    MYSQL_ROW row;
    unsigned long long value = 0;

    if (!connection) return 0;
    if (mysql_query(connection, sql) == 0 &&
        (result_set = mysql_store_result(connection)) != NULL &&
        (row = mysql_fetch_row(result_set)) != NULL && row[0])
        value = strtoull(row[0], NULL, 10);
    if (result_set) mysql_free_result(result_set);
    mysql_close(connection);
    return value;
}

int main(int argc, char **argv)
{
    char user_name[33], sql[2048];
    unsigned long long relation_id, generation_one, generation_two;
    AiUserIndexTask add_task, remove_task;
    AiUserIndexSource source;
    AiUserIndexSnapshot snapshot;
    FaissSearchResult candidates[2];
    AiSearchFile search_files[2];
    size_t search_count;
    int recovered;
    int ok = 0;

    if (argc != 2 || !valid_md5(argv[1])) return 2;
    runtime_config_init();
    snprintf(user_name, sizeof(user_name), "ai_probe_%.8s", argv[1]);
    snprintf(sql, sizeof(sql), "DELETE FROM user_info WHERE user_name = '%s'",
             user_name);
    if (!execute_sql(sql)) goto cleanup;
    snprintf(sql, sizeof(sql), "DELETE FROM file_info WHERE md5 = '%s'", argv[1]);
    if (!execute_sql(sql)) goto cleanup;
    snprintf(sql, sizeof(sql),
        "INSERT INTO user_info (user_name, nick_name, password, salt) VALUES "
        "('%s', 'nick_%s', '00000000000000000000000000000000', "
        "'11111111111111111111111111111111')", user_name, user_name);
    if (!execute_sql(sql)) goto cleanup;
    snprintf(sql, sizeof(sql),
        "INSERT INTO file_info (md5, storage_key, url, size, type, reference_count) "
        "VALUES ('%s', 'probe/user-index', 'http://storage/probe', 42, 'jpg', 1)",
        argv[1]);
    if (!execute_sql(sql)) goto cleanup;
    snprintf(sql, sizeof(sql),
        "INSERT INTO user_file_list (user_name, md5, file_name) "
        "VALUES ('%s', '%s', 'probe.jpg')", user_name, argv[1]);
    if (!execute_sql(sql)) goto cleanup;
    snprintf(sql, sizeof(sql),
        "SELECT id FROM user_file_list WHERE user_name = '%s' AND md5 = '%s'",
        user_name, argv[1]);
    relation_id = scalar_sql(sql);
    if (relation_id == 0) goto cleanup;
    snprintf(sql, sizeof(sql),
        "INSERT INTO file_ai_metadata (md5, status, embedding_version) "
        "VALUES ('%s', 'pending', 1)", argv[1]);
    if (!execute_sql(sql)) goto cleanup;
    snprintf(sql, sizeof(sql),
        "INSERT INTO user_ai_index_entry "
        "(user_name, md5, user_file_id, status, embedding_version) "
        "VALUES ('%s', '%s', %llu, 'pending', 1)",
        user_name, argv[1], relation_id);
    if (!execute_sql(sql)) goto cleanup;

    memset(&add_task, 0, sizeof(add_task));
    add_task.event_id = 700001;
    add_task.type = AI_INDEX_EVENT_USER_FILE_ADDED;
    snprintf(add_task.user_name, sizeof(add_task.user_name), "%s", user_name);
    snprintf(add_task.md5, sizeof(add_task.md5), "%s", argv[1]);
    add_task.user_file_id = relation_id;
    add_task.embedding_version = 1;
    if (claim_ai_user_index_task(&add_task, &source) !=
        AI_USER_INDEX_WAITING_CONTENT) goto cleanup;
    snprintf(sql, sizeof(sql),
        "UPDATE file_ai_metadata SET status = 'ready', "
        "embedding = REPEAT(CHAR(1), 4096), embedding_model = 'probe-model', "
        "embedding_dimension = 1024, ready_at = CURRENT_TIMESTAMP "
        "WHERE md5 = '%s'", argv[1]);
    if (!execute_sql(sql) ||
        claim_ai_user_index_task(&add_task, &source) != AI_USER_INDEX_CLAIMED ||
        source.vector_id != relation_id || source.lease_generation == 0)
        goto cleanup;
    if (load_ai_user_index_snapshot(&add_task, source.lease_generation,
                                    &snapshot) != 0 || snapshot.count != 1 ||
        snapshot.vectors[0].vector_id != relation_id ||
        snapshot.next_index_version != 1) {
        free_ai_user_index_snapshot(&snapshot);
        goto cleanup;
    }
    free_ai_user_index_snapshot(&snapshot);
    generation_one = source.lease_generation;
    if (claim_ai_user_index_task(&add_task, &source) != AI_USER_INDEX_BUSY ||
        retry_ai_user_index_task_after(&add_task, "probe retry", 3600,
                                       generation_one) != 0) goto cleanup;
    if (claim_ai_user_index_task(&add_task, &source) != AI_USER_INDEX_DEFERRED)
        goto cleanup;
    snprintf(sql, sizeof(sql),
        "UPDATE user_ai_index_entry SET next_attempt_at = CURRENT_TIMESTAMP "
        "WHERE user_name = '%s' AND md5 = '%s'", user_name, argv[1]);
    if (!execute_sql(sql) ||
        claim_ai_user_index_task(&add_task, &source) != AI_USER_INDEX_CLAIMED)
        goto cleanup;
    generation_two = source.lease_generation;
    snprintf(sql, sizeof(sql),
        "UPDATE user_ai_index_entry SET processing_started_at = "
        "TIMESTAMPADD(SECOND, -600, CURRENT_TIMESTAMP) "
        "WHERE user_name = '%s' AND md5 = '%s'", user_name, argv[1]);
    if (!execute_sql(sql)) goto cleanup;
    recovered = requeue_stale_ai_user_index_tasks(300);
    if (recovered < 1 ||
        claim_ai_user_index_task(&add_task, &source) != AI_USER_INDEX_CLAIMED ||
        complete_ai_user_index_add(&add_task, 1, generation_two) != 1 ||
        complete_ai_user_index_add(&add_task, 1, source.lease_generation) != 0 ||
        claim_ai_user_index_task(&add_task, &source) !=
            AI_USER_INDEX_ALREADY_APPLIED) goto cleanup;
    memset(candidates, 0, sizeof(candidates));
    candidates[0].vector_id = relation_id;
    candidates[0].score = 0.9F;
    candidates[1].vector_id = relation_id + 999999ULL;
    candidates[1].score = 0.8F;
    if (filter_owned_ai_search_results(user_name, candidates, 2, search_files,
                                       2, &search_count) != 0 ||
        search_count != 1 || search_files[0].user_file_id != relation_id ||
        strcmp(search_files[0].md5, argv[1]) != 0 ||
        search_files[0].score != candidates[0].score) goto cleanup;

    snprintf(sql, sizeof(sql),
        "DELETE FROM user_file_list WHERE id = %llu", relation_id);
    if (!execute_sql(sql)) goto cleanup;
    snprintf(sql, sizeof(sql),
        "UPDATE user_ai_index_entry SET status = 'removing', "
        "processing_started_at = NULL, processing_event_id = NULL, "
        "completed_event_id = NULL, processing_generation = processing_generation + 1 "
        "WHERE user_name = '%s' AND md5 = '%s' AND user_file_id = %llu",
        user_name, argv[1], relation_id);
    if (!execute_sql(sql)) goto cleanup;
    remove_task = add_task;
    remove_task.event_id = 700002;
    remove_task.type = AI_INDEX_EVENT_USER_FILE_REMOVED;
    if (claim_ai_user_index_task(&remove_task, &source) != AI_USER_INDEX_CLAIMED ||
        source.vector_id != relation_id ||
        complete_ai_user_index_remove(&remove_task, 2,
                                      source.lease_generation) != 0 ||
        claim_ai_user_index_task(&remove_task, &source) != AI_USER_INDEX_MISSING)
        goto cleanup;
    ok = 1;

cleanup:
    snprintf(sql, sizeof(sql), "DELETE FROM user_info WHERE user_name = '%s'",
             user_name);
    if (!execute_sql(sql)) ok = 0;
    snprintf(sql, sizeof(sql), "DELETE FROM file_info WHERE md5 = '%s'", argv[1]);
    if (!execute_sql(sql)) ok = 0;
    if (!ok) {
        fprintf(stderr, "ai user index repository probe failed\n");
        return 1;
    }
    puts("ai user index repository probe passed");
    return 0;
}
