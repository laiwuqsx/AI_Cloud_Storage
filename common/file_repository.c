#include "file_repository.h"

#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime_config.h"

int list_user_files(const char *user, UserFile *files, size_t capacity, size_t *count)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND parameter[1], result_bind[7];
    unsigned long user_length, lengths[5];
    char md5[33], file_name[129], url[513], type[33], create_time[20];
    my_ulonglong size;
    unsigned int shared_status;
    size_t index = 0;
    int fetch_result = MYSQL_NO_DATA;
    const char *sql =
        "SELECT u.md5, u.file_name, f.url, f.size, f.type, "
        "CASE WHEN EXISTS ("
        "SELECT 1 FROM share_file_list s WHERE s.user_file_id = u.id "
        "AND (s.expires_at IS NULL OR s.expires_at > UTC_TIMESTAMP())"
        ") THEN 1 ELSE 0 END, "
        "DATE_FORMAT(u.create_time, '%Y-%m-%d %H:%i:%s') "
        "FROM user_file_list u JOIN file_info f ON f.md5 = u.md5 "
        "WHERE u.user_name = ? ORDER BY u.create_time DESC LIMIT 100";
    int result = -1;

    if (!user || !files || !count) return -1;
    *count = 0;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
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
    memset(md5, 0, sizeof(md5));
    memset(file_name, 0, sizeof(file_name));
    memset(url, 0, sizeof(url));
    memset(type, 0, sizeof(type));
    memset(create_time, 0, sizeof(create_time));
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = md5; result_bind[0].buffer_length = sizeof(md5) - 1;
    result_bind[0].length = &lengths[0];
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = file_name; result_bind[1].buffer_length = sizeof(file_name) - 1;
    result_bind[1].length = &lengths[1];
    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = url; result_bind[2].buffer_length = sizeof(url) - 1;
    result_bind[2].length = &lengths[2];
    result_bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[3].buffer = &size; result_bind[3].is_unsigned = 1;
    result_bind[4].buffer_type = MYSQL_TYPE_STRING;
    result_bind[4].buffer = type; result_bind[4].buffer_length = sizeof(type) - 1;
    result_bind[4].length = &lengths[3];
    result_bind[5].buffer_type = MYSQL_TYPE_LONG;
    result_bind[5].buffer = &shared_status; result_bind[5].is_unsigned = 1;
    result_bind[6].buffer_type = MYSQL_TYPE_STRING;
    result_bind[6].buffer = create_time; result_bind[6].buffer_length = sizeof(create_time) - 1;
    result_bind[6].length = &lengths[4];
    if (mysql_stmt_bind_result(stmt, result_bind) != 0) goto done;

    while (index < capacity && (fetch_result = mysql_stmt_fetch(stmt)) == 0) {
        md5[lengths[0]] = '\0';
        file_name[lengths[1]] = '\0';
        url[lengths[2]] = '\0';
        type[lengths[3]] = '\0';
        create_time[lengths[4]] = '\0';
        memcpy(files[index].md5, md5, lengths[0] + 1);
        memcpy(files[index].file_name, file_name, lengths[1] + 1);
        memcpy(files[index].url, url, lengths[2] + 1);
        memcpy(files[index].type, type, lengths[3] + 1);
        memcpy(files[index].create_time, create_time, lengths[4] + 1);
        files[index].size = size;
        files[index].shared_status = shared_status;
        ++index;
    }
    if (fetch_result != MYSQL_NO_DATA && fetch_result != 0) goto done;
    *count = index;
    result = 0;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}

static int valid_new_file_record(const NewFileRecord *record)
{
    if (!record || !record->user_name || !record->md5 || !record->file_name ||
        !record->storage_key || !record->url || !record->type) return 0;
    return strlen(record->user_name) > 0 && strlen(record->user_name) <= 32 &&
           strlen(record->md5) == 32 &&
           strlen(record->file_name) > 0 && strlen(record->file_name) <= 128 &&
           strlen(record->storage_key) > 0 && strlen(record->storage_key) <= 256 &&
           strlen(record->url) > 0 && strlen(record->url) <= 512 &&
           strlen(record->type) <= 32;
}

