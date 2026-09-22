#include "chunk_upload_repository.h"

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

CreateChunkSessionResult create_chunk_upload_session(
    const NewChunkUploadSession *session)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[8];
    unsigned long lengths[4];
    my_ulonglong total_size;
    unsigned int chunk_size;
    unsigned int total_chunks;
    unsigned int expires_in;
    const char *sql =
        "INSERT INTO chunk_upload_session "
        "(upload_id, user_name, file_name, file_md5, total_size, chunk_size, "
        "total_chunks, status, expires_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 'receiving', "
        "TIMESTAMPADD(SECOND, ?, CURRENT_TIMESTAMP))";
    CreateChunkSessionResult result = CREATE_CHUNK_SESSION_DATABASE_ERROR;

    if (!session || !session->upload_id || !session->user_name ||
        !session->file_name || !session->file_md5 || session->total_size == 0 ||
        session->chunk_size == 0 || session->total_chunks == 0 ||
        session->expires_in_seconds == 0) return CREATE_CHUNK_SESSION_DATABASE_ERROR;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;

    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(session->upload_id);
    lengths[1] = (unsigned long)strlen(session->user_name);
    lengths[2] = (unsigned long)strlen(session->file_name);
    lengths[3] = (unsigned long)strlen(session->file_md5);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)session->upload_id;
    bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)session->user_name;
    bind[1].length = &lengths[1];
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void *)session->file_name;
    bind[2].length = &lengths[2];
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = (void *)session->file_md5;
    bind[3].length = &lengths[3];
    total_size = (my_ulonglong)session->total_size;
    bind[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[4].buffer = &total_size;
    bind[4].is_unsigned = 1;
    chunk_size = session->chunk_size;
    bind[5].buffer_type = MYSQL_TYPE_LONG;
    bind[5].buffer = &chunk_size;
    bind[5].is_unsigned = 1;
    total_chunks = session->total_chunks;
    bind[6].buffer_type = MYSQL_TYPE_LONG;
    bind[6].buffer = &total_chunks;
    bind[6].is_unsigned = 1;
    expires_in = session->expires_in_seconds;
    bind[7].buffer_type = MYSQL_TYPE_LONG;
    bind[7].buffer = &expires_in;
    bind[7].is_unsigned = 1;

    if (mysql_stmt_bind_param(statement, bind) != 0) goto done;
    if (mysql_stmt_execute(statement) != 0) {
        if (mysql_stmt_errno(statement) == 1062U) {
            result = CREATE_CHUNK_SESSION_DUPLICATE;
        }
        goto done;
    }
    result = CREATE_CHUNK_SESSION_OK;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}
