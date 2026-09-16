#include "http_response.h"

#include "fcgi_stdio.h"

void write_json_response(int code, const char *message, const char *token)
{
    write_json_header();
    if (token) {
        printf("{\"code\":%d,\"msg\":\"%s\",\"token\":\"%s\"}\n",
               code, message, token);
        return;
    }
    printf("{\"code\":%d,\"msg\":\"%s\"}\n", code, message);
}

void write_json_header(void)
{
    printf("Content-Type: application/json\r\n\r\n");
}
