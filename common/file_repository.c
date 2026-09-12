#include "file_repository.h"

#include <mysql/mysql.h>
#include <stdlib.h>
#include <string.h>

int list_user_files(const char *user, UserFile *files, size_t capacity, size_t *count)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND parameter[1], result_bind[7];
    unsigned long user_length, lengths[5];
    my_ulonglong size;
    unsigned int shared_status;
    size_t index = 0;
    int fetch_result;
    const char *sql =
        "SELECT u.md5, u.file_name, f.url, f.size, f.type, u.shared_status, "
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
    result_bind[5].buffer_type = MYSQL_TYPE_LONG;
    result_bind[5].buffer = &shared_status; result_bind[5].is_unsigned = 1;
    result_bind[6].buffer_type = MYSQL_TYPE_STRING;
    result_bind[6].buffer = files[0].create_time; result_bind[6].buffer_length = sizeof(files[0].create_time) - 1;
    result_bind[6].length = &lengths[4];
    if (mysql_stmt_bind_result(stmt, result_bind) != 0) goto done;

    while (index < capacity && (fetch_result = mysql_stmt_fetch(stmt)) == 0) {
        files[index].md5[32] = '\0';
        files[index].file_name[sizeof(files[index].file_name) - 1] = '\0';
        files[index].url[sizeof(files[index].url) - 1] = '\0';
        files[index].type[sizeof(files[index].type) - 1] = '\0';
        files[index].create_time[sizeof(files[index].create_time) - 1] = '\0';
        files[index].size = size;
        files[index].shared_status = shared_status;
        ++index;
        if (index == capacity) break;
        result_bind[0].buffer = files[index].md5;
        result_bind[1].buffer = files[index].file_name;
        result_bind[2].buffer = files[index].url;
        result_bind[4].buffer = files[index].type;
        result_bind[6].buffer = files[index].create_time;
    }
    if (fetch_result != MYSQL_NO_DATA && fetch_result != 0) goto done;
    *count = index;
    result = 0;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}

int claim_existing_file(const char *user, const char *md5, const char *file_name)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *check_stmt = NULL;
    MYSQL_STMT *insert_stmt = NULL;
    MYSQL_STMT *increment_stmt = NULL;
    MYSQL_BIND check_bind[1], insert_bind[3], increment_bind[1];
    unsigned long md5_length, user_length, name_length;
    const char *check_sql = "SELECT 1 FROM file_info WHERE md5 = ? FOR UPDATE";
    const char *insert_sql =
        "INSERT INTO user_file_list (user_name, md5, file_name) VALUES (?, ?, ?)";
    const char *increment_sql =
        "UPDATE file_info SET reference_count = reference_count + 1 WHERE md5 = ?";
    int result = -1;

    if (!user || !md5 || !file_name) return -1;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1",
                            getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root",
                            getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "",
                            getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") : "ai_cloud_storage",
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    check_stmt = mysql_stmt_init(conn);
    if (!check_stmt || mysql_stmt_prepare(check_stmt, check_sql, (unsigned long)strlen(check_sql)) != 0) goto rollback;
    memset(check_bind, 0, sizeof(check_bind));
    md5_length = (unsigned long)strlen(md5);
    check_bind[0].buffer_type = MYSQL_TYPE_STRING;
    check_bind[0].buffer = (void *)md5;
    check_bind[0].length = &md5_length;
    if (mysql_stmt_bind_param(check_stmt, check_bind) != 0 ||
        mysql_stmt_execute(check_stmt) != 0 || mysql_stmt_store_result(check_stmt) != 0) goto rollback;
    if (mysql_stmt_num_rows(check_stmt) == 0) {
        result = 1;
        goto rollback;
    }

    insert_stmt = mysql_stmt_init(conn);
    if (!insert_stmt || mysql_stmt_prepare(insert_stmt, insert_sql, (unsigned long)strlen(insert_sql)) != 0) goto rollback;
    memset(insert_bind, 0, sizeof(insert_bind));
    user_length = (unsigned long)strlen(user);
    name_length = (unsigned long)strlen(file_name);
    insert_bind[0].buffer_type = MYSQL_TYPE_STRING;
    insert_bind[0].buffer = (void *)user; insert_bind[0].length = &user_length;
    insert_bind[1].buffer_type = MYSQL_TYPE_STRING;
    insert_bind[1].buffer = (void *)md5; insert_bind[1].length = &md5_length;
    insert_bind[2].buffer_type = MYSQL_TYPE_STRING;
    insert_bind[2].buffer = (void *)file_name; insert_bind[2].length = &name_length;
    if (mysql_stmt_bind_param(insert_stmt, insert_bind) != 0) goto rollback;
    if (mysql_stmt_execute(insert_stmt) != 0) {
        if (mysql_stmt_errno(insert_stmt) == 1062) result = 2;
        goto rollback;
    }

    increment_stmt = mysql_stmt_init(conn);
    if (!increment_stmt ||
        mysql_stmt_prepare(increment_stmt, increment_sql, (unsigned long)strlen(increment_sql)) != 0) goto rollback;
    memset(increment_bind, 0, sizeof(increment_bind));
    increment_bind[0].buffer_type = MYSQL_TYPE_STRING;
    increment_bind[0].buffer = (void *)md5; increment_bind[0].length = &md5_length;
    if (mysql_stmt_bind_param(increment_stmt, increment_bind) != 0 ||
        mysql_stmt_execute(increment_stmt) != 0 ||
        mysql_stmt_affected_rows(increment_stmt) != 1) goto rollback;
    if (mysql_commit(conn) != 0) goto rollback;
    result = 0;
    goto done;

