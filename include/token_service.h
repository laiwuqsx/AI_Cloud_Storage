#ifndef AI_CLOUD_TOKEN_SERVICE_H
#define AI_CLOUD_TOKEN_SERVICE_H

#include <stddef.h>

/* Generates an opaque token and stores token -> user in Redis with a TTL. */
int create_session_token(const char *user, char *token, size_t token_size);
int verify_session_token(const char *user, const char *token);

/* 0: revoked or already absent, 1: token belongs to another user, -1: Redis failure. */
int revoke_session_token(const char *user, const char *token);

#endif
