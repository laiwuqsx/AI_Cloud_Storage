#include "ai_search_repository.h"

#include <mysql/mysql.h>
#include <string.h>

#include "runtime_config.h"

int filter_owned_ai_search_results(const char *user_name,
                                   const FaissSearchResult *candidates,
                                   size_t candidate_count,
                                   AiSearchFile *files, size_t capacity,
                                   size_t *file_count)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND parameter[3], row[6];
    unsigned long user_length, lengths[4];
    my_ulonglong candidate_id = 0, user_file_id = 0, size = 0;
    char md5[33], file_name[129], type[33], create_time[32];
    const char *sql =
        "SELECT u.id, u.md5, u.file_name, f.size, f.type, "
        "DATE_FORMAT(u.create_time, '%Y-%m-%d %H:%i:%s') "
        "FROM user_file_list u JOIN file_info f ON f.md5 = u.md5 "
        "JOIN user_ai_index_entry a ON a.user_name = u.user_name "
        "AND a.md5 = u.md5 AND a.user_file_id = u.id "
        "WHERE u.user_name = ? AND u.id = ? AND a.vector_id = ? "
        "AND a.status = 'indexed'";
    size_t candidate_index, output_index = 0;
    int fetch_result;
    int result = -1;

    if (!user_name || !candidates || !files || !file_count || capacity == 0)
        return -1;
    *file_count = 0;
    memset(files, 0, capacity * sizeof(*files));
    connection = mysql_init(NULL);
    if (!connection || !mysql_real_connect(connection,
            runtime_config_get("MYSQL_HOST", "127.0.0.1"),
            runtime_config_get("MYSQL_USER", "root"),
            runtime_config_get("MYSQL_PASSWORD", ""),
            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
            3306, NULL, 0)) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql,
                                         (unsigned long)strlen(sql)) != 0) goto done;
    memset(parameter, 0, sizeof(parameter));
    user_length = (unsigned long)strlen(user_name);
    parameter[0].buffer_type = MYSQL_TYPE_STRING;
    parameter[0].buffer = (void *)user_name;
    parameter[0].length = &user_length;
    parameter[1].buffer_type = MYSQL_TYPE_LONGLONG;
    parameter[1].buffer = &candidate_id;
    parameter[1].is_unsigned = 1;
    parameter[2].buffer_type = MYSQL_TYPE_LONGLONG;
    parameter[2].buffer = &candidate_id;
    parameter[2].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, parameter) != 0) goto done;
    memset(row, 0, sizeof(row));
    row[0].buffer_type = MYSQL_TYPE_LONGLONG;
    row[0].buffer = &user_file_id;
    row[0].is_unsigned = 1;
    row[1].buffer_type = MYSQL_TYPE_STRING;
    row[1].buffer = md5;
    row[1].buffer_length = sizeof(md5) - 1;
    row[1].length = &lengths[0];
    row[2].buffer_type = MYSQL_TYPE_STRING;
    row[2].buffer = file_name;
    row[2].buffer_length = sizeof(file_name) - 1;
    row[2].length = &lengths[1];
    row[3].buffer_type = MYSQL_TYPE_LONGLONG;
    row[3].buffer = &size;
    row[3].is_unsigned = 1;
    row[4].buffer_type = MYSQL_TYPE_STRING;
    row[4].buffer = type;
    row[4].buffer_length = sizeof(type) - 1;
    row[4].length = &lengths[2];
    row[5].buffer_type = MYSQL_TYPE_STRING;
    row[5].buffer = create_time;
    row[5].buffer_length = sizeof(create_time) - 1;
    row[5].length = &lengths[3];
    if (mysql_stmt_bind_result(statement, row) != 0) goto done;

    for (candidate_index = 0;
         candidate_index < candidate_count && output_index < capacity;
         ++candidate_index) {
        candidate_id = (my_ulonglong)candidates[candidate_index].vector_id;
        memset(md5, 0, sizeof(md5));
        memset(file_name, 0, sizeof(file_name));
        memset(type, 0, sizeof(type));
        memset(create_time, 0, sizeof(create_time));
        if (mysql_stmt_execute(statement) != 0 ||
            mysql_stmt_store_result(statement) != 0) goto done;
        fetch_result = mysql_stmt_fetch(statement);
        if (fetch_result == MYSQL_NO_DATA) {
            mysql_stmt_free_result(statement);
            continue;
        }
        if (fetch_result != 0 || lengths[0] >= sizeof(md5) ||
            lengths[1] >= sizeof(file_name) || lengths[2] >= sizeof(type) ||
            lengths[3] >= sizeof(create_time) || user_file_id != candidate_id) goto done;
        md5[lengths[0]] = '\0';
        file_name[lengths[1]] = '\0';
        type[lengths[2]] = '\0';
        create_time[lengths[3]] = '\0';
        files[output_index].user_file_id = (unsigned long long)user_file_id;
        files[output_index].size = (unsigned long long)size;
        files[output_index].score = candidates[candidate_index].score;
        memcpy(files[output_index].md5, md5, lengths[0] + 1);
        memcpy(files[output_index].file_name, file_name, lengths[1] + 1);
        memcpy(files[output_index].type, type, lengths[2] + 1);
        memcpy(files[output_index].create_time, create_time, lengths[3] + 1);
        ++output_index;
        mysql_stmt_free_result(statement);
    }
    *file_count = output_index;
    result = 0;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}
