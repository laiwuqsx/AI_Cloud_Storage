#include "fcgi_stdio.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk_upload_repository.h"
#include "chunk_storage.h"
#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "token_service.h"
#include "upload_id.h"
#include "upload_intake.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096
#define MIN_CHUNK_SIZE (1024U * 1024U)
#define MAX_CHUNK_SIZE (20U * 1024U * 1024U)
#define MAX_TOTAL_SIZE (50ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_TOTAL_CHUNKS 10000U
#define SESSION_TTL_SECONDS 86400U
#define ID_GENERATION_ATTEMPTS 3
#define CHUNK_READ_BUFFER_SIZE (64U * 1024U)

enum {
    CHUNK_INIT_OK = 0,
    CHUNK_INIT_INVALID_REQUEST = 1,
    CHUNK_INIT_TOKEN_ERROR = 2,
    CHUNK_INIT_DATABASE_ERROR = 3
};

enum {
    CHUNK_UPLOAD_OK = 0,
    CHUNK_UPLOAD_INVALID_REQUEST = 1,
    CHUNK_UPLOAD_TOKEN_ERROR = 2,
    CHUNK_UPLOAD_DATABASE_ERROR = 3,
    CHUNK_UPLOAD_SESSION_UNAVAILABLE = 4,
    CHUNK_UPLOAD_RECEIVE_ERROR = 5,
    CHUNK_UPLOAD_CONFLICT = 6,
    CHUNK_UPLOAD_STORAGE_ERROR = 7
};

static int read_body(char body[MAX_BODY_SIZE])
{
    const char *length_text = getenv("CONTENT_LENGTH");
    char *end;
    unsigned long length;

    if (!length_text || length_text[0] == '\0') return -1;
    length = strtoul(length_text, &end, 10);
    if (*end != '\0' || length == 0 || length >= MAX_BODY_SIZE) return -1;
    if (fread(body, 1, length, stdin) != length) return -1;
    body[length] = '\0';
    return 0;
}

