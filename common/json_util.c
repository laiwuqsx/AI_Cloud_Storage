#include "json_util.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *skip_spaces(const char *p)
{
    while (p && *p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

int json_is_valid_object(const char *json)
{
    const char *start;
    const char *end;

    if (!json) {
        return 0;
    }
    start = skip_spaces(json);
    if (*start != '{') {
        return 0;
    }
    end = json + strlen(json);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        --end;
    }
    return end > start && *(end - 1) == '}';
}

int json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[128];
    const char *cursor;
    const char *value_start;
    size_t length = 0;

    if (!json || !key || !out || out_size == 0 || !json_is_valid_object(json)) {
        return -1;
    }

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return -1;
    }
    cursor = strstr(json, pattern);
    if (!cursor) {
        return -1;
    }
    cursor = skip_spaces(cursor + strlen(pattern));
    if (*cursor != ':') {
        return -1;
    }
    cursor = skip_spaces(cursor + 1);
    if (*cursor != '\"') {
        return -1;
    }

    value_start = ++cursor;
    while (*cursor && *cursor != '\"') {
        if (*cursor == '\\') {
            return -1; /* Escaped strings are intentionally not supported yet. */
        }
        ++cursor;
    }
    if (*cursor != '\"') {
        return -1;
    }
    length = (size_t)(cursor - value_start);
    if (length == 0 || length >= out_size) {
        return -1;
    }
    memcpy(out, value_start, length);
    out[length] = '\0';
    return 0;
}
