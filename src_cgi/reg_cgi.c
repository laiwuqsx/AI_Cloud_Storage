#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>

#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "user_repository.h"
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

int main(void)
{
    char body[MAX_BODY_SIZE], user[33], nickname[33], password[33], salt[33], digest[33];
    int result;

    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        if (read_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "nickname", nickname, sizeof(nickname)) != 0 ||
            json_get_string(body, "password", password, sizeof(password)) != 0 ||
            !validate_username(user) || !validate_nickname(nickname) ||
            !validate_password_md5(password)) {
            write_json_response(1, "invalid registration request", NULL);
            continue;
        }
        if (create_salt(salt) != 0) {
            write_json_response(1, "cannot create password salt", NULL);
            continue;
        }
        make_password_digest(salt, password, digest);
        result = create_user(user, nickname, digest, salt);
        if (result == 0) write_json_response(0, "registered", NULL);
        else if (result == 1) write_json_response(2, "user or nickname already exists", NULL);
        else write_json_response(1, "database error", NULL);
    }
    return 0;
}
