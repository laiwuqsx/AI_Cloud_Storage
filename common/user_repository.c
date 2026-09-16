#include "user_repository.h"

#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime_config.h"

int create_user(const char *user, const char *nickname, const char *password_digest,
                const char *salt)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND bind[4];
    unsigned long lengths[4];
    const char *sql = "INSERT INTO user_info (user_name, nick_name, password, salt) VALUES (?, ?, ?, ?)";
    int result = -1;
    int i;

    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) {
        fprintf(stderr, "create_user MySQL connection failed: %s\n", mysql_error(conn));
        goto done;
    }
    stmt = mysql_stmt_init(conn);
    if (!stmt || mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) goto done;

    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(user);
    lengths[1] = (unsigned long)strlen(nickname);
    lengths[2] = (unsigned long)strlen(password_digest);
    lengths[3] = (unsigned long)strlen(salt);
    for (i = 0; i < 4; ++i) bind[i].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)user; bind[0].length = &lengths[0];
    bind[1].buffer = (void *)nickname; bind[1].length = &lengths[1];
    bind[2].buffer = (void *)password_digest; bind[2].length = &lengths[2];
    bind[3].buffer = (void *)salt; bind[3].length = &lengths[3];
    if (mysql_stmt_bind_param(stmt, bind) != 0) goto done;
    if (mysql_stmt_execute(stmt) == 0) result = 0;
    else if (mysql_stmt_errno(stmt) == 1062) result = 1;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}

int find_user_credentials(const char *user, UserCredentials *credentials)
{
    MYSQL *conn = NULL;
    MYSQL_STMT *stmt = NULL;
    MYSQL_BIND parameter[1];
    MYSQL_BIND result_bind[2];
    unsigned long user_length;
    unsigned long result_lengths[2];
    const char *sql = "SELECT password, salt FROM user_info WHERE user_name = ? LIMIT 1";
    int result = -1;

    if (!user || !credentials) return -1;
    memset(credentials, 0, sizeof(*credentials));
    conn = mysql_init(NULL);
    if (!conn) goto done;
    if (!mysql_real_connect(conn, runtime_config_get("MYSQL_HOST", "127.0.0.1"),
                            runtime_config_get("MYSQL_USER", "root"),
                            runtime_config_get("MYSQL_PASSWORD", ""),
                            runtime_config_get("MYSQL_DATABASE", "ai_cloud_storage"),
                            3306, NULL, 0)) {
        fprintf(stderr, "find_user_credentials MySQL connection failed: %s\n",
                mysql_error(conn));
        goto done;
    }
    stmt = mysql_stmt_init(conn);
    if (!stmt || mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) goto done;

    memset(parameter, 0, sizeof(parameter));
    user_length = (unsigned long)strlen(user);
    parameter[0].buffer_type = MYSQL_TYPE_STRING;
    parameter[0].buffer = (void *)user;
    parameter[0].length = &user_length;
    if (mysql_stmt_bind_param(stmt, parameter) != 0 || mysql_stmt_execute(stmt) != 0) goto done;

    memset(result_bind, 0, sizeof(result_bind));
    result_bind[0].buffer_type = MYSQL_TYPE_STRING;
    result_bind[0].buffer = credentials->password_digest;
    result_bind[0].buffer_length = sizeof(credentials->password_digest) - 1;
    result_bind[0].length = &result_lengths[0];
    result_bind[1].buffer_type = MYSQL_TYPE_STRING;
    result_bind[1].buffer = credentials->salt;
    result_bind[1].buffer_length = sizeof(credentials->salt) - 1;
    result_bind[1].length = &result_lengths[1];
    if (mysql_stmt_bind_result(stmt, result_bind) != 0) goto done;

    if (mysql_stmt_fetch(stmt) == MYSQL_NO_DATA) {
        result = 1;
        goto done;
    }
    if (result_lengths[0] != 32 || result_lengths[1] != 32) goto done;
    credentials->password_digest[32] = '\0';
    credentials->salt[32] = '\0';
    result = 0;

done:
    if (stmt) mysql_stmt_close(stmt);
    if (conn) mysql_close(conn);
    return result;
}
