#ifndef AI_CLOUD_SHARE_CODE_H
#define AI_CLOUD_SHARE_CODE_H

#define SHARE_CODE_SALT_HEX_LENGTH 32
#define SHARE_CODE_HASH_HEX_LENGTH 64

int validate_share_code(const char *code);
int create_share_code_digest(const char *code,
                             char salt[SHARE_CODE_SALT_HEX_LENGTH + 1],
                             char hash[SHARE_CODE_HASH_HEX_LENGTH + 1]);
int verify_share_code_digest(const char *code, const char *salt,
                             const char *expected_hash);

#endif
