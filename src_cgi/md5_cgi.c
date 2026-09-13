#include "fcgi_stdio.h"

#include <stdio.h>
#include <stdlib.h>

#include "file_repository.h"
#include "http_response.h"
#include "json_util.h"
#include "token_service.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096

enum {
    MD5_RESPONSE_OK = 0,
    MD5_RESPONSE_NEEDS_UPLOAD = 1,
    MD5_RESPONSE_INVALID_REQUEST = 3,
    MD5_RESPONSE_TOKEN_ERROR = 4,
    MD5_RESPONSE_ALREADY_OWNED = 5,
    MD5_RESPONSE_DATABASE_ERROR = 6
};

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
    char body[MAX_BODY_SIZE], user[33], token[65], md5[33], file_name[129];
    ClaimFileResult result;

    while (FCGI_Accept() >= 0) {
        if (read_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "token", token, sizeof(token)) != 0 ||
            json_get_string(body, "md5", md5, sizeof(md5)) != 0 ||
            json_get_string(body, "file_name", file_name, sizeof(file_name)) != 0 ||
            !validate_username(user) || !validate_password_md5(md5) ||
            !validate_file_name(file_name)) {
            write_json_response(MD5_RESPONSE_INVALID_REQUEST, "invalid instant upload request", NULL);
            continue;
        }
        if (verify_session_token(user, token) != 0) {
            write_json_response(MD5_RESPONSE_TOKEN_ERROR, "token error", NULL);
            continue;
        }

        result = claim_existing_file(user, md5, file_name);
        if (result == CLAIM_FILE_LINKED) {
            write_json_response(MD5_RESPONSE_OK, "instant upload complete", NULL);
        } else if (result == CLAIM_FILE_PHYSICAL_MISSING) {
            write_json_response(MD5_RESPONSE_NEEDS_UPLOAD, "physical file not found", NULL);
        } else if (result == CLAIM_FILE_ALREADY_OWNED) {
            write_json_response(MD5_RESPONSE_ALREADY_OWNED, "user already owns this file", NULL);
        } else {
            write_json_response(MD5_RESPONSE_DATABASE_ERROR, "database error", NULL);
        }
    }
    return 0;
}
