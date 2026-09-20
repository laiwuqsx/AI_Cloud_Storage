#include "share_code.h"

#include <ctype.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#define SHARE_CODE_MIN_LENGTH 4
#define SHARE_CODE_MAX_LENGTH 12
#define SHARE_CODE_PBKDF2_ITERATIONS 120000

static void bytes_to_hex(const unsigned char *bytes, size_t length, char *output)
{
    static const char hex[] = "0123456789abcdef";
    size_t index;

    for (index = 0; index < length; ++index) {
        output[index * 2] = hex[bytes[index] >> 4];
        output[index * 2 + 1] = hex[bytes[index] & 0x0f];
    }
    output[length * 2] = '\0';
}

static int hex_to_bytes(const char *hex, size_t byte_length, unsigned char *output)
{
    size_t index;

    if (!hex || strlen(hex) != byte_length * 2) return -1;
    for (index = 0; index < byte_length * 2; ++index) {
        if (!isxdigit((unsigned char)hex[index]) ||
            (hex[index] >= 'A' && hex[index] <= 'F')) return -1;
    }
    for (index = 0; index < byte_length; ++index) {
        unsigned char high = (unsigned char)(hex[index * 2] <= '9'
            ? hex[index * 2] - '0' : hex[index * 2] - 'a' + 10);
        unsigned char low = (unsigned char)(hex[index * 2 + 1] <= '9'
            ? hex[index * 2 + 1] - '0' : hex[index * 2 + 1] - 'a' + 10);
        output[index] = (unsigned char)((high << 4) | low);
    }
    return 0;
}

static int random_bytes(unsigned char *output, size_t length)
{
    size_t offset = 0;
    int descriptor = open("/dev/urandom", O_RDONLY);

    if (descriptor < 0) return -1;
    while (offset < length) {
        ssize_t count = read(descriptor, output + offset, length - offset);
        if (count <= 0) {
            close(descriptor);
            return -1;
        }
        offset += (size_t)count;
    }
    close(descriptor);
    return 0;
}

int validate_share_code(const char *code)
{
    size_t index, length;

    if (!code) return 0;
    length = strlen(code);
    if (length < SHARE_CODE_MIN_LENGTH || length > SHARE_CODE_MAX_LENGTH) return 0;
    for (index = 0; index < length; ++index) {
        if (!isalnum((unsigned char)code[index])) return 0;
    }
    return 1;
}

int create_share_code_digest(const char *code,
                             char salt[SHARE_CODE_SALT_HEX_LENGTH + 1],
                             char hash[SHARE_CODE_HASH_HEX_LENGTH + 1])
{
    unsigned char salt_bytes[SHARE_CODE_SALT_HEX_LENGTH / 2];
    unsigned char digest[SHARE_CODE_HASH_HEX_LENGTH / 2];
    int result = -1;

    if (!validate_share_code(code) || !salt || !hash ||
        random_bytes(salt_bytes, sizeof(salt_bytes)) != 0) return -1;
    if (PKCS5_PBKDF2_HMAC(code, (int)strlen(code), salt_bytes,
                          (int)sizeof(salt_bytes), SHARE_CODE_PBKDF2_ITERATIONS,
                          EVP_sha256(), (int)sizeof(digest), digest) != 1) goto done;
    bytes_to_hex(salt_bytes, sizeof(salt_bytes), salt);
    bytes_to_hex(digest, sizeof(digest), hash);
    result = 0;

done:
    OPENSSL_cleanse(digest, sizeof(digest));
    OPENSSL_cleanse(salt_bytes, sizeof(salt_bytes));
    return result;
}

int verify_share_code_digest(const char *code, const char *salt,
                             const char *expected_hash)
{
    unsigned char salt_bytes[SHARE_CODE_SALT_HEX_LENGTH / 2];
    unsigned char expected[SHARE_CODE_HASH_HEX_LENGTH / 2];
    unsigned char actual[SHARE_CODE_HASH_HEX_LENGTH / 2];
    int matches = 0;

    if (!validate_share_code(code) ||
        hex_to_bytes(salt, sizeof(salt_bytes), salt_bytes) != 0 ||
        hex_to_bytes(expected_hash, sizeof(expected), expected) != 0) return 0;
    if (PKCS5_PBKDF2_HMAC(code, (int)strlen(code), salt_bytes,
                          (int)sizeof(salt_bytes), SHARE_CODE_PBKDF2_ITERATIONS,
                          EVP_sha256(), (int)sizeof(actual), actual) == 1) {
        matches = CRYPTO_memcmp(actual, expected, sizeof(actual)) == 0;
    }
    OPENSSL_cleanse(actual, sizeof(actual));
    OPENSSL_cleanse(expected, sizeof(expected));
    OPENSSL_cleanse(salt_bytes, sizeof(salt_bytes));
    return matches;
}
