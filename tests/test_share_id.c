#include <stdio.h>
#include <string.h>

#include "share_id.h"

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char first[SHARE_ID_HEX_LENGTH + 1];
    char second[SHARE_ID_HEX_LENGTH + 1];

    EXPECT(generate_share_id(first) == 0, "generate first share id");
    EXPECT(generate_share_id(second) == 0, "generate second share id");
    EXPECT(validate_share_id(first), "first share id format");
    EXPECT(validate_share_id(second), "second share id format");
    EXPECT(strcmp(first, second) != 0, "share ids should be independently random");
    EXPECT(!validate_share_id("short"), "reject short share id");
    first[10] = 'z';
    EXPECT(!validate_share_id(first), "reject non-hex share id");
    first[10] = 'A';
    EXPECT(!validate_share_id(first), "reject non-canonical uppercase share id");
    puts("share id tests passed");
    return 0;
}
