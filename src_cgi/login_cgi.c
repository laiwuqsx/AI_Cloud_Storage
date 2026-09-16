#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "token_service.h"
#include "user_repository.h"
#include "user_validation.h"

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
    char user[33];
    char password[33];
    char expected_digest[33];
    char token[65];
    UserCredentials credentials;
    int lookup_result;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        if (read_request_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "password", password, sizeof(password)) != 0 ||
            !validate_username(user) || !validate_password_md5(password)) {
            write_json_response(1, "invalid login request", NULL);
            continue;
        }

        lookup_result = find_user_credentials(user, &credentials);
        if (lookup_result == 1) {
            write_json_response(2, "invalid credentials", NULL);
            continue;
        }
        if (lookup_result != 0) {
            write_json_response(1, "database error", NULL);
            continue;
        }
        make_password_digest(credentials.salt, password, expected_digest);
        if (strcmp(expected_digest, credentials.password_digest) != 0) {
            write_json_response(2, "invalid credentials", NULL);
            continue;
        }
        if (create_session_token(user, token, sizeof(token)) != 0) {
            write_json_response(1, "session creation failed", NULL);
            continue;
        }
        write_json_response(0, "login accepted", token);
    }
    return 0;
}
