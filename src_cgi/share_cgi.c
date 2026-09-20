#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_repository.h"
#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "share_access_service.h"
#include "share_id.h"
#include "token_service.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096

static int read_body(char *body, size_t size)
{
    const char *length_text = getenv("CONTENT_LENGTH");
    long length = length_text ? strtol(length_text, NULL, 10) : 0;

    if (length <= 0 || (size_t)length >= size) return -1;
    if (fread(body, 1, (size_t)length, stdin) != (size_t)length) return -1;
    body[length] = '\0';
    return 0;
}

static int get_share_id(const char *query, char output[SHARE_ID_HEX_LENGTH + 1])
{
    const char *cursor = query;

    if (!query || !output) return -1;
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        size_t segment_length = end ? (size_t)(end - cursor) : strlen(cursor);

        if (segment_length == 9 + SHARE_ID_HEX_LENGTH &&
            strncmp(cursor, "share_id=", 9) == 0) {
            memcpy(output, cursor + 9, SHARE_ID_HEX_LENGTH);
            output[SHARE_ID_HEX_LENGTH] = '\0';
            return validate_share_id(output) ? 0 : -1;
        }
        if (!end) break;
        cursor = end + 1;
    }
    return -1;
}

static void write_json_string(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");

    putchar('"');
    while (*cursor) {
        if (*cursor == '"' || *cursor == '\\') putchar('\\');
        if (*cursor == '\n') fputs("\\n", stdout);
        else if (*cursor == '\r') fputs("\\r", stdout);
        else if (*cursor == '\t') fputs("\\t", stdout);
        else if (*cursor >= 0x20) putchar(*cursor);
        ++cursor;
    }
    putchar('"');
}

static void write_share(const PublicShare *share)
{
    write_json_header();
    fputs("{\"code\":0,\"file_name\":", stdout);
    write_json_string(share->file_name);
    printf(",\"size\":%llu", share->size);
    fputs(",\"type\":", stdout);
    write_json_string(share->type);
    fputs(",\"expires_at\":", stdout);
    write_json_string(share->expires_at);
    printf(",\"requires_code\":%s", share->requires_code ? "true" : "false");
    fputs("}\n", stdout);
}

static int write_access_error(ShareAccessResult result)
{
    if (result == SHARE_ACCESS_GRANTED) return 0;
    if (result == SHARE_ACCESS_UNAVAILABLE) write_json_response(2, "share unavailable", NULL);
    else if (result == SHARE_ACCESS_CODE_REQUIRED) write_json_response(7, "share access code required", NULL);
    else if (result == SHARE_ACCESS_CODE_INVALID) write_json_response(7, "invalid share access code", NULL);
    else if (result == SHARE_ACCESS_RATE_LIMITED) write_json_response(8, "too many access code attempts", NULL);
    else write_json_response(6, "share access verification error", NULL);
    return -1;
}

static void handle_public_share(void)
{
    char share_id[SHARE_ID_HEX_LENGTH + 1];
    PublicShare share;
    int result;

    if (get_share_id(getenv("QUERY_STRING"), share_id) != 0) {
        write_json_response(1, "invalid share request", NULL);
        return;
    }
    result = find_active_share(share_id, &share);
    if (result == 0) {
        write_share(&share);
    } else if (result == 1) {
        write_json_response(2, "share unavailable", NULL);
    } else {
        write_json_response(6, "database error", NULL);
    }
}

static void handle_save_share(void)
{
    char body[MAX_BODY_SIZE];
    char user[33], token[65], share_id[SHARE_ID_HEX_LENGTH + 1];
    char access_code[MAX_BODY_SIZE];
    SaveSharedFileResult result;
    ShareAccessResult access_result;

    if (read_body(body, sizeof(body)) != 0 ||
        json_get_string(body, "user", user, sizeof(user)) != 0 ||
        json_get_string(body, "token", token, sizeof(token)) != 0 ||
        json_get_string(body, "share_id", share_id, sizeof(share_id)) != 0 ||
        !validate_username(user) || !validate_share_id(share_id)) {
        write_json_response(3, "invalid save request", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(4, "token error", NULL);
        return;
    }
    access_code[0] = '\0';
    json_get_string(body, "access_code", access_code, sizeof(access_code));
    access_result = authorize_share_access(share_id, access_code, user);
    if (write_access_error(access_result) != 0) return;
    result = save_shared_file(user, share_id);
    if (result == SAVE_SHARED_FILE_SAVED) {
        write_json_response(0, "file saved", NULL);
    } else if (result == SAVE_SHARED_FILE_ALREADY_OWNED) {
        write_json_response(0, "file already saved", NULL);
    } else if (result == SAVE_SHARED_FILE_UNAVAILABLE) {
        write_json_response(2, "share unavailable", NULL);
    } else {
        write_json_response(6, "database error", NULL);
    }
}

int main(void)
{
    const char *method;
    const char *script_name;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        method = getenv("REQUEST_METHOD");
        script_name = getenv("SCRIPT_NAME");
        if (script_name && strcmp(script_name, "/api/share") == 0 &&
            method && strcmp(method, "GET") == 0) {
            handle_public_share();
        } else if (script_name && strcmp(script_name, "/api/share/save") == 0 &&
                   method && strcmp(method, "POST") == 0) {
            handle_save_share();
        } else if ((script_name && strcmp(script_name, "/api/share") == 0) ||
                   (script_name && strcmp(script_name, "/api/share/save") == 0)) {
            write_json_response(1, "method not allowed", NULL);
        } else {
            write_json_response(1, "unknown share route", NULL);
        }
    }
    return 0;
}
