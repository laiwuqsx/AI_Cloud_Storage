#include "chunk_upload_repository.h"

#include <mysql/mysql.h>
#include <stdlib.h>
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

GetChunkStatusResult get_chunk_upload_status(
    const char *upload_id, const char *user_name, ChunkUploadStatus *status)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND param[2], output[7];
    unsigned long param_lengths[2];
    unsigned long file_name_length = 0;
    unsigned long file_md5_length = 0;
    unsigned long status_length = 0;
    my_ulonglong total_size;
    my_ulonglong expires_in;
    unsigned int chunk_size;
    unsigned int total_chunks;
    unsigned int chunk_index;
    int fetch_result;
    const char *session_sql =
        "SELECT file_name, file_md5, total_size, chunk_size, total_chunks, "
        "CASE WHEN status = 'receiving' AND expires_at <= CURRENT_TIMESTAMP "
        "THEN 'expired' ELSE status END, "
        "GREATEST(TIMESTAMPDIFF(SECOND, CURRENT_TIMESTAMP, expires_at), 0) "
        "FROM chunk_upload_session WHERE upload_id = ? AND user_name = ?";
    const char *parts_sql =
        "SELECT chunk_index FROM chunk_upload_part "
        "WHERE upload_id = ? AND status = 'ready' ORDER BY chunk_index";
    GetChunkStatusResult result = GET_CHUNK_STATUS_DATABASE_ERROR;

    if (!upload_id || !user_name || !status) return GET_CHUNK_STATUS_DATABASE_ERROR;
    memset(status, 0, sizeof(*status));
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, session_sql,
                           (unsigned long)strlen(session_sql)) != 0) goto done;
    memset(param, 0, sizeof(param));
    param_lengths[0] = (unsigned long)strlen(upload_id);
    param_lengths[1] = (unsigned long)strlen(user_name);
    param[0].buffer_type = MYSQL_TYPE_STRING;
    param[0].buffer = (void *)upload_id;
    param[0].length = &param_lengths[0];
    param[1].buffer_type = MYSQL_TYPE_STRING;
    param[1].buffer = (void *)user_name;
    param[1].length = &param_lengths[1];
    if (mysql_stmt_bind_param(statement, param) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    memset(output, 0, sizeof(output));
    output[0].buffer_type = MYSQL_TYPE_STRING;
    output[0].buffer = status->file_name;
    output[0].buffer_length = sizeof(status->file_name) - 1;
    output[0].length = &file_name_length;
    output[1].buffer_type = MYSQL_TYPE_STRING;
    output[1].buffer = status->file_md5;
    output[1].buffer_length = sizeof(status->file_md5) - 1;
    output[1].length = &file_md5_length;
    output[2].buffer_type = MYSQL_TYPE_LONGLONG;
    output[2].buffer = &total_size;
    output[2].is_unsigned = 1;
    output[3].buffer_type = MYSQL_TYPE_LONG;
    output[3].buffer = &chunk_size;
    output[3].is_unsigned = 1;
    output[4].buffer_type = MYSQL_TYPE_LONG;
    output[4].buffer = &total_chunks;
    output[4].is_unsigned = 1;
    output[5].buffer_type = MYSQL_TYPE_STRING;
    output[5].buffer = status->status;
    output[5].buffer_length = sizeof(status->status) - 1;
    output[5].length = &status_length;
    output[6].buffer_type = MYSQL_TYPE_LONGLONG;
    output[6].buffer = &expires_in;
    output[6].is_unsigned = 1;
    if (mysql_stmt_bind_result(statement, output) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto done;
    fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result == MYSQL_NO_DATA) {
        result = GET_CHUNK_STATUS_UNAVAILABLE;
        goto done;
    }
    if (fetch_result != 0 ||
        file_name_length >= sizeof(status->file_name) ||
        file_md5_length >= sizeof(status->file_md5) ||
        status_length >= sizeof(status->status) ||
        total_chunks == 0 || total_chunks > CHUNK_UPLOAD_MAX_PARTS) goto done;
    status->file_name[file_name_length] = '\0';
    status->file_md5[file_md5_length] = '\0';
    status->status[status_length] = '\0';
    status->total_size = (uint64_t)total_size;
    status->chunk_size = chunk_size;
    status->total_chunks = total_chunks;
    status->expires_in_seconds = (uint64_t)expires_in;

    mysql_stmt_close(statement);
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, parts_sql,
                           (unsigned long)strlen(parts_sql)) != 0) goto done;
    memset(param, 0, sizeof(param));
    param_lengths[0] = (unsigned long)strlen(upload_id);
    param[0].buffer_type = MYSQL_TYPE_STRING;
    param[0].buffer = (void *)upload_id;
    param[0].length = &param_lengths[0];
    if (mysql_stmt_bind_param(statement, param) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    memset(output, 0, sizeof(output));
    output[0].buffer_type = MYSQL_TYPE_LONG;
    output[0].buffer = &chunk_index;
    output[0].is_unsigned = 1;
    if (mysql_stmt_bind_result(statement, output) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto done;
    while ((fetch_result = mysql_stmt_fetch(statement)) == 0) {
        if (status->ready_count >= CHUNK_UPLOAD_MAX_PARTS ||
            chunk_index >= status->total_chunks) goto done;
        status->ready_chunks[status->ready_count++] = chunk_index;
    }
    if (fetch_result != MYSQL_NO_DATA) goto done;
    result = GET_CHUNK_STATUS_OK;

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

static int bind_identity(MYSQL_STMT *statement, const char *upload_id,
                         const char *user_name, MYSQL_BIND bind[2],
                         unsigned long lengths[2])
{
    memset(bind, 0, sizeof(MYSQL_BIND) * 2);
    lengths[0] = (unsigned long)strlen(upload_id);
    lengths[1] = (unsigned long)strlen(user_name);
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void *)upload_id;
    bind[0].length = &lengths[0];
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void *)user_name;
    bind[1].length = &lengths[1];
    return mysql_stmt_bind_param(statement, bind);
}