rollback:
    mysql_rollback(conn);
done:
    if (check_stmt) mysql_stmt_close(check_stmt);
    if (insert_stmt) mysql_stmt_close(insert_stmt);
    if (increment_stmt) mysql_stmt_close(increment_stmt);
    if (conn) mysql_close(conn);
    return result;
}

int remove_user_file(const char *user, const char *md5)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *unshare_stmt = NULL;
    MYSQL_STMT *delete_stmt = NULL;
    MYSQL_STMT *decrement_stmt = NULL;
    MYSQL_BIND unshare_bind[2], delete_bind[2], decrement_bind[1];
    unsigned long user_length, md5_length;
    const char *delete_sql = "DELETE FROM user_file_list WHERE user_name = ? AND md5 = ?";
    const char *unshare_sql = "DELETE FROM share_file_list WHERE user_name = ? AND md5 = ?";
    const char *decrement_sql =
        "UPDATE file_info SET reference_count = IF(reference_count > 0, reference_count - 1, 0) "
        "WHERE md5 = ?";
    int result = -1;

    if (!user || !md5) return -1;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1",
                            getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root",
                            getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "",
                            getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") : "ai_cloud_storage",
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    user_length = (unsigned long)strlen(user);
    md5_length = (unsigned long)strlen(md5);
    unshare_stmt = mysql_stmt_init(conn);
    if (!unshare_stmt ||
        mysql_stmt_prepare(unshare_stmt, unshare_sql, (unsigned long)strlen(unshare_sql)) != 0) goto rollback;
    memset(unshare_bind, 0, sizeof(unshare_bind));
    unshare_bind[0].buffer_type = MYSQL_TYPE_STRING;
    unshare_bind[0].buffer = (void *)user; unshare_bind[0].length = &user_length;
    unshare_bind[1].buffer_type = MYSQL_TYPE_STRING;
    unshare_bind[1].buffer = (void *)md5; unshare_bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(unshare_stmt, unshare_bind) != 0 ||
        mysql_stmt_execute(unshare_stmt) != 0) goto rollback;

    delete_stmt = mysql_stmt_init(conn);
    if (!delete_stmt ||
        mysql_stmt_prepare(delete_stmt, delete_sql, (unsigned long)strlen(delete_sql)) != 0) goto rollback;
    memset(delete_bind, 0, sizeof(delete_bind));
    delete_bind[0].buffer_type = MYSQL_TYPE_STRING;
    delete_bind[0].buffer = (void *)user; delete_bind[0].length = &user_length;
    delete_bind[1].buffer_type = MYSQL_TYPE_STRING;
    delete_bind[1].buffer = (void *)md5; delete_bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(delete_stmt, delete_bind) != 0 ||
        mysql_stmt_execute(delete_stmt) != 0) goto rollback;
    if (mysql_stmt_affected_rows(delete_stmt) == 0) {
        result = 1;
        goto rollback;
    }

    decrement_stmt = mysql_stmt_init(conn);
    if (!decrement_stmt ||
        mysql_stmt_prepare(decrement_stmt, decrement_sql, (unsigned long)strlen(decrement_sql)) != 0) goto rollback;
    memset(decrement_bind, 0, sizeof(decrement_bind));
    decrement_bind[0].buffer_type = MYSQL_TYPE_STRING;
    decrement_bind[0].buffer = (void *)md5; decrement_bind[0].length = &md5_length;
    if (mysql_stmt_bind_param(decrement_stmt, decrement_bind) != 0 ||
        mysql_stmt_execute(decrement_stmt) != 0 ||
        mysql_stmt_affected_rows(decrement_stmt) != 1) goto rollback;
    if (mysql_commit(conn) != 0) goto rollback;
    result = 0;
    goto done;

rollback:
    mysql_rollback(conn);
done:
    if (unshare_stmt) mysql_stmt_close(unshare_stmt);
    if (delete_stmt) mysql_stmt_close(delete_stmt);
    if (decrement_stmt) mysql_stmt_close(decrement_stmt);
    if (conn) mysql_close(conn);
    return result;
}