RecordNewFileResult record_new_file_upload(const NewFileRecord *record)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *file_stmt = NULL;
    MYSQL_STMT *user_file_stmt = NULL;
    MYSQL_BIND file_bind[5], user_file_bind[3];
    unsigned long md5_length, storage_key_length, url_length, type_length;
    unsigned long user_length, file_name_length;
    my_ulonglong file_size;
    const char *file_sql =
        "INSERT INTO file_info "
        "(md5, storage_key, url, size, type, reference_count) VALUES (?, ?, ?, ?, ?, 1)";
    const char *user_file_sql =
        "INSERT INTO user_file_list (user_name, md5, file_name) VALUES (?, ?, ?)";
    RecordNewFileResult result = RECORD_NEW_FILE_DATABASE_FAILURE;

    if (!valid_new_file_record(record)) return RECORD_NEW_FILE_INVALID_ARGUMENT;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    md5_length = (unsigned long)strlen(record->md5);
    storage_key_length = (unsigned long)strlen(record->storage_key);
    url_length = (unsigned long)strlen(record->url);
    type_length = (unsigned long)strlen(record->type);
    file_size = (my_ulonglong)record->size;

    file_stmt = mysql_stmt_init(conn);
    if (!file_stmt ||
        mysql_stmt_prepare(file_stmt, file_sql, (unsigned long)strlen(file_sql)) != 0) goto rollback;
    memset(file_bind, 0, sizeof(file_bind));
    file_bind[0].buffer_type = MYSQL_TYPE_STRING;
    file_bind[0].buffer = (void *)record->md5; file_bind[0].length = &md5_length;
    file_bind[1].buffer_type = MYSQL_TYPE_STRING;
    file_bind[1].buffer = (void *)record->storage_key; file_bind[1].length = &storage_key_length;
    file_bind[2].buffer_type = MYSQL_TYPE_STRING;
    file_bind[2].buffer = (void *)record->url; file_bind[2].length = &url_length;
    file_bind[3].buffer_type = MYSQL_TYPE_LONGLONG;
    file_bind[3].buffer = &file_size; file_bind[3].is_unsigned = 1;
    file_bind[4].buffer_type = MYSQL_TYPE_STRING;
    file_bind[4].buffer = (void *)record->type; file_bind[4].length = &type_length;
    if (mysql_stmt_bind_param(file_stmt, file_bind) != 0) goto rollback;
    if (mysql_stmt_execute(file_stmt) != 0) {
        if (mysql_stmt_errno(file_stmt) == 1062) result = RECORD_NEW_FILE_PHYSICAL_CONFLICT;
        goto rollback;
    }
    if (mysql_stmt_affected_rows(file_stmt) != 1) goto rollback;

    user_length = (unsigned long)strlen(record->user_name);
    file_name_length = (unsigned long)strlen(record->file_name);
    user_file_stmt = mysql_stmt_init(conn);
    if (!user_file_stmt ||
        mysql_stmt_prepare(user_file_stmt, user_file_sql,
                           (unsigned long)strlen(user_file_sql)) != 0) goto rollback;
    memset(user_file_bind, 0, sizeof(user_file_bind));
    user_file_bind[0].buffer_type = MYSQL_TYPE_STRING;
    user_file_bind[0].buffer = (void *)record->user_name; user_file_bind[0].length = &user_length;
    user_file_bind[1].buffer_type = MYSQL_TYPE_STRING;
    user_file_bind[1].buffer = (void *)record->md5; user_file_bind[1].length = &md5_length;
    user_file_bind[2].buffer_type = MYSQL_TYPE_STRING;
    user_file_bind[2].buffer = (void *)record->file_name; user_file_bind[2].length = &file_name_length;
    if (mysql_stmt_bind_param(user_file_stmt, user_file_bind) != 0 ||
        mysql_stmt_execute(user_file_stmt) != 0 ||
        mysql_stmt_affected_rows(user_file_stmt) != 1) goto rollback;

    if (mysql_commit(conn) != 0) {
        result = RECORD_NEW_FILE_COMMIT_UNKNOWN;
        goto done;
    }
    result = RECORD_NEW_FILE_CREATED;
    goto done;

rollback:
    mysql_rollback(conn);
