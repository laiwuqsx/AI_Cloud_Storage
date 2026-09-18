#include "upload_service.h"

#include <string.h>

static int valid_request(const FirstUploadRequest *request)
{
    return request && request->local_path && request->local_path[0] != '\0' &&
           request->user_name && request->user_name[0] != '\0' &&
           request->md5 && strlen(request->md5) == 32 &&
           request->file_name && request->file_name[0] != '\0' &&
           request->type;
}

static FirstUploadResult remove_uploaded_object(const StorageClient *storage,
                                                const StoredObject *stored,
                                                FirstUploadResult result_after_cleanup)
{
    if (storage_client_remove(storage, stored->storage_key) != 0) {
        return FIRST_UPLOAD_CLEANUP_FAILED;
    }
    return result_after_cleanup;
}

FirstUploadResult execute_first_upload(const StorageClient *storage,
                                       const UploadRepository *repository,
                                       const FirstUploadRequest *request,
                                       StoredObject *stored_object)
{
    StoredObject uploaded;
    NewFileRecord record;
    RecordNewFileResult record_result;
    int confirmation;

    if (!storage || !repository || !repository->record || !repository->confirm ||
        !repository->claim_existing || !valid_request(request)) {
        return FIRST_UPLOAD_INVALID_ARGUMENT;
    }
    if (storage_client_upload(storage, request->local_path, &uploaded) != 0) {
        return FIRST_UPLOAD_STORAGE_FAILED;
    }

    record.user_name = request->user_name;
    record.md5 = request->md5;
    record.file_name = request->file_name;
    record.storage_key = uploaded.storage_key;
    record.url = uploaded.url;
    record.type = request->type;
    record.size = request->size;
    record_result = repository->record(repository->context, &record);

    if (record_result == RECORD_NEW_FILE_CREATED) {
        if (stored_object) *stored_object = uploaded;
        return FIRST_UPLOAD_OK;
    }
    if (record_result == RECORD_NEW_FILE_PHYSICAL_CONFLICT) {
        StoredObject existing;
        ClaimFileResult claim_result;

        if (storage_client_remove(storage, uploaded.storage_key) != 0) {
            return FIRST_UPLOAD_CLEANUP_FAILED;
        }
        claim_result = repository->claim_existing(
            repository->context, request->user_name, request->md5,
            request->file_name, &existing);
        if (claim_result == CLAIM_FILE_LINKED ||
            claim_result == CLAIM_FILE_ALREADY_OWNED) {
            if (stored_object) *stored_object = existing;
            return FIRST_UPLOAD_OK;
        }
        return FIRST_UPLOAD_DATABASE_FAILED;
    }
    if (record_result != RECORD_NEW_FILE_COMMIT_UNKNOWN) {
        return remove_uploaded_object(storage, &uploaded, FIRST_UPLOAD_DATABASE_FAILED);
    }

    confirmation = repository->confirm(repository->context, request->user_name,
                                       request->md5, uploaded.storage_key);
    if (confirmation == 1) {
        if (stored_object) *stored_object = uploaded;
        return FIRST_UPLOAD_OK;
    }
    if (confirmation == 0) {
        return remove_uploaded_object(storage, &uploaded, FIRST_UPLOAD_DATABASE_FAILED);
    }

    /* The object may already be referenced by committed rows, so deleting it is unsafe. */
    return FIRST_UPLOAD_COMMIT_UNRESOLVED;
}
