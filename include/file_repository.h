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

typedef struct {
    char file_name[129];
    char type[33];
    char expires_at[32];
    unsigned long long size;
} PublicShare;

typedef struct {
    char storage_key[257];
    char file_name[129];
} DownloadFile;

typedef enum {
    CLAIM_FILE_DATABASE_FAILURE = -1,
    CLAIM_FILE_LINKED = 0,
    CLAIM_FILE_PHYSICAL_MISSING = 1,
    CLAIM_FILE_ALREADY_OWNED = 2
} ClaimFileResult;

typedef struct {
    char storage_key[257];
    char url[513];
} FileLocation;

typedef struct {
    const char *user_name;
    const char *md5;
    const char *file_name;
    const char *storage_key;
    const char *url;
    const char *type;
    unsigned long long size;
} NewFileRecord;

typedef enum {
    RECORD_NEW_FILE_INVALID_ARGUMENT = -3,
    RECORD_NEW_FILE_COMMIT_UNKNOWN = -2,
    RECORD_NEW_FILE_DATABASE_FAILURE = -1,
    RECORD_NEW_FILE_CREATED = 0,
    RECORD_NEW_FILE_PHYSICAL_CONFLICT = 1
} RecordNewFileResult;

typedef enum {
    SAVE_SHARED_FILE_DATABASE_FAILURE = -1,
    SAVE_SHARED_FILE_SAVED = 0,
    SAVE_SHARED_FILE_UNAVAILABLE = 1,
    SAVE_SHARED_FILE_ALREADY_OWNED = 2
} SaveSharedFileResult;

/* 0: success, -1: database failure. */
int list_user_files(const char *user, UserFile *files, size_t capacity, size_t *count);

/* Atomically creates file_info(reference_count=1) and the uploader's user_file_list row. */
RecordNewFileResult record_new_file_upload(const NewFileRecord *record);

/* 1: exact committed file and owner rows exist, 0: absent, -1: database failure. */
int confirm_new_file_upload(const char *user_name, const char *md5, const char *storage_key);

/* 1: the user owns this logical file, 0: not owned, -1: database failure. */
int user_owns_file(const char *user, const char *md5);

ClaimFileResult claim_existing_file(const char *user, const char *md5, const char *file_name);

/* Same atomic claim operation, also returning the already committed physical object. */
ClaimFileResult claim_existing_file_with_location(const char *user, const char *md5,
                                                  const char *file_name,
                                                  FileLocation *location);

/* 0: removed, 1: user-file relationship does not exist, -1: database failure. */
int remove_user_file(const char *user, const char *md5);

/* 0: shared, 1: user-file relationship does not exist, 2: already shared, -1: database failure. */
int share_user_file(const char *user, const char *md5, const char *share_id,
                    unsigned int expires_in_seconds);

/* 0: sharing cancelled, 1: file is not shared by this user, -1: database failure. */
int unshare_user_file(const char *user, const char *md5);

/* 0: active share found, 1: missing/revoked/expired, -1: database failure. */
int find_active_share(const char *share_id, PublicShare *share);

/* Atomically validates an active share, creates the recipient relation, and increments its reference count. */
SaveSharedFileResult save_shared_file(const char *user, const char *share_id);

/* Authorizes a private download and increments that user-file row's request counter. */
int authorize_owned_download(const char *user, const char *md5, DownloadFile *file);

/* Authorizes an active share download and increments that share's request counter. */
int authorize_share_download(const char *share_id, DownloadFile *file);

#endif
