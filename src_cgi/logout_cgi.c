#include "fcgi_stdio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_response.h"
#include "json_util.h"
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

static int valid_token(const char *token)
{
    size_t index;

    if (!token || strlen(token) != 64) return 0;
    for (index = 0; index < 64; ++index) {
        if (!isxdigit((unsigned char)token[index])) return 0;
    }
    return 1;
}

int main(void)
{
    char body[MAX_BODY_SIZE], user[33], token[65];
    const char *method;
    int result;

    while (FCGI_Accept() >= 0) {
        method = getenv("REQUEST_METHOD");
        if (!method || strcmp(method, "POST") != 0 ||
            read_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "token", token, sizeof(token)) != 0 ||
            !validate_username(user) || !valid_token(token)) {
            write_json_response(1, "invalid logout request", NULL);
            continue;
        }

        result = revoke_session_token(user, token);
        if (result == 0) write_json_response(0, "logout complete", NULL);
        else if (result == 1) write_json_response(4, "token error", NULL);
        else write_json_response(1, "session service error", NULL);
    }
    return 0;
}
