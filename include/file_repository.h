#ifndef AI_CLOUD_FILE_REPOSITORY_H
#define AI_CLOUD_FILE_REPOSITORY_H

#include <stddef.h>

typedef struct {
    char md5[33];
    char file_name[129];
    char url[513];
    char type[33];
    char create_time[32];
    unsigned long long size;
    unsigned int shared_status;
} UserFile;

typedef struct {
    char user_name[33];
    char md5[33];
    char file_name[129];
    char url[513];
    char type[33];
    char create_time[32];
    unsigned long long size;
    unsigned long pv;
} SharedFile;

typedef enum {
    CLAIM_FILE_DATABASE_FAILURE = -1,
    CLAIM_FILE_LINKED = 0,
    CLAIM_FILE_PHYSICAL_MISSING = 1,
    CLAIM_FILE_ALREADY_OWNED = 2
} ClaimFileResult;

/* 0: success, -1: database failure. */
int list_user_files(const char *user, UserFile *files, size_t capacity, size_t *count);

ClaimFileResult claim_existing_file(const char *user, const char *md5, const char *file_name);

/* 0: removed, 1: user-file relationship does not exist, -1: database failure. */
int remove_user_file(const char *user, const char *md5);

/* 0: shared, 1: user-file relationship does not exist, 2: already shared, -1: database failure. */
int share_user_file(const char *user, const char *md5);

/* 0: sharing cancelled, 1: file is not shared by this user, -1: database failure. */
int unshare_user_file(const char *user, const char *md5);

#endif
