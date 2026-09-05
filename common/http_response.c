#include "http_response.h"

#include <stdio.h>

void write_json_response(int code, const char *message, const char *token)
{
    printf("Content-Type: application/json\\r\\n\\r\\n");
    if (token) {
        printf("{\\\"code\\\":%d,\\\"msg\\\":\\\"%s\\\",\\\"token\\\":\\\"%s\\\"}\\n",
               code, message, token);
        return;
    }
    printf("{\\\"code\\\":%d,\\\"msg\\\":\\\"%s\\\"}\\n", code, message);
}