done:
    if (file_stmt) mysql_stmt_close(file_stmt);
    if (user_file_stmt) mysql_stmt_close(user_file_stmt);
    if (conn) mysql_close(conn);
    return result;
}

int confirm_new_file_upload(const char *user_name, const char *md5, const char *storage_key)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND bind[3];
    unsigned long user_length, md5_length, storage_key_length;
    const char *sql =
        "SELECT 1 FROM file_info f JOIN user_file_list u ON u.md5 = f.md5 "
        "WHERE u.user_name = ? AND f.md5 = ? AND f.storage_key = ? LIMIT 1";
    int result = -1;

    if (!user_name || !md5 || !storage_key) return -1;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;

    user_length = (unsigned long)strlen(user_name);
    md5_length = (unsigned long)strlen(md5);
    storage_key_length = (unsigned long)strlen(storage_key);
    stmt = mysql_stmt_init(conn);
    if (!stmt || mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)user_name; bind[0].length = &user_length;
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)md5; bind[1].length = &md5_length;
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)storage_key; bind[2].length = &storage_key_length;
    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_store_result(stmt) != 0) goto done;
    result = mysql_stmt_num_rows(stmt) == 1 ? 1 : 0;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}

int user_owns_file(const char *user, const char *md5)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND bind[2];
    unsigned long user_length, md5_length;
    const char *sql =
        "SELECT 1 FROM user_file_list WHERE user_name = ? AND md5 = ? LIMIT 1";
    int result = -1;

    if (!user || !md5) return -1;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;
    stmt = mysql_stmt_init(conn);
    if (!stmt || mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) goto done;

    memset(bind, 0, sizeof(bind));
    user_length = (unsigned long)strlen(user);
    md5_length = (unsigned long)strlen(md5);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)user;
    bind[0].length = &user_length;
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)md5;
    bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(stmt, bind) != 0 || mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_store_result(stmt) != 0) goto done;
    result = mysql_stmt_num_rows(stmt) == 1 ? 1 : 0;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}

