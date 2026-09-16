#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_repository.h"
#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
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

int main(void)
{
    char body[MAX_BODY_SIZE], user[33], token[65], md5[33];
    int result, command;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
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
            result = share_user_file(user, md5);
            if (result == 0) {
                write_json_response(0, "file marked as shared", NULL);
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
