#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "md5.h"
#include "user_validation.h"

int main(void)
{
    char hash[33], digest[33];
    md5_hex((const unsigned char *)"abc", 3, hash);
    assert(strcmp(hash, "900150983cd24fb0d6963f7d28e17f72") == 0);
    assert(validate_username("alice_01"));
    assert(!validate_username("a!"));
    assert(validate_nickname("Alice"));
    assert(!validate_nickname("a"));
    assert(validate_password_md5("900150983cd24fb0d6963f7d28e17f72"));
    assert(!validate_password_md5("not-an-md5"));
    assert(validate_file_name("project notes.txt"));
    assert(!validate_file_name("../secret.txt"));
    make_password_digest("0123456789abcdef0123456789abcdef",
                         "900150983cd24fb0d6963f7d28e17f72", digest);
    assert(strlen(digest) == 32);
    assert(strcmp(digest, "6ed96e8403c79174a7098adf240a7353") == 0);
    puts("auth helper tests passed");
    return 0;
}
