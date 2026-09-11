#include "user_validation.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "md5.h"

static int is_valid_name(const char *value, size_t min_length, size_t max_length)
{
    size_t i;
    size_t length;
    if (!value) return 0;
    length = strlen(value);
    if (length < min_length || length > max_length) return 0;
    for (i = 0; i < length; ++i) {
        if (!isalnum((unsigned char)value[i]) && value[i] != '_') return 0;
    }
    return 1;
}

int validate_username(const char *value) { return is_valid_name(value, 3, 32); }
int validate_nickname(const char *value) { return is_valid_name(value, 2, 32); }

int validate_password_md5(const char *value)
{
    size_t i;
    if (!value || strlen(value) != 32) return 0;
    for (i = 0; i < 32; ++i) {
        if (!isxdigit((unsigned char)value[i])) return 0;
    }
    return 1;
}

int validate_file_name(const char *value)
{
    size_t i;
    size_t length;
    if (!value) return 0;
    length = strlen(value);
    if (length == 0 || length > 128) return 0;
    for (i = 0; i < length; ++i) {
        if ((unsigned char)value[i] < 0x20 || value[i] == '/' || value[i] == '\\') return 0;
    }
    return 1;
}

int create_salt(char output[33])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    size_t i;
    if (fd < 0 || read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    for (i = 0; i < sizeof(bytes); ++i) {
        output[i * 2] = hex[bytes[i] >> 4];
        output[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    output[32] = '\0';
    return 0;
}

void make_password_digest(const char *salt, const char *client_password_md5,
                          char output[33])
{
    char combined[65];
    snprintf(combined, sizeof(combined), "%s%s", salt, client_password_md5);
    md5_hex((const unsigned char *)combined, strlen(combined), output);
}
