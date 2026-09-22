#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "json_util.h"

int main(void)
{
    char user[32];
    char password[32];
    uint64_t total_size;
    const char *request =
        " { \"user\" : \"alice\", \"password\" : \"demo-pass\", "
        "\"total_size\": 10737418240 } ";

    assert(json_is_valid_object(request));
    assert(json_get_string(request, "user", user, sizeof(user)) == 0);
    assert(json_get_string(request, "password", password, sizeof(password)) == 0);
    assert(strcmp(user, "alice") == 0);
    assert(strcmp(password, "demo-pass") == 0);
    assert(json_get_uint64(request, "total_size", &total_size) == 0);
    assert(total_size == 10737418240ULL);
    assert(json_get_uint64("{\"value\":-1}", "value", &total_size) == -1);
    assert(json_get_uint64("{\"value\":12x}", "value", &total_size) == -1);
    assert(json_get_string(request, "token", user, sizeof(user)) == -1);
    assert(!json_is_valid_object("not-json"));
    puts("json_util tests passed");
    return 0;
}
