#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_response.h"
#include "json_util.h"

#define MAX_BODY_SIZE 4096

static int read_request_body(char *body, size_t body_size)
{
    const char *length_text = getenv("CONTENT_LENGTH");
    long length;
    size_t read_size;

    if (!length_text || !body || body_size == 0) {
        return -1;
    }
    length = strtol(length_text, NULL, 10);
    if (length <= 0 || (size_t)length >= body_size) {
        return -1;
    }
    read_size = fread(body, 1, (size_t)length, stdin);
    if (read_size != (size_t)length) {
        return -1;
    }
    body[read_size] = '\0';
    return 0;
}

int main(void)
{
    char body[MAX_BODY_SIZE];
    char user[64];
    char password[128];
    char token[128];

    while (FCGI_Accept() >= 0) {
        if (read_request_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "password", password, sizeof(password)) != 0) {
            write_json_response(1, "invalid login request", NULL);
            continue;
        }

        /* First vertical slice: request validation and response only.
         * Next step replaces this with MySQL password verification and Redis token storage. */
        if (strcmp(password, "demo-pass") != 0) {
            write_json_response(2, "invalid credentials", NULL);
            continue;
        }
        snprintf(token, sizeof(token), "demo-token-%s", user);
        write_json_response(0, "login accepted", token);
    }
    return 0;
}