ClaimFileResult claim_existing_file_with_location(const char *user, const char *md5,
                                                  const char *file_name,
                                                  FileLocation *location)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *check_stmt = NULL;
    MYSQL_STMT *insert_stmt = NULL;
    MYSQL_STMT *increment_stmt = NULL;
    MYSQL_BIND check_bind[1], location_bind[2], insert_bind[3], increment_bind[1];
    unsigned long md5_length, user_length, name_length, location_lengths[2];
    char storage_key[257], url[513];
    const char *check_sql =
        "SELECT storage_key, url FROM file_info WHERE md5 = ? FOR UPDATE";
    const char *insert_sql =
        "INSERT INTO user_file_list (user_name, md5, file_name) VALUES (?, ?, ?)";
    const char *increment_sql =
        "UPDATE file_info SET reference_count = reference_count + 1 WHERE md5 = ?";
    ClaimFileResult result = CLAIM_FILE_DATABASE_FAILURE;

    if (!user || !md5 || !file_name) return CLAIM_FILE_DATABASE_FAILURE;
    if (location) memset(location, 0, sizeof(*location));
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    check_stmt = mysql_stmt_init(conn);
    if (!check_stmt || mysql_stmt_prepare(check_stmt, check_sql, (unsigned long)strlen(check_sql)) != 0) goto rollback;
    memset(check_bind, 0, sizeof(check_bind));
    md5_length = (unsigned long)strlen(md5);
    check_bind[0].buffer_type = MYSQL_TYPE_STRING;
    check_bind[0].buffer = (void *)md5;
    check_bind[0].length = &md5_length;
    memset(storage_key, 0, sizeof(storage_key));
    memset(url, 0, sizeof(url));
    memset(location_bind, 0, sizeof(location_bind));
    location_bind[0].buffer_type = MYSQL_TYPE_STRING;
    location_bind[0].buffer = storage_key;
    location_bind[0].buffer_length = sizeof(storage_key) - 1;
    location_bind[0].length = &location_lengths[0];
    location_bind[1].buffer_type = MYSQL_TYPE_STRING;
    location_bind[1].buffer = url;
    location_bind[1].buffer_length = sizeof(url) - 1;
    location_bind[1].length = &location_lengths[1];
    if (mysql_stmt_bind_param(check_stmt, check_bind) != 0 ||
        mysql_stmt_execute(check_stmt) != 0 ||
        mysql_stmt_bind_result(check_stmt, location_bind) != 0 ||
        mysql_stmt_store_result(check_stmt) != 0) goto rollback;
    if (mysql_stmt_num_rows(check_stmt) == 0) {
        result = CLAIM_FILE_PHYSICAL_MISSING;
        goto rollback;
    }
    if (mysql_stmt_fetch(check_stmt) != 0 ||
        location_lengths[0] >= sizeof(storage_key) ||
        location_lengths[1] >= sizeof(url)) goto rollback;
    storage_key[location_lengths[0]] = '\0';
    url[location_lengths[1]] = '\0';
    if (location) {
        memcpy(location->storage_key, storage_key, location_lengths[0] + 1);
        memcpy(location->url, url, location_lengths[1] + 1);
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
        if (mysql_stmt_errno(insert_stmt) == 1062) result = CLAIM_FILE_ALREADY_OWNED;
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
    result = CLAIM_FILE_LINKED;
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

ClaimFileResult claim_existing_file(const char *user, const char *md5,
                                    const char *file_name)
{
    return claim_existing_file_with_location(user, md5, file_name, NULL);
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
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
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

int share_user_file(const char *user, const char *md5, const char *share_id,
                    unsigned int expires_in_seconds)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *expired_stmt = NULL;
    MYSQL_STMT *share_stmt = NULL;
    MYSQL_STMT *status_stmt = NULL;
    MYSQL_BIND expired_bind[2], share_bind[4], status_bind[2];
    unsigned long user_length, md5_length, share_id_length;
    const char *expired_sql =
        "DELETE s FROM share_file_list s "
        "JOIN user_file_list u ON u.id = s.user_file_id "
        "WHERE u.user_name = ? AND u.md5 = ? "
        "AND s.expires_at IS NOT NULL AND s.expires_at <= UTC_TIMESTAMP()";
    const char *share_sql =
        "INSERT INTO share_file_list "
        "(user_file_id, user_name, md5, file_name, share_id, expires_at) "
        "SELECT id, user_name, md5, file_name, ?, "
        "TIMESTAMPADD(SECOND, ?, UTC_TIMESTAMP()) "
        "FROM user_file_list WHERE user_name = ? AND md5 = ?";
    const char *status_sql = "UPDATE user_file_list SET shared_status = 1 WHERE user_name = ? AND md5 = ?";
    int result = -1;

    if (!user || !md5 || !share_id || strlen(share_id) != 64 || expires_in_seconds == 0) return -1;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    user_length = (unsigned long)strlen(user);
    md5_length = (unsigned long)strlen(md5);
    share_id_length = (unsigned long)strlen(share_id);

    expired_stmt = mysql_stmt_init(conn);
    if (!expired_stmt ||
        mysql_stmt_prepare(expired_stmt, expired_sql,
                           (unsigned long)strlen(expired_sql)) != 0) goto rollback_share;
    memset(expired_bind, 0, sizeof(expired_bind));
    expired_bind[0].buffer_type = MYSQL_TYPE_STRING;
    expired_bind[0].buffer = (void *)user; expired_bind[0].length = &user_length;
    expired_bind[1].buffer_type = MYSQL_TYPE_STRING;
    expired_bind[1].buffer = (void *)md5; expired_bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(expired_stmt, expired_bind) != 0 ||
        mysql_stmt_execute(expired_stmt) != 0) goto rollback_share;

    share_stmt = mysql_stmt_init(conn);
    if (!share_stmt ||
        mysql_stmt_prepare(share_stmt, share_sql, (unsigned long)strlen(share_sql)) != 0) goto rollback_share;
    memset(share_bind, 0, sizeof(share_bind));
    share_bind[0].buffer_type = MYSQL_TYPE_STRING;
    share_bind[0].buffer = (void *)share_id; share_bind[0].length = &share_id_length;
    share_bind[1].buffer_type = MYSQL_TYPE_LONG;
    share_bind[1].buffer = &expires_in_seconds; share_bind[1].is_unsigned = 1;
    share_bind[2].buffer_type = MYSQL_TYPE_STRING;
    share_bind[2].buffer = (void *)user; share_bind[2].length = &user_length;
    share_bind[3].buffer_type = MYSQL_TYPE_STRING;
    share_bind[3].buffer = (void *)md5; share_bind[3].length = &md5_length;
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
    if (expired_stmt) mysql_stmt_close(expired_stmt);
    if (share_stmt) mysql_stmt_close(share_stmt);
    if (status_stmt) mysql_stmt_close(status_stmt);
    if (conn) mysql_close(conn);
    return result;
}

