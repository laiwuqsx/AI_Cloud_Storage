#include "fcgi_stdio.h"

#include <errno.h>
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
#define DEFAULT_SHARE_TTL_SECONDS 604800U
#define MAX_SHARE_TTL_SECONDS 31536000U

static int read_body(char *body, size_t size)
{
    const char *length_text = getenv("CONTENT_LENGTH");
    long length = length_text ? strtol(length_text, NULL, 10) : 0;
    if (length <= 0 || (size_t)length >= size) return -1;
    if (fread(body, 1, (size_t)length, stdin) != (size_t)length) return -1;
    body[length] = '\0';
    return 0;
}

static int get_command(const char *query)
{
    const char *cursor = query;
    if (!cursor) return 0;
    while (*cursor) {
        if (strncmp(cursor, "cmd=del", 7) == 0 &&
            (cursor[7] == '\0' || cursor[7] == '&')) return 1;
        if (strncmp(cursor, "cmd=share", 9) == 0 &&
            (cursor[9] == '\0' || cursor[9] == '&')) return 2;
        if (strncmp(cursor, "cmd=unshare", 11) == 0 &&
            (cursor[11] == '\0' || cursor[11] == '&')) return 3;
        cursor = strchr(cursor, '&');
        if (!cursor) break;
        ++cursor;
    }
    return 0;
}

static int get_share_ttl(unsigned int *ttl)
{
    const char *text = runtime_config_get("SHARE_TTL_SECONDS", "604800");
    char *end = NULL;
    unsigned long value;

    if (!ttl || !text || *text == '\0') return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0 ||
        value > MAX_SHARE_TTL_SECONDS) return -1;
    *ttl = (unsigned int)value;
    return 0;
}

int main(void)
{
    char body[MAX_BODY_SIZE], user[33], token[65], md5[33];
    char share_id[SHARE_ID_HEX_LENGTH + 1];
    unsigned int share_ttl = DEFAULT_SHARE_TTL_SECONDS;
    int result, command;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        if (!getenv("REQUEST_METHOD") || strcmp(getenv("REQUEST_METHOD"), "POST") != 0) {
            write_json_response(1, "method not allowed", NULL);
            continue;
        }
        command = get_command(getenv("QUERY_STRING"));
        if (command == 0) {
            write_json_response(1, "unknown file command", NULL);
            continue;
        }
        if (read_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "token", token, sizeof(token)) != 0 ||
            json_get_string(body, "md5", md5, sizeof(md5)) != 0 ||
            !validate_username(user) || !validate_password_md5(md5)) {
            write_json_response(1, "invalid file command request", NULL);
            continue;
        }
        if (verify_session_token(user, token) != 0) {
            write_json_response(4, "token error", NULL);
            continue;
        }
        if (command == 1) {
            result = remove_user_file(user, md5);
            if (result == 0) {
                write_json_response(0, "file removed from user list", NULL);
            } else if (result == 1) {
                write_json_response(1, "file not found in user list", NULL);
            } else {
                write_json_response(1, "database error", NULL);
            }
        } else if (command == 2) {
            if (get_share_ttl(&share_ttl) != 0 || generate_share_id(share_id) != 0) {
                write_json_response(1, "share configuration error", NULL);
                continue;
            }
            result = share_user_file(user, md5, share_id, share_ttl);
            if (result == 0) {
                write_json_header();
                printf("{\"code\":0,\"msg\":\"file shared\","
                       "\"share_id\":\"%s\",\"expires_in\":%u}\n",
                       share_id, share_ttl);
            } else if (result == 1) {
                write_json_response(1, "file not found in user list", NULL);
            } else if (result == 2) {
                write_json_response(5, "file already shared", NULL);
            } else {
                write_json_response(1, "database error", NULL);
            }
        } else {
            result = unshare_user_file(user, md5);
            if (result == 0) {
                write_json_response(0, "file sharing cancelled", NULL);
            } else if (result == 1) {
                write_json_response(1, "file is not shared by this user", NULL);
            } else {
                write_json_response(1, "database error", NULL);
            }
        }
    }
    return 0;
}
