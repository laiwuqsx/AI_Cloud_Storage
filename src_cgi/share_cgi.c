#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_repository.h"
#include "http_response.h"
#include "runtime_config.h"
#include "share_id.h"

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
    fputs("}\n", stdout);
}

int main(void)
{
    char share_id[SHARE_ID_HEX_LENGTH + 1];
    PublicShare share;
    int result;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        if (!getenv("REQUEST_METHOD") || strcmp(getenv("REQUEST_METHOD"), "GET") != 0) {
            write_json_response(1, "method not allowed", NULL);
            continue;
        }
        if (get_share_id(getenv("QUERY_STRING"), share_id) != 0) {
            write_json_response(1, "invalid share request", NULL);
            continue;
        }
        result = find_active_share(share_id, &share);
        if (result == 0) {
            write_share(&share);
        } else if (result == 1) {
            write_json_response(2, "share unavailable", NULL);
        } else {
            write_json_response(1, "database error", NULL);
        }
    }
    return 0;
}
