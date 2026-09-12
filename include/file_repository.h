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

/* 0: success, -1: database failure. */
int list_user_files(const char *user, UserFile *files, size_t capacity, size_t *count);

/* 0: linked, 1: physical file missing, 2: user already owns the file, -1: database failure. */
int claim_existing_file(const char *user, const char *md5, const char *file_name);

/* 0: removed, 1: user-file relationship does not exist, -1: database failure. */
int remove_user_file(const char *user, const char *md5);

/* 0: shared, 1: user-file relationship does not exist, 2: already shared, -1: database failure. */
int share_user_file(const char *user, const char *md5);

#endif
