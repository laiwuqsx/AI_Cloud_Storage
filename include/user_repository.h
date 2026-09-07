#ifndef AI_CLOUD_USER_REPOSITORY_H
#define AI_CLOUD_USER_REPOSITORY_H

/* 0: success, 1: duplicate user/nickname, -1: database failure. */
int create_user(const char *user, const char *nickname, const char *password_digest,
                const char *salt);

#endif