int unshare_user_file(const char *user, const char *md5)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *unshare_stmt = NULL;
    MYSQL_STMT *status_stmt = NULL;
    MYSQL_BIND unshare_bind[2], status_bind[2];
    unsigned long user_length, md5_length;
    const char *unshare_sql = "DELETE FROM share_file_list WHERE user_name = ? AND md5 = ?";
    const char *status_sql = "UPDATE user_file_list SET shared_status = 0 WHERE user_name = ? AND md5 = ?";
    int result = -1;

    if (!user || !md5) return -1;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    user_length = (unsigned long)strlen(user);
    md5_length = (unsigned long)strlen(md5);
    unshare_stmt = mysql_stmt_init(conn);
    if (!unshare_stmt ||
        mysql_stmt_prepare(unshare_stmt, unshare_sql, (unsigned long)strlen(unshare_sql)) != 0) goto rollback_unshare;
    memset(unshare_bind, 0, sizeof(unshare_bind));
    unshare_bind[0].buffer_type = MYSQL_TYPE_STRING;
    unshare_bind[0].buffer = (void *)user; unshare_bind[0].length = &user_length;
    unshare_bind[1].buffer_type = MYSQL_TYPE_STRING;
    unshare_bind[1].buffer = (void *)md5; unshare_bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(unshare_stmt, unshare_bind) != 0 ||
        mysql_stmt_execute(unshare_stmt) != 0) goto rollback_unshare;
    if (mysql_stmt_affected_rows(unshare_stmt) == 0) {
        result = 1;
        goto rollback_unshare;
    }

    status_stmt = mysql_stmt_init(conn);
    if (!status_stmt ||
        mysql_stmt_prepare(status_stmt, status_sql, (unsigned long)strlen(status_sql)) != 0) goto rollback_unshare;
    memset(status_bind, 0, sizeof(status_bind));
    status_bind[0].buffer_type = MYSQL_TYPE_STRING;
    status_bind[0].buffer = (void *)user; status_bind[0].length = &user_length;
    status_bind[1].buffer_type = MYSQL_TYPE_STRING;
    status_bind[1].buffer = (void *)md5; status_bind[1].length = &md5_length;
    if (mysql_stmt_bind_param(status_stmt, status_bind) != 0 ||
        mysql_stmt_execute(status_stmt) != 0 ||
        mysql_stmt_affected_rows(status_stmt) != 1) goto rollback_unshare;
    if (mysql_commit(conn) != 0) goto rollback_unshare;
    result = 0;
    goto done;

rollback_unshare:
    mysql_rollback(conn);
done:
    if (unshare_stmt) mysql_stmt_close(unshare_stmt);
    if (status_stmt) mysql_stmt_close(status_stmt);
    if (conn) mysql_close(conn);
    return result;
}

