#ifndef AI_CLOUD_UPLOAD_SERVICE_H
#define AI_CLOUD_UPLOAD_SERVICE_H

#include "file_repository.h"
#include "storage_client.h"

typedef struct {
    void *context;
    RecordNewFileResult (*record)(void *context, const NewFileRecord *record);
    /* 1: exact committed record exists, 0: absent, -1: unable to confirm. */
    int (*confirm)(void *context, const char *user_name, const char *md5,
                   const char *storage_key);
    ClaimFileResult (*claim_existing)(void *context, const char *user_name,
                                      const char *md5, const char *file_name,
                                      StoredObject *stored_object);
    /* Persist a deletion that could not be completed immediately. */
    int (*schedule_cleanup)(void *context, const char *storage_key,
                            const char *reason, const char *last_error);
} UploadRepository;

typedef struct {
    const char *local_path;
    const char *user_name;
    const char *md5;
    const char *file_name;
    const char *type;
    unsigned long long size;
} FirstUploadRequest;

typedef enum {
    FIRST_UPLOAD_INVALID_ARGUMENT = -1,
    FIRST_UPLOAD_OK = 0,
    FIRST_UPLOAD_STORAGE_FAILED = 1,
    FIRST_UPLOAD_DATABASE_FAILED = 2,
    FIRST_UPLOAD_PHYSICAL_CONFLICT = 3,
    FIRST_UPLOAD_CLEANUP_FAILED = 4,
    FIRST_UPLOAD_COMMIT_UNRESOLVED = 5
} FirstUploadResult;

FirstUploadResult execute_first_upload(const StorageClient *storage,
                                       const UploadRepository *repository,
                                       const FirstUploadRequest *request,
                                       StoredObject *stored_object);

#endif
