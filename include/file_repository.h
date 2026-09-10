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
} UserFile;

/* 0: success, -1: database failure. */
int list_user_files(const char *user, UserFile *files, size_t capacity, size_t *count);

#endif
