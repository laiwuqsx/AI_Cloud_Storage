#ifndef AI_CLOUD_MD5_H
#define AI_CLOUD_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t bits;
    unsigned char buffer[64];
} Md5Context;

void md5_init(Md5Context *context);
void md5_update(Md5Context *context, const unsigned char *input, size_t length);
void md5_final(Md5Context *context, char output[33]);

void md5_hex(const unsigned char *input, size_t length, char output[33]);

#endif