static int set_completion_status(MYSQL *connection, const char *upload_id,
                                 const char *user_name, const char *target)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND bind[2];
    unsigned long lengths[2];
    const char *complete_sql =
        "UPDATE chunk_upload_session SET status = 'completed' "
        "WHERE upload_id = ? AND user_name = ? AND status = 'completing'";
    const char *release_sql =
        "UPDATE chunk_upload_session SET status = 'receiving' "
        "WHERE upload_id = ? AND user_name = ? AND status = 'completing' "
        "AND expires_at > CURRENT_TIMESTAMP";
    const char *sql = strcmp(target, "completed") == 0 ? complete_sql : release_sql;
    int result = -1;

    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0 ||
        bind_identity(statement, upload_id, user_name, bind, lengths) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    result = mysql_stmt_affected_rows(statement) == 1 ? 0 : 1;

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

static ClaimChunkCompletionResult classify_unclaimed_session(
    MYSQL *connection, const char *upload_id, const char *user_name)
{
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND param[2], output[2];
    unsigned long lengths[2];
    char status[16] = {0};
    unsigned long status_length = 0;
    unsigned int complete = 0;
    int fetch_result;
    const char *sql =
        "SELECT status, ((SELECT COUNT(*) FROM chunk_upload_part p "
        "WHERE p.upload_id = s.upload_id AND p.status = 'ready') = s.total_chunks "
        "AND (SELECT COALESCE(SUM(size), 0) FROM chunk_upload_part p "
        "WHERE p.upload_id = s.upload_id AND p.status = 'ready') = s.total_size) "
        "FROM chunk_upload_session s WHERE upload_id = ? AND user_name = ? "
        "AND expires_at > CURRENT_TIMESTAMP";
    ClaimChunkCompletionResult result = CLAIM_CHUNK_COMPLETION_DATABASE_ERROR;

    statement = mysql_stmt_init(connection);
    if (!statement || mysql_stmt_prepare(statement, sql, (unsigned long)strlen(sql)) != 0 ||
        bind_identity(statement, upload_id, user_name, param, lengths) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    memset(output, 0, sizeof(output));
    output[0].buffer_type = MYSQL_TYPE_STRING;
    output[0].buffer = status;
    output[0].buffer_length = sizeof(status) - 1;
    output[0].length = &status_length;
    output[1].buffer_type = MYSQL_TYPE_LONG;
    output[1].buffer = &complete;
    output[1].is_unsigned = 1;
    if (mysql_stmt_bind_result(statement, output) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto done;
    fetch_result = mysql_stmt_fetch(statement);
    if (fetch_result == MYSQL_NO_DATA) {
        result = CLAIM_CHUNK_COMPLETION_UNAVAILABLE;
    } else if (fetch_result == 0 && status_length < sizeof(status)) {
        status[status_length] = '\0';
        if (strcmp(status, "completed") == 0) {
            result = CLAIM_CHUNK_COMPLETION_ALREADY_COMPLETED;
        } else {
            result = strcmp(status, "receiving") == 0 && !complete
                ? CLAIM_CHUNK_COMPLETION_INCOMPLETE
                : CLAIM_CHUNK_COMPLETION_UNAVAILABLE;
        }
    }

done:
    if (statement) mysql_stmt_close(statement);
    return result;
}

ClaimChunkCompletionResult claim_chunk_upload_completion(
    const char *upload_id, const char *user_name, ChunkUploadCompletion *completion)
{
    MYSQL *connection = NULL;
    MYSQL_STMT *statement = NULL;
    MYSQL_BIND param[2], output[4];
    unsigned long lengths[2];
    unsigned long file_name_length = 0;
    unsigned long md5_length = 0;
    unsigned long path_length = 0;
    my_ulonglong total_size = 0;
    my_ulonglong part_size = 0;
    unsigned int total_chunks = 0;
    unsigned int chunk_index = 0;
    char part_path[CHUNK_STORED_PATH_CAPACITY];
    size_t count = 0;
    int fetch_result;
    const char *claim_sql =
        "UPDATE chunk_upload_session s SET status = 'completing' "
        "WHERE upload_id = ? AND user_name = ? AND status = 'receiving' "
        "AND expires_at > CURRENT_TIMESTAMP "
        "AND (SELECT COUNT(*) FROM chunk_upload_part p WHERE p.upload_id = s.upload_id "
        "AND p.status = 'ready') = s.total_chunks "
        "AND (SELECT COALESCE(SUM(size), 0) FROM chunk_upload_part p "
        "WHERE p.upload_id = s.upload_id AND p.status = 'ready') = s.total_size";
    const char *session_sql =
        "SELECT file_name, file_md5, total_size, total_chunks "
        "FROM chunk_upload_session WHERE upload_id = ? AND user_name = ? "
        "AND status = 'completing'";
    const char *parts_sql =
        "SELECT chunk_index, size, stored_path FROM chunk_upload_part "
        "WHERE upload_id = ? AND status = 'ready' ORDER BY chunk_index";
    ClaimChunkCompletionResult result = CLAIM_CHUNK_COMPLETION_DATABASE_ERROR;

    if (!upload_id || !user_name || !completion) return result;
    memset(completion, 0, sizeof(*completion));
    connection = connect_database();
    if (!connection) goto done;
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, claim_sql, (unsigned long)strlen(claim_sql)) != 0 ||
        bind_identity(statement, upload_id, user_name, param, lengths) != 0 ||
        mysql_stmt_execute(statement) != 0) goto done;
    if (mysql_stmt_affected_rows(statement) != 1) {
        mysql_stmt_close(statement);
        statement = NULL;
        result = classify_unclaimed_session(connection, upload_id, user_name);
        goto done;
    }

    mysql_stmt_close(statement);
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, session_sql, (unsigned long)strlen(session_sql)) != 0 ||
        bind_identity(statement, upload_id, user_name, param, lengths) != 0 ||
        mysql_stmt_execute(statement) != 0) goto release;
    memset(output, 0, sizeof(output));
    output[0].buffer_type = MYSQL_TYPE_STRING;
    output[0].buffer = completion->file_name;
    output[0].buffer_length = sizeof(completion->file_name) - 1;
    output[0].length = &file_name_length;
    output[1].buffer_type = MYSQL_TYPE_STRING;
    output[1].buffer = completion->file_md5;
    output[1].buffer_length = sizeof(completion->file_md5) - 1;
    output[1].length = &md5_length;
    output[2].buffer_type = MYSQL_TYPE_LONGLONG;
    output[2].buffer = &total_size;
    output[2].is_unsigned = 1;
    output[3].buffer_type = MYSQL_TYPE_LONG;
    output[3].buffer = &total_chunks;
    output[3].is_unsigned = 1;
    if (mysql_stmt_bind_result(statement, output) != 0 ||
        mysql_stmt_store_result(statement) != 0 || mysql_stmt_fetch(statement) != 0 ||
        file_name_length >= sizeof(completion->file_name) ||
        md5_length >= sizeof(completion->file_md5) || total_chunks == 0 ||
        total_chunks > CHUNK_UPLOAD_MAX_PARTS) goto release;
    completion->file_name[file_name_length] = '\0';
    completion->file_md5[md5_length] = '\0';
    completion->total_size = (uint64_t)total_size;
    completion->total_chunks = total_chunks;
    completion->parts = calloc(total_chunks, sizeof(*completion->parts));
    if (!completion->parts) goto release;

    mysql_stmt_close(statement);
    statement = mysql_stmt_init(connection);
    if (!statement ||
        mysql_stmt_prepare(statement, parts_sql, (unsigned long)strlen(parts_sql)) != 0)
        goto release;
    memset(param, 0, sizeof(param));
    lengths[0] = (unsigned long)strlen(upload_id);
    param[0].buffer_type = MYSQL_TYPE_STRING;
    param[0].buffer = (void *)upload_id;
    param[0].length = &lengths[0];
    if (mysql_stmt_bind_param(statement, param) != 0 ||
        mysql_stmt_execute(statement) != 0) goto release;
    memset(output, 0, sizeof(output));
    output[0].buffer_type = MYSQL_TYPE_LONG;
    output[0].buffer = &chunk_index;
    output[0].is_unsigned = 1;
    output[1].buffer_type = MYSQL_TYPE_LONGLONG;
    output[1].buffer = &part_size;
    output[1].is_unsigned = 1;
    output[2].buffer_type = MYSQL_TYPE_STRING;
    output[2].buffer = part_path;
    output[2].buffer_length = CHUNK_STORED_PATH_CAPACITY - 1;
    output[2].length = &path_length;
    if (mysql_stmt_bind_result(statement, output) != 0 ||
        mysql_stmt_store_result(statement) != 0) goto release;
    while ((fetch_result = mysql_stmt_fetch(statement)) == 0) {
        if (count >= total_chunks || chunk_index != count) goto release;
        if (path_length >= CHUNK_STORED_PATH_CAPACITY) goto release;
        part_path[path_length] = '\0';
        memcpy(completion->parts[count].stored_path, part_path, path_length + 1);
        completion->parts[count].size = (uint64_t)part_size;
        ++count;
    }
    if (fetch_result != MYSQL_NO_DATA || count != total_chunks) goto release;
    result = CLAIM_CHUNK_COMPLETION_OK;
    goto done;

release:
    set_completion_status(connection, upload_id, user_name, "receiving");
    free_chunk_upload_completion(completion);

done:
    if (statement) mysql_stmt_close(statement);
    if (connection) mysql_close(connection);
    return result;
}

int finish_chunk_upload_completion(const char *upload_id, const char *user_name,
                                   int success)
{
    MYSQL *connection;
    int result;

    if (!upload_id || !user_name) return -1;
    connection = connect_database();
    if (!connection) return -1;
    result = set_completion_status(connection, upload_id, user_name,
                                   success ? "completed" : "receiving");
    mysql_close(connection);
    return result;
}

void free_chunk_upload_completion(ChunkUploadCompletion *completion)
{
    if (!completion) return;
    free(completion->parts);
    memset(completion, 0, sizeof(*completion));
}
