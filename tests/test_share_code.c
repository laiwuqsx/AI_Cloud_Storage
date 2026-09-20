#include <stdio.h>
#include <string.h>

#include "share_code.h"

static int failures = 0;

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

int main(void)
{
    char first_salt[SHARE_CODE_SALT_HEX_LENGTH + 1];
    char first_hash[SHARE_CODE_HASH_HEX_LENGTH + 1];
    char second_salt[SHARE_CODE_SALT_HEX_LENGTH + 1];
    char second_hash[SHARE_CODE_HASH_HEX_LENGTH + 1];

    EXPECT(validate_share_code("A7b9"), "accept four-character alphanumeric code");
    EXPECT(validate_share_code("Abc123xyz789"), "accept twelve-character code");
    EXPECT(!validate_share_code("123"), "reject short code");
    EXPECT(!validate_share_code("1234567890123"), "reject long code");
    EXPECT(!validate_share_code("ab-12"), "reject punctuation");

    EXPECT(create_share_code_digest("A7b9", first_salt, first_hash) == 0,
           "create first digest");
    EXPECT(create_share_code_digest("A7b9", second_salt, second_hash) == 0,
           "create second digest");
    EXPECT(strlen(first_salt) == SHARE_CODE_SALT_HEX_LENGTH, "salt length");
    EXPECT(strlen(first_hash) == SHARE_CODE_HASH_HEX_LENGTH, "hash length");
    EXPECT(strcmp(first_salt, second_salt) != 0, "random salts differ");
    EXPECT(strcmp(first_hash, second_hash) != 0, "salted hashes differ");
    EXPECT(verify_share_code_digest("A7b9", first_salt, first_hash),
           "correct code verifies");
    EXPECT(!verify_share_code_digest("B7b9", first_salt, first_hash),
           "incorrect code rejected");
    first_hash[0] = 'A';
    EXPECT(!verify_share_code_digest("A7b9", first_salt, first_hash),
           "non-canonical digest rejected");

    if (failures) return 1;
    puts("share code tests passed");
    return 0;
}