int find_active_share(const char *share_id, PublicShare *share)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND parameter[1], result_bind[4];
    unsigned long share_id_length, lengths[3];
    char file_name[129], type[33], expires_at[32];
    my_ulonglong size = 0;
    int fetch_result;
    const char *sql =
        "SELECT u.file_name, f.size, f.type, "
        "COALESCE(DATE_FORMAT(s.expires_at, '%Y-%m-%d %H:%i:%s'), 'never') "
        "FROM share_file_list s "
        "JOIN user_file_list u ON u.id = s.user_file_id "
        "JOIN file_info f ON f.md5 = u.md5 "
        "WHERE s.share_id = ? "
        "AND (s.expires_at IS NULL OR s.expires_at > UTC_TIMESTAMP()) LIMIT 1";
    int result = -1;

    if (!share_id || !share || strlen(share_id) != 64) return -1;
    memset(share, 0, sizeof(*share));
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;
    stmt = mysql_stmt_init(conn);
    if (!stmt || mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) goto done;

    share_id_length = (unsigned long)strlen(share_id);
    memset(parameter, 0, sizeof(parameter));
    parameter[0].buffer_type = MYSQL_TYPE_STRING;
    parameter[0].buffer = (void *)share_id;
    parameter[0].length = &share_id_length;
    if (mysql_stmt_bind_param(stmt, parameter) != 0 || mysql_stmt_execute(stmt) != 0 ||
        mysql_stmt_store_result(stmt) != 0) goto done;

    memset(file_name, 0, sizeof(file_name));
    memset(type, 0, sizeof(type));
    memset(expires_at, 0, sizeof(expires_at));
    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = file_name; result_bind[0].buffer_length = sizeof(file_name) - 1;
    result_bind[0].length = &lengths[0];
    result_bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
    result_bind[1].buffer = &size; result_bind[1].is_unsigned = 1;
    result_bind[2].buffer_type = MYSQL_TYPE_STRING;
    result_bind[2].buffer = type; result_bind[2].buffer_length = sizeof(type) - 1;
    result_bind[2].length = &lengths[1];
    result_bind[3].buffer_type = MYSQL_TYPE_STRING;
    result_bind[3].buffer = expires_at; result_bind[3].buffer_length = sizeof(expires_at) - 1;
    result_bind[3].length = &lengths[2];
    if (mysql_stmt_bind_result(stmt, result_bind) != 0) goto done;

    fetch_result = mysql_stmt_fetch(stmt);
    if (fetch_result == MYSQL_NO_DATA) {
        result = 1;
        goto done;
    }
    if (fetch_result != 0) goto done;
    file_name[lengths[0]] = '\0';
    type[lengths[1]] = '\0';
    expires_at[lengths[2]] = '\0';
    memcpy(share->file_name, file_name, lengths[0] + 1);
    memcpy(share->type, type, lengths[1] + 1);
    memcpy(share->expires_at, expires_at, lengths[2] + 1);
    share->size = size;
    result = 0;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}

static void mark_retryable_transaction_error(MYSQL *conn, MYSQL_STMT *stmt,
                                             int *retryable)
{
    unsigned int error_code = stmt ? mysql_stmt_errno(stmt) : 0;

    if (error_code == 0 && conn) error_code = mysql_errno(conn);
    if (retryable && (error_code == 1205 || error_code == 1213)) *retryable = 1;
}

