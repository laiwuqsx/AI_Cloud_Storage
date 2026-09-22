#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "upload_id.h"

int main(void)
{
    char first[UPLOAD_ID_HEX_LENGTH + 1];
    char second[UPLOAD_ID_HEX_LENGTH + 1];

    assert(generate_upload_id(first) == 0);
    assert(generate_upload_id(second) == 0);
    assert(validate_upload_id(first));
    assert(validate_upload_id(second));
    assert(strcmp(first, second) != 0);
    assert(!validate_upload_id("short"));
    first[0] = 'G';
    assert(!validate_upload_id(first));
    puts("upload id tests passed");
    return 0;
}
