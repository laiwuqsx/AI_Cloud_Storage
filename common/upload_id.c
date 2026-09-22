#include "upload_id.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int generate_upload_id(char output[UPLOAD_ID_HEX_LENGTH + 1])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[UPLOAD_ID_HEX_LENGTH / 2];
    size_t received = 0;
    size_t index;
    int descriptor;

    if (!output) return -1;
    descriptor = open("/dev/urandom", O_RDONLY);
    if (descriptor < 0) return -1;
    while (received < sizeof(bytes)) {
        ssize_t count = read(descriptor, bytes + received, sizeof(bytes) - received);

        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(descriptor);
            return -1;
        }
        received += (size_t)count;
    }
    close(descriptor);
    for (index = 0; index < sizeof(bytes); ++index) {
        output[index * 2] = hex[bytes[index] >> 4];
        output[index * 2 + 1] = hex[bytes[index] & 0x0f];
    }
    output[UPLOAD_ID_HEX_LENGTH] = '\0';
    return 0;
}

int validate_upload_id(const char *value)
{
    size_t index;

    if (!value || strlen(value) != UPLOAD_ID_HEX_LENGTH) return 0;
    for (index = 0; index < UPLOAD_ID_HEX_LENGTH; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return 0;
    }
    return 1;
}
