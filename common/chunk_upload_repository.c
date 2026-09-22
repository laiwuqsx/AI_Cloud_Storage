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

FindChunkSessionResult find_receiving_chunk_session(
    const char *upload_id, const char *user_name, ChunkUploadSessionPlan *plan)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND param[2], output[3];
    unsigned long lengths[2];
    my_ulonglong total_size;
    unsigned int chunk_size;
    unsigned int total_chunks;
    const char *sql =
        "SELECT total_size, chunk_size, total_chunks FROM chunk_upload_session "
        "WHERE upload_id = ? AND user_name = ? AND status = 'receiving' "
        "AND expires_at > CURRENT_TIMESTAMP";
    int fetch_result;
    FindChunkSessionResult result = FIND_CHUNK_SESSION_DATABASE_ERROR;

    if (!upload_id || !user_name || !plan) return FIND_CHUNK_SESSION_DATABASE_ERROR;
    memset(plan, 0, sizeof(*plan));
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;
    memset(param, 0, sizeof(param));
    lengths[0] = (unsigned long)strlen(upload_id);
    lengths[1] = (unsigned long)strlen(user_name);
    param[0].buffer_type = MYSQL_TYPE_STRING;
    param[0].buffer = (void *)upload_id;
    param[0].length = &lengths[0];
    param[1].buffer_type = MYSQL_TYPE_STRING;
    param[1].buffer = (void *)user_name;
    param[1].length = &lengths[1];
    if (mysql_stmt_bind_param(statement, param) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    memset(output, 0, sizeof(output));
    output[0].buffer_type = MYSQL_TYPE_LONGLONG;
    output[0].buffer = &total_size;
    output[0].is_unsigned = 1;
    output[1].buffer_type = MYSQL_TYPE_LONG;
    output[1].buffer = &chunk_size;
    output[1].is_unsigned = 1;
    output[2].buffer_type = MYSQL_TYPE_LONG;
    output[2].buffer = &total_chunks;
    output[2].is_unsigned = 1;
    if (mysql_stmt_bind_result(statement, output) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto done;
    fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result == MYSQL_NO_DATA) {
        result = FIND_CHUNK_SESSION_UNAVAILABLE;
        goto done;
    }
    if (fetch_result != 0) goto done;
    plan->total_size = (uint64_t)total_size;
    plan->chunk_size = chunk_size;
    plan->total_chunks = total_chunks;
    result = FIND_CHUNK_SESSION_OK;

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

static ReserveChunkPartResult find_reserved_part(MYSQL *connection,
                                                 const ChunkUploadPart *part)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND param[2], output[4];
    unsigned long upload_id_length;
    unsigned long md5_length = 0;
    unsigned long path_length = 0;
    my_ulonglong size;
    unsigned int chunk_index = part->chunk_index;
    char md5[33];
    char stored_path[CHUNK_STORED_PATH_CAPACITY];
    char status[16];
    unsigned long status_length = 0;
    const char *sql =
        "SELECT size, chunk_md5, stored_path, status FROM chunk_upload_part "
        "WHERE upload_id = ? AND chunk_index = ?";
    ReserveChunkPartResult result = RESERVE_CHUNK_PART_DATABASE_ERROR;

    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;
    memset(param, 0, sizeof(param));
    upload_id_length = (unsigned long)strlen(part->upload_id);
    param[0].buffer_type = MYSQL_TYPE_STRING;
    param[0].buffer = (void *)part->upload_id;
    param[0].length = &upload_id_length;
    param[1].buffer_type = MYSQL_TYPE_LONG;
    param[1].buffer = &chunk_index;
    param[1].is_unsigned = 1;
    if (mysql_stmt_bind_param(statement, param) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    memset(md5, 0, sizeof(md5));
    memset(stored_path, 0, sizeof(stored_path));
    memset(status, 0, sizeof(status));
    memset(output, 0, sizeof(output));
    output[0].buffer_type = MYSQL_TYPE_LONGLONG;
    output[0].buffer = &size;
    output[0].is_unsigned = 1;
    output[1].buffer_type = MYSQL_TYPE_STRING;
    output[1].buffer = md5;
    output[1].buffer_length = sizeof(md5) - 1;
    output[1].length = &md5_length;
    output[2].buffer_type = MYSQL_TYPE_STRING;
    output[2].buffer = stored_path;
    output[2].buffer_length = sizeof(stored_path) - 1;
    output[2].length = &path_length;
    output[3].buffer_type = MYSQL_TYPE_STRING;
    output[3].buffer = status;
    output[3].buffer_length = sizeof(status) - 1;
    output[3].length = &status_length;
    if (mysql_stmt_bind_result(statement, output) != 0 ||
        mysql_stmt_store_result(statement) != 0 || mysql_stmt_fetch(statement) != 0 ||
        md5_length >= sizeof(md5) || path_length >= sizeof(stored_path) ||
        status_length >= sizeof(status)) goto done;
    md5[md5_length] = '\0';
    stored_path[path_length] = '\0';
    status[status_length] = '\0';
    if ((uint64_t)size != part->size || strcmp(md5, part->chunk_md5) != 0 ||
        strcmp(stored_path, part->stored_path) != 0) {
        result = RESERVE_CHUNK_PART_CONFLICT;
    } else if (strcmp(status, "ready") == 0) {
        result = RESERVE_CHUNK_PART_READY;
    } else if (strcmp(status, "staging") == 0) {
        result = RESERVE_CHUNK_PART_STAGING;
    }

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

ReserveChunkPartResult reserve_chunk_upload_part(const ChunkUploadPart *part)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[5];
    unsigned long lengths[3];
    unsigned int chunk_index;
    my_ulonglong size;
    const char *sql =
        "INSERT INTO chunk_upload_part "
        "(upload_id, chunk_index, size, chunk_md5, stored_path, status) "
        "VALUES (?, ?, ?, ?, ?, 'staging')";
    ReserveChunkPartResult result = RESERVE_CHUNK_PART_DATABASE_ERROR;

    if (!part || !part->upload_id || !part->chunk_md5 || !part->stored_path ||
        part->size == 0) return RESERVE_CHUNK_PART_DATABASE_ERROR;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(part->upload_id);
    lengths[1] = (unsigned long)strlen(part->chunk_md5);
    lengths[2] = (unsigned long)strlen(part->stored_path);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)part->upload_id;
    bind[0].length = &lengths[0];
    chunk_index = part->chunk_index;
    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = &chunk_index;
    bind[1].is_unsigned = 1;
    size = (my_ulonglong)part->size;
    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &size;
    bind[2].is_unsigned = 1;
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = (void *)part->chunk_md5;
    bind[3].length = &lengths[1];
    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = (void *)part->stored_path;
    bind[4].length = &lengths[2];
    if (mysql_stmt_bind_param(statement, bind) != 0) goto done;
    if (mysql_stmt_execute(statement) == 0) {
        result = RESERVE_CHUNK_PART_NEW;
        goto done;
    }
    if (mysql_stmt_errno(statement) != 1062U) goto done;
    mysql_stmt_close(statement);
    statement = NULL;
    result = find_reserved_part(connection, part);

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int mark_chunk_upload_part_ready(const ChunkUploadPart *part)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[5];
    unsigned long lengths[3];
    unsigned int chunk_index;
    my_ulonglong size;
    const char *sql =
        "UPDATE chunk_upload_part SET status = 'ready' "
        "WHERE upload_id = ? AND chunk_index = ? AND size = ? "
        "AND chunk_md5 = ? AND stored_path = ? AND status IN ('staging', 'ready')";
    int result = -1;

    if (!part || !part->upload_id || !part->chunk_md5 || !part->stored_path) return -1;
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0) goto done;
    memset(bind, 0, sizeof(bind));
    lengths[0] = (unsigned long)strlen(part->upload_id);
    lengths[1] = (unsigned long)strlen(part->chunk_md5);
    lengths[2] = (unsigned long)strlen(part->stored_path);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)part->upload_id;
    bind[0].length = &lengths[0];
    chunk_index = part->chunk_index;
    bind[1].buffer_type = MYSQL_TYPE_LONG;
    bind[1].buffer = &chunk_index;
    bind[1].is_unsigned = 1;
    size = (my_ulonglong)part->size;
    bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind[2].buffer = &size;
    bind[2].is_unsigned = 1;
    bind[3].buffer_type = MYSQL_TYPE_STRING;
    bind[3].buffer = (void *)part->chunk_md5;
    bind[3].length = &lengths[1];
    bind[4].buffer_type = MYSQL_TYPE_STRING;
    bind[4].buffer = (void *)part->stored_path;
    bind[4].length = &lengths[2];
    if (mysql_stmt_bind_param(statement, bind) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    if (mysql_stmt_affected_rows(statement) == 1) {
        result = 0;
    } else {
        ReserveChunkPartResult existing = find_reserved_part(connection, part);

        result = existing == RESERVE_CHUNK_PART_READY ? 0 : 1;
    }

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}