int share_user_file(const char *user, const char *md5)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *share_stmt = NULL;
    MYSQL_STMT *status_stmt = NULL;
    MYSQL_BIND share_bind[2], status_bind[2];
    unsigned long user_length, md5_length;
    const char *share_sql =
        "INSERT INTO share_file_list (user_name, md5, file_name) "
        "SELECT user_name, md5, file_name FROM user_file_list WHERE user_name = ? AND md5 = ?";
    const char *status_sql = "UPDATE user_file_list SET shared_status = 1 WHERE user_name = ? AND md5 = ?";
    int result = -1;

    if (!user || !md5) return -1;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1",
                            getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root",
                            getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "",
                            getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") : "ai_cloud_storage",
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    user_length = (unsigned long)strlen(user);
    md5_length = (unsigned long)strlen(md5);
    share_stmt = mysql_stmt_init(conn);
    if (!share_stmt ||
        mysql_stmt_prepare(share_stmt, share_sql, (unsigned long)strlen(share_sql)) != 0) goto rollback_share;
    memset(share_bind, 0, sizeof(share_bind));
    share_bind[0].buffer_type = MYSQL_TYPE_STRING;
    share_bind[0].buffer = (void *)user; share_bind[0].length = &user_length;
    share_bind[1].buffer_type = MYSQL_TYPE_STRING;
    share_bind[1].buffer = (void *)md5; share_bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(share_stmt, share_bind) != 0) goto rollback_share;
    if (mysql_stmt_execute(share_stmt) != 0) {
        if (mysql_stmt_errno(share_stmt) == 1062) result = 2;
        goto rollback_share;
    }
    if (mysql_stmt_affected_rows(share_stmt) == 0) {
        result = 1;
        goto rollback_share;
    }

    status_stmt = mysql_stmt_init(conn);
    if (!status_stmt ||
        mysql_stmt_prepare(status_stmt, status_sql, (unsigned long)strlen(status_sql)) != 0) goto rollback_share;
    memset(status_bind, 0, sizeof(status_bind));
    status_bind[0].buffer_type = MYSQL_TYPE_STRING;
    status_bind[0].buffer = (void *)user; status_bind[0].length = &user_length;
    status_bind[1].buffer_type = MYSQL_TYPE_STRING;
    status_bind[1].buffer = (void *)md5; status_bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(status_stmt, status_bind) != 0 ||
        mysql_stmt_execute(status_stmt) != 0) goto rollback_share;
    if (mysql_commit(conn) != 0) goto rollback_share;
    result = 0;
    goto done;

rollback_share:
    mysql_rollback(conn);
done:
    if (share_stmt) mysql_stmt_close(share_stmt);
    if (status_stmt) mysql_stmt_close(status_stmt);
    if (conn) mysql_close(conn);
    return result;
}

int list_shared_files(SharedFile *files, size_t capacity, size_t *count)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result_set = NULL;
    MYSQL_ROW row;
    const char *sql =
        "SELECT s.user_name, s.md5, s.file_name, f.url, f.size, f.type, s.pv, "
        "DATE_FORMAT(s.create_time, '%Y-%m-%d %H:%i:%s') "
        "FROM share_file_list s JOIN file_info f ON f.md5 = s.md5 "
        "ORDER BY s.create_time DESC LIMIT 100";
    size_t index = 0;
    int result = -1;

    if (!files || !count) return -1;
    *count = 0;
    memset(files, 0, capacity * sizeof(*files));
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1",
                            getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root",
                            getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "",
                            getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") : "ai_cloud_storage",
                            3306, NULL, 0)) goto done;
    if (mysql_query(conn, sql) != 0) goto done;
    result_set = mysql_store_result(conn);
    if (!result_set) goto done;
    while (index < capacity && (row = mysql_fetch_row(result_set)) != NULL) {
        snprintf(files[index].user_name, sizeof(files[index].user_name), "%s", row[0] ? row[0] : "");
        snprintf(files[index].md5, sizeof(files[index].md5), "%s", row[1] ? row[1] : "");
        snprintf(files[index].file_name, sizeof(files[index].file_name), "%s", row[2] ? row[2] : "");
        snprintf(files[index].url, sizeof(files[index].url), "%s", row[3] ? row[3] : "");
        files[index].size = row[4] ? strtoull(row[4], NULL, 10) : 0;
        snprintf(files[index].type, sizeof(files[index].type), "%s", row[5] ? row[5] : "");
        files[index].pv = row[6] ? strtoul(row[6], NULL, 10) : 0;
        snprintf(files[index].create_time, sizeof(files[index].create_time), "%s", row[7] ? row[7] : "");
        ++index;
    }
    *count = index;
    result = 0;

done:
    if (result_set) mysql_free_result(result_set);
    if (conn) mysql_close(conn);
    return result;
}
