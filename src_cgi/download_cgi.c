#include "fcgi_stdio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_repository.h"
#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "share_id.h"
#include "token_service.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096
#define INTERNAL_STORAGE_PREFIX "/_internal_storage/"

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

static int valid_storage_key(const char *value)
{
    static const char required_prefix[] = "group1/M00/";
    size_t index;

    if (!value || strncmp(value, required_prefix, sizeof(required_prefix) - 1) != 0 ||
        value[sizeof(required_prefix) - 1] == '\0' || strstr(value, "..")) return 0;
    for (index = 0; value[index]; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (!isalnum(character) && character != '/' && character != '_' &&
            character != '-' && character != '.') return 0;
    }
    return 1;
}

static void write_download(const DownloadFile *file)
{
    if (!file || !valid_storage_key(file->storage_key)) {
        write_json_response(6, "invalid storage location", NULL);
        return;
    }
    printf("Content-Type: application/octet-stream\r\n");
    printf("Content-Disposition: attachment\r\n");
    printf("X-Content-Type-Options: nosniff\r\n");
    printf("Cache-Control: private, no-store\r\n");
    printf("X-Accel-Redirect: %s%s\r\n\r\n",
           INTERNAL_STORAGE_PREFIX, file->storage_key);
}

static void handle_owned_download(void)
{
    char body[MAX_BODY_SIZE], user[33], token[65], md5[33];
    DownloadFile file;
    int result;

    if (read_body(body, sizeof(body)) != 0 ||
        json_get_string(body, "user", user, sizeof(user)) != 0 ||
        json_get_string(body, "token", token, sizeof(token)) != 0 ||
        json_get_string(body, "md5", md5, sizeof(md5)) != 0 ||
        !validate_username(user) || !validate_password_md5(md5)) {
        write_json_response(3, "invalid download request", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(4, "token error", NULL);
        return;
    }
    result = authorize_owned_download(user, md5, &file);
    if (result == 0) write_download(&file);
    else if (result == 1) write_json_response(2, "file unavailable", NULL);
    else write_json_response(6, "database error", NULL);
}

static void handle_share_download(void)
{
    char share_id[SHARE_ID_HEX_LENGTH + 1];
    DownloadFile file;
    int result;

    if (get_share_id(getenv("QUERY_STRING"), share_id) != 0) {
        write_json_response(3, "invalid share download request", NULL);
        return;
    }
    result = authorize_share_download(share_id, &file);
    if (result == 0) write_download(&file);
    else if (result == 1) write_json_response(2, "share unavailable", NULL);
    else write_json_response(6, "database error", NULL);
}

int main(void)
{
    const char *method;
    const char *script_name;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        method = getenv("REQUEST_METHOD");
        script_name = getenv("SCRIPT_NAME");
        if (script_name && strcmp(script_name, "/api/download") == 0 &&
            method && strcmp(method, "POST") == 0) {
            handle_owned_download();
        } else if (script_name && strcmp(script_name, "/api/share/download") == 0 &&
                   method && strcmp(method, "GET") == 0) {
            handle_share_download();
        } else if ((script_name && strcmp(script_name, "/api/download") == 0) ||
                   (script_name && strcmp(script_name, "/api/share/download") == 0)) {
            write_json_response(1, "method not allowed", NULL);
        } else {
            write_json_response(1, "unknown download route", NULL);
        }
    }
    return 0;
}