static void handle_init_request(void)
{
    const char *method = getenv("REQUEST_METHOD");
    char body[MAX_BODY_SIZE];
    char user[33];
    char token[65];
    char file_name[129];
    char file_md5[33];
    char upload_id[UPLOAD_ID_HEX_LENGTH + 1];
    uint64_t total_size;
    uint64_t requested_chunk_size;
    uint64_t chunks;
    NewChunkUploadSession session;
    CreateChunkSessionResult result = CREATE_CHUNK_SESSION_DATABASE_ERROR;
    int attempt;

    if (!method || strcmp(method, "POST") != 0 || read_body(body) != 0 ||
        json_get_string(body, "user", user, sizeof(user)) != 0 ||
        json_get_string(body, "token", token, sizeof(token)) != 0 ||
        json_get_string(body, "file_name", file_name, sizeof(file_name)) != 0 ||
        json_get_string(body, "md5", file_md5, sizeof(file_md5)) != 0 ||
        json_get_uint64(body, "total_size", &total_size) != 0 ||
        json_get_uint64(body, "chunk_size", &requested_chunk_size) != 0 ||
        !validate_username(user) || !validate_file_name(file_name) ||
        !validate_password_md5(file_md5) || total_size == 0 ||
        total_size > MAX_TOTAL_SIZE || requested_chunk_size < MIN_CHUNK_SIZE ||
        requested_chunk_size > MAX_CHUNK_SIZE) {
        write_json_response(CHUNK_INIT_INVALID_REQUEST,
                            "invalid chunk upload init request", NULL);
        return;
    }
    chunks = ((total_size - 1U) / requested_chunk_size) + 1U;
    if (chunks == 0 || chunks > MAX_TOTAL_CHUNKS) {
        write_json_response(CHUNK_INIT_INVALID_REQUEST,
                            "invalid chunk upload plan", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(CHUNK_INIT_TOKEN_ERROR, "token error", NULL);
        return;
    }

    memset(&session, 0, sizeof(session));
    session.upload_id = upload_id;
    session.user_name = user;
    session.file_name = file_name;
    session.file_md5 = file_md5;
    session.total_size = total_size;
    session.chunk_size = (unsigned int)requested_chunk_size;
    session.total_chunks = (unsigned int)chunks;
    session.expires_in_seconds = SESSION_TTL_SECONDS;
    for (attempt = 0; attempt < ID_GENERATION_ATTEMPTS; ++attempt) {
        if (generate_upload_id(upload_id) != 0) break;
        result = create_chunk_upload_session(&session);
        if (result != CREATE_CHUNK_SESSION_DUPLICATE) break;
    }
    if (result != CREATE_CHUNK_SESSION_OK) {
        write_json_response(CHUNK_INIT_DATABASE_ERROR,
                            "unable to create chunk upload session", NULL);
        return;
    }

    write_json_header();
    printf("{\"code\":0,\"msg\":\"chunk upload initialized\","
           "\"upload_id\":\"%s\",\"chunk_size\":%u,"
           "\"total_chunks\":%u,\"expires_in\":%u,\"uploaded_chunks\":[]}\n",
           upload_id, session.chunk_size, session.total_chunks,
           session.expires_in_seconds);
}

static int parse_uint64_text(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (!text || text[0] == '\0' || !value) return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_chunk_path(const char *path,
                            char upload_id[UPLOAD_ID_HEX_LENGTH + 1],
                            unsigned int *chunk_index)
{
    static const char prefix[] = "/api/uploads/";
    static const char separator[] = "/chunks/";
    const char *cursor;
    char *end;
    unsigned long parsed_index;
    size_t index;

    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0 || !chunk_index) return -1;
    cursor = path + sizeof(prefix) - 1;
    for (index = 0; index < UPLOAD_ID_HEX_LENGTH; ++index) {
        if (!isxdigit((unsigned char)cursor[index]) ||
            (cursor[index] >= 'A' && cursor[index] <= 'F')) return -1;
        upload_id[index] = cursor[index];
    }
    upload_id[UPLOAD_ID_HEX_LENGTH] = '\0';
    cursor += UPLOAD_ID_HEX_LENGTH;
    if (strncmp(cursor, separator, sizeof(separator) - 1) != 0) return -1;
    cursor += sizeof(separator) - 1;
    if (!isdigit((unsigned char)*cursor)) return -1;
    errno = 0;
    parsed_index = strtoul(cursor, &end, 10);
    if (errno != 0 || *end != '\0' || parsed_index > UINT_MAX) return -1;
    *chunk_index = (unsigned int)parsed_index;
    return 0;
}

static int receive_chunk_body(const char *directory, uint64_t expected_size,
                              const char *expected_md5, ReceivedUpload *received)
{
    UploadIntake intake;
    unsigned char buffer[CHUNK_READ_BUFFER_SIZE];
    uint64_t remaining = expected_size;

    if (upload_intake_open(&intake, directory, expected_size) != UPLOAD_INTAKE_OK) return -1;
    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        size_t count = fread(buffer, 1, wanted, stdin);

        if (count == 0 || upload_intake_write(&intake, buffer, count) != UPLOAD_INTAKE_OK) {
            upload_intake_abort(&intake);
            return -1;
        }
        remaining -= (uint64_t)count;
    }
    if (upload_intake_finish(&intake, expected_size, expected_md5, received) !=
        UPLOAD_INTAKE_OK) {
        upload_intake_abort(&intake);
        return -1;
    }
    return 0;
}

static void write_chunk_success(unsigned int chunk_index, int idempotent)
{
    write_json_header();
    printf("{\"code\":0,\"msg\":\"chunk uploaded\",\"chunk_index\":%u,"
           "\"idempotent\":%s}\n", chunk_index, idempotent ? "true" : "false");
}