static SaveSharedFileResult save_shared_file_once(const char *user,
                                                  const char *share_id,
                                                  int *retryable)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *share_stmt = NULL;
    MYSQL_STMT *owner_stmt = NULL;
    MYSQL_STMT *file_stmt = NULL;
    MYSQL_STMT *insert_stmt = NULL;
    MYSQL_STMT *increment_stmt = NULL;
    MYSQL_BIND share_param[1], share_result[1];
    MYSQL_BIND owner_param[1], owner_result[2];
    MYSQL_BIND file_param[1], file_result[1];
    MYSQL_BIND insert_param[3], increment_param[1];
    unsigned long share_id_length, user_length, md5_length, file_name_length;
    unsigned long owner_lengths[2];
    my_ulonglong owner_file_id = 0;
    unsigned int file_marker = 0;
    char md5[33], file_name[129];
    const char *share_sql =
        "SELECT user_file_id FROM share_file_list "
        "WHERE share_id = ? "
        "AND (expires_at IS NULL OR expires_at > UTC_TIMESTAMP()) FOR UPDATE";
    const char *owner_sql =
        "SELECT md5, file_name FROM user_file_list WHERE id = ? FOR UPDATE";
    const char *file_sql = "SELECT 1 FROM file_info WHERE md5 = ? FOR UPDATE";
    const char *insert_sql =
        "INSERT INTO user_file_list (user_name, md5, file_name, shared_status) "
        "VALUES (?, ?, ?, 0)";
    const char *increment_sql =
        "UPDATE file_info SET reference_count = reference_count + 1 WHERE md5 = ?";
    SaveSharedFileResult result = SAVE_SHARED_FILE_DATABASE_FAILURE;

    if (retryable) *retryable = 0;
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) goto done;
    if (mysql_autocommit(conn, 0) != 0) goto done;

    share_id_length = (unsigned long)strlen(share_id);
    share_stmt = mysql_stmt_init(conn);
    if (!share_stmt ||
        mysql_stmt_prepare(share_stmt, share_sql, (unsigned long)strlen(share_sql)) != 0) {
        mark_retryable_transaction_error(conn, share_stmt, retryable);
        goto rollback;
    }
    memset(share_param, 0, sizeof(share_param));
    share_param[0].buffer_type = MYSQL_TYPE_STRING;
    share_param[0].buffer = (void *)share_id;
    share_param[0].length = &share_id_length;
    memset(share_result, 0, sizeof(share_result));
    share_result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    share_result[0].buffer = &owner_file_id;
    share_result[0].is_unsigned = 1;
    if (mysql_stmt_bind_param(share_stmt, share_param) != 0 ||
        mysql_stmt_execute(share_stmt) != 0 ||
        mysql_stmt_bind_result(share_stmt, share_result) != 0 ||
        mysql_stmt_store_result(share_stmt) != 0) {
        mark_retryable_transaction_error(conn, share_stmt, retryable);
        goto rollback;
    }
    if (mysql_stmt_num_rows(share_stmt) == 0) {
        result = SAVE_SHARED_FILE_UNAVAILABLE;
        goto rollback;
    }
    if (mysql_stmt_fetch(share_stmt) != 0) {
        mark_retryable_transaction_error(conn, share_stmt, retryable);
        goto rollback;
    }

    owner_stmt = mysql_stmt_init(conn);
    if (!owner_stmt ||
        mysql_stmt_prepare(owner_stmt, owner_sql, (unsigned long)strlen(owner_sql)) != 0) {
        mark_retryable_transaction_error(conn, owner_stmt, retryable);
        goto rollback;
    }
    memset(owner_param, 0, sizeof(owner_param));
    owner_param[0].buffer_type = MYSQL_TYPE_LONGLONG;
    owner_param[0].buffer = &owner_file_id;
    owner_param[0].is_unsigned = 1;
    memset(md5, 0, sizeof(md5));
    memset(file_name, 0, sizeof(file_name));
    memset(owner_result, 0, sizeof(owner_result));
    owner_result[0].buffer_type = MYSQL_TYPE_STRING;
    owner_result[0].buffer = md5;
    owner_result[0].buffer_length = sizeof(md5) - 1;
    owner_result[0].length = &owner_lengths[0];
    owner_result[1].buffer_type = MYSQL_TYPE_STRING;
    owner_result[1].buffer = file_name;
    owner_result[1].buffer_length = sizeof(file_name) - 1;
    owner_result[1].length = &owner_lengths[1];
    if (mysql_stmt_bind_param(owner_stmt, owner_param) != 0 ||
        mysql_stmt_execute(owner_stmt) != 0 ||
        mysql_stmt_bind_result(owner_stmt, owner_result) != 0 ||
        mysql_stmt_store_result(owner_stmt) != 0 ||
        mysql_stmt_num_rows(owner_stmt) != 1 ||
        mysql_stmt_fetch(owner_stmt) != 0 ||
        owner_lengths[0] >= sizeof(md5) || owner_lengths[1] >= sizeof(file_name)) {
        mark_retryable_transaction_error(conn, owner_stmt, retryable);
        goto rollback;
    }
    md5[owner_lengths[0]] = '\0';
    file_name[owner_lengths[1]] = '\0';
    md5_length = owner_lengths[0];
    file_name_length = owner_lengths[1];

    file_stmt = mysql_stmt_init(conn);
    if (!file_stmt ||
        mysql_stmt_prepare(file_stmt, file_sql, (unsigned long)strlen(file_sql)) != 0) {
        mark_retryable_transaction_error(conn, file_stmt, retryable);
        goto rollback;
    }
    memset(file_param, 0, sizeof(file_param));
    file_param[0].buffer_type = MYSQL_TYPE_STRING;
    file_param[0].buffer = md5;
    file_param[0].length = &md5_length;
    memset(file_result, 0, sizeof(file_result));
    file_result[0].buffer_type = MYSQL_TYPE_LONG;
    file_result[0].buffer = &file_marker;
    file_result[0].is_unsigned = 1;
    if (mysql_stmt_bind_param(file_stmt, file_param) != 0 ||
        mysql_stmt_execute(file_stmt) != 0 ||
        mysql_stmt_bind_result(file_stmt, file_result) != 0 ||
        mysql_stmt_store_result(file_stmt) != 0 ||
        mysql_stmt_num_rows(file_stmt) != 1 || mysql_stmt_fetch(file_stmt) != 0) {
        mark_retryable_transaction_error(conn, file_stmt, retryable);
        goto rollback;
    }

    user_length = (unsigned long)strlen(user);
    insert_stmt = mysql_stmt_init(conn);
    if (!insert_stmt ||
        mysql_stmt_prepare(insert_stmt, insert_sql, (unsigned long)strlen(insert_sql)) != 0) {
        mark_retryable_transaction_error(conn, insert_stmt, retryable);
        goto rollback;
    }
    memset(insert_param, 0, sizeof(insert_param));
    insert_param[0].buffer_type = MYSQL_TYPE_STRING;
    insert_param[0].buffer = (void *)user;
    insert_param[0].length = &user_length;
    insert_param[1].buffer_type = MYSQL_TYPE_STRING;
    insert_param[1].buffer = md5;
    insert_param[1].length = &md5_length;
    insert_param[2].buffer_type = MYSQL_TYPE_STRING;
    insert_param[2].buffer = file_name;
    insert_param[2].length = &file_name_length;
    if (mysql_stmt_bind_param(insert_stmt, insert_param) != 0) {
        mark_retryable_transaction_error(conn, insert_stmt, retryable);
        goto rollback;
    }
    if (mysql_stmt_execute(insert_stmt) != 0) {
        if (mysql_stmt_errno(insert_stmt) == 1062) {
            result = SAVE_SHARED_FILE_ALREADY_OWNED;
        } else {
            mark_retryable_transaction_error(conn, insert_stmt, retryable);
        }
        goto rollback;
    }

    increment_stmt = mysql_stmt_init(conn);
    if (!increment_stmt ||
        mysql_stmt_prepare(increment_stmt, increment_sql,
                           (unsigned long)strlen(increment_sql)) != 0) {
        mark_retryable_transaction_error(conn, increment_stmt, retryable);
        goto rollback;
    }
    memset(increment_param, 0, sizeof(increment_param));
    increment_param[0].buffer_type = MYSQL_TYPE_STRING;
    increment_param[0].buffer = md5;
    increment_param[0].length = &md5_length;
    if (mysql_stmt_bind_param(increment_stmt, increment_param) != 0 ||
        mysql_stmt_execute(increment_stmt) != 0 ||
        mysql_stmt_affected_rows(increment_stmt) != 1) {
        mark_retryable_transaction_error(conn, increment_stmt, retryable);
        goto rollback;
    }
    if (mysql_commit(conn) != 0) goto rollback;
    result = SAVE_SHARED_FILE_SAVED;
    goto done;

rollback:
    mysql_rollback(conn);
done:
    if (share_stmt) mysql_stmt_close(share_stmt);
    if (owner_stmt) mysql_stmt_close(owner_stmt);
    if (file_stmt) mysql_stmt_close(file_stmt);
    if (insert_stmt) mysql_stmt_close(insert_stmt);
    if (increment_stmt) mysql_stmt_close(increment_stmt);
    if (conn) mysql_close(conn);
    return result;
}

SaveSharedFileResult save_shared_file(const char *user, const char *share_id)
{
    SaveSharedFileResult result;
    int attempt;
    int retryable = 0;

    if (!user || strlen(user) == 0 || strlen(user) > 32 ||
        !share_id || strlen(share_id) != 64) return SAVE_SHARED_FILE_DATABASE_FAILURE;
    for (attempt = 0; attempt < 3; ++attempt) {
        result = save_shared_file_once(user, share_id, &retryable);
        if (result != SAVE_SHARED_FILE_DATABASE_FAILURE || !retryable) return result;
    }
    return SAVE_SHARED_FILE_DATABASE_FAILURE;
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
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
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
