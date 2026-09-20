#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>

#include "file_repository.h"
#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "token_service.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096
#define MAX_FILES_PER_RESPONSE 100

static int read_body(char *body, size_t size)
{
    const char *length_text = getenv("CONTENT_LENGTH");
    long length = length_text ? strtol(length_text, NULL, 10) : 0;
    if (length <= 0 || (size_t)length >= size) return -1;
    if (fread(body, 1, (size_t)length, stdin) != (size_t)length) return -1;
    body[length] = '\0';
    return 0;
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

static void write_files(const UserFile *files, size_t count)
{
    size_t index;
    write_json_header();
    fputs("{\"code\":0,\"files\":[", stdout);
    for (index = 0; index < count; ++index) {
        if (index > 0) putchar(',');
        fputs("{\"md5\":", stdout); write_json_string(files[index].md5);
        fputs(",\"file_name\":", stdout); write_json_string(files[index].file_name);
        printf(",\"size\":%llu", files[index].size);
        printf(",\"shared_status\":%u", files[index].shared_status);
        fputs(",\"type\":", stdout); write_json_string(files[index].type);
        fputs(",\"create_time\":", stdout); write_json_string(files[index].create_time);
        putchar('}');
    }
    fputs("]}\n", stdout);
}

int main(void)
{
    char body[MAX_BODY_SIZE], user[33], token[65];
    UserFile files[MAX_FILES_PER_RESPONSE];
    size_t count;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        if (read_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "token", token, sizeof(token)) != 0 ||
            !validate_username(user)) {
            write_json_response(1, "invalid file list request", NULL);
            continue;
        }
        if (verify_session_token(user, token) != 0) {
            write_json_response(4, "token error", NULL);
            continue;
        }
        if (list_user_files(user, files, MAX_FILES_PER_RESPONSE, &count) != 0) {
            write_json_response(1, "database error", NULL);
            continue;
        }
        write_files(files, count);
    }
    return 0;
}
