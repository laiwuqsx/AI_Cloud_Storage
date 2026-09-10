#include "file_repository.h"

#include <mysql/mysql.h>
#include <stdlib.h>
#include <string.h>

int list_user_files(const char *user, UserFile *files, size_t capacity, size_t *count)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND parameter[1], result_bind[6];
    unsigned long user_length, lengths[5];
    my_ulonglong size;
    size_t index = 0;
    int fetch_result;
    const char *sql =
        "SELECT u.md5, u.file_name, f.url, f.size, f.type, "
        "DATE_FORMAT(u.create_time, '%Y-%m-%d %H:%i:%s') "
        "FROM user_file_list u JOIN file_info f ON f.md5 = u.md5 "
        "WHERE u.user_name = ? ORDER BY u.create_time DESC LIMIT 100";
    int result = -1;

    if (!user || !files || !count) return -1;
    *count = 0;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1",
                            getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root",
                            getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "",
                            getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") : "ai_cloud_storage",
                            3306, NULL, 0)) goto done;
    stmt = mysql_stmt_init(conn);
    if (!stmt || mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) goto done;

    memset(parameter, 0, sizeof(parameter));
    user_length = (unsigned long)strlen(user);
    parameter[0].buffer_type = MYSQL_TYPE_STRING;
    parameter[0].buffer = (void *)user;
    parameter[0].length = &user_length;
    if (mysql_stmt_bind_param(stmt, parameter) != 0 || mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_store_result(stmt) != 0) goto done;

    memset(result_bind, 0, sizeof(result_bind));
    memset(files, 0, capacity * sizeof(*files));
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = files[0].md5; result_bind[0].buffer_length = sizeof(files[0].md5) - 1;
    result_bind[0].length = &lengths[0];
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = files[0].file_name; result_bind[1].buffer_length = sizeof(files[0].file_name) - 1;
    result_bind[1].length = &lengths[1];
    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = files[0].url; result_bind[2].buffer_length = sizeof(files[0].url) - 1;
    result_bind[2].length = &lengths[2];
    result_bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[3].buffer = &size; result_bind[3].is_unsigned = 1;
    result_bind[4].buffer_type = MYSQL_TYPE_STRING;
    result_bind[4].buffer = files[0].type; result_bind[4].buffer_length = sizeof(files[0].type) - 1;
    result_bind[4].length = &lengths[3];
    result_bind[5].buffer_type = MYSQL_TYPE_STRING;
    result_bind[5].buffer = files[0].create_time; result_bind[5].buffer_length = sizeof(files[0].create_time) - 1;
    result_bind[5].length = &lengths[4];
    if (mysql_stmt_bind_result(stmt, result_bind) != 0) goto done;

    while (index < capacity && (fetch_result = mysql_stmt_fetch(stmt)) == 0) {
        files[index].md5[32] = '\0';
        files[index].file_name[sizeof(files[index].file_name) - 1] = '\0';
        files[index].url[sizeof(files[index].url) - 1] = '\0';
        files[index].type[sizeof(files[index].type) - 1] = '\0';
        files[index].create_time[sizeof(files[index].create_time) - 1] = '\0';
        files[index].size = size;
        ++index;
        if (index == capacity) break;
        result_bind[0].buffer = files[index].md5;
        result_bind[1].buffer = files[index].file_name;
        result_bind[2].buffer = files[index].url;
        result_bind[4].buffer = files[index].type;
        result_bind[5].buffer = files[index].create_time;
    }
    if (fetch_result != MYSQL_NO_DATA && fetch_result != 0) goto done;
    *count = index;
    result = 0;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}