static void handle_chunk_request(const char *path)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *user = getenv("HTTP_X_UPLOAD_USER");
    const char *token = getenv("HTTP_X_UPLOAD_TOKEN");
    const char *chunk_md5 = getenv("HTTP_X_CHUNK_MD5");
    const char *content_length_text = getenv("CONTENT_LENGTH");
    const char *chunk_root = runtime_config_get("CHUNK_UPLOAD_DIR", "/data/chunk-uploads");
    char upload_id[UPLOAD_ID_HEX_LENGTH + 1];
    char final_path[CHUNK_STORED_PATH_CAPACITY];
    unsigned int chunk_index;
    uint64_t content_length;
    uint64_t expected_size;
    ChunkUploadSessionPlan plan;
    FindChunkSessionResult find_result;
    ReceivedUpload received;
    ChunkUploadPart part;
    ReserveChunkPartResult reserve_result;
    int install_result;

    if (!method || strcmp(method, "PUT") != 0 ||
        parse_chunk_path(path, upload_id, &chunk_index) != 0 ||
        !validate_username(user) || !validate_password_md5(chunk_md5) ||
        parse_uint64_text(content_length_text, &content_length) != 0 ||
        content_length == 0 || content_length > MAX_CHUNK_SIZE) {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST, "invalid chunk request", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(CHUNK_UPLOAD_TOKEN_ERROR, "token error", NULL);
        return;
    }
    find_result = find_receiving_chunk_session(upload_id, user, &plan);
    if (find_result == FIND_CHUNK_SESSION_UNAVAILABLE) {
        write_json_response(CHUNK_UPLOAD_SESSION_UNAVAILABLE,
                            "chunk upload session unavailable", NULL);
        return;
    }
    if (find_result != FIND_CHUNK_SESSION_OK) {
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR, "database error", NULL);
        return;
    }
    if (chunk_index >= plan.total_chunks) {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST, "chunk index out of range", NULL);
        return;
    }
    expected_size = chunk_index + 1U == plan.total_chunks
        ? plan.total_size - (uint64_t)plan.chunk_size * (plan.total_chunks - 1U)
        : plan.chunk_size;
    if (content_length != expected_size || expected_size == 0) {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST, "chunk size mismatch", NULL);
        return;
    }
    if (chunk_storage_path(chunk_root, upload_id, chunk_index,
                           final_path, sizeof(final_path)) != 0) {
        write_json_response(CHUNK_UPLOAD_STORAGE_ERROR, "chunk storage unavailable", NULL);
        return;
    }
    if (receive_chunk_body(chunk_root, expected_size, chunk_md5, &received) != 0) {
        write_json_response(CHUNK_UPLOAD_RECEIVE_ERROR,
                            "chunk size or MD5 verification failed", NULL);
        return;
    }

    memset(&part, 0, sizeof(part));
    part.upload_id = upload_id;
    part.chunk_index = chunk_index;
    part.size = received.size;
    part.chunk_md5 = received.md5;
    part.stored_path = final_path;
    reserve_result = reserve_chunk_upload_part(&part);
    if (reserve_result == RESERVE_CHUNK_PART_CONFLICT) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_CONFLICT,
                            "chunk index already contains different content", NULL);
        return;
    }
    if (reserve_result == RESERVE_CHUNK_PART_DATABASE_ERROR) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR, "database error", NULL);
        return;
    }
    install_result = chunk_storage_install(received.path, final_path);
    if (install_result < 0) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_STORAGE_ERROR, "unable to store chunk", NULL);
        return;
    }
    if (reserve_result != RESERVE_CHUNK_PART_READY &&
        mark_chunk_upload_part_ready(&part) != 0) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR,
                            "unable to confirm stored chunk", NULL);
        return;
    }
    received_upload_discard(&received);
    write_chunk_success(chunk_index,
                        reserve_result != RESERVE_CHUNK_PART_NEW || install_result == 1);
}

int main(void)
{
    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        const char *path = getenv("SCRIPT_NAME");

        if (path && strcmp(path, "/api/uploads/init") == 0) {
            handle_init_request();
        } else {
            handle_chunk_request(path);
        }
    }
    return 0;
}
