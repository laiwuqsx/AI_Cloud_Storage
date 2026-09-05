#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "json_util.h"

int main(void)
{
    char user[32];
    char password[32];
    const char *request = " { \"user\" : \"alice\", \"password\" : \"demo-pass\" } ";

    assert(json_is_valid_object(request));
    assert(json_get_string(request, "user", user, sizeof(user)) == 0);
    assert(json_get_string(request, "password", password, sizeof(password)) == 0);
    assert(strcmp(user, "alice") == 0);
    assert(strcmp(password, "demo-pass") == 0);
    assert(json_get_string(request, "token", user, sizeof(user)) == -1);
    assert(!json_is_valid_object("not-json"));
    puts("json_util tests passed");
    return 0;
}
