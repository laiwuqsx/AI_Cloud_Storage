#include "fcgi_stdio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk_upload_repository.h"
#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "token_service.h"
#include "upload_id.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096
#define MIN_CHUNK_SIZE (1024U * 1024U)
#define MAX_CHUNK_SIZE (20U * 1024U * 1024U)
#define MAX_TOTAL_SIZE (50ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_TOTAL_CHUNKS 10000U
#define SESSION_TTL_SECONDS 86400U
#define ID_GENERATION_ATTEMPTS 3

enum {
    CHUNK_INIT_OK = 0,
    CHUNK_INIT_INVALID_REQUEST = 1,
    CHUNK_INIT_TOKEN_ERROR = 2,
    CHUNK_INIT_DATABASE_ERROR = 3
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

int main(void)
{
    runtime_config_init();
    while (FCGI_Accept() >= 0) handle_init_request();
    return 0;
}
