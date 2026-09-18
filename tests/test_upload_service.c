#include <stdio.h>
#include <string.h>

#include "upload_service.h"

typedef struct {
    int upload_result;
    int remove_result;
    RecordNewFileResult record_result;
    int confirm_result;
    ClaimFileResult claim_result;
    int upload_calls;
    int remove_calls;
    int record_calls;
    int confirm_calls;
    int claim_calls;
    char removed_key[STORAGE_KEY_CAPACITY];
} FakeState;

static int fake_upload(void *context, const char *local_path, StoredObject *stored)
{
    FakeState *state = context;

    ++state->upload_calls;
    if (state->upload_result != 0 || strcmp(local_path, "/tmp/demo.txt") != 0) return -1;
    snprintf(stored->storage_key, sizeof(stored->storage_key), "group1/demo-key");
    snprintf(stored->url, sizeof(stored->url), "http://storage.local/group1/demo-key");
    return 0;
}

static int fake_remove(void *context, const char *storage_key)
{
    FakeState *state = context;

    ++state->remove_calls;
    snprintf(state->removed_key, sizeof(state->removed_key), "%s", storage_key);
    return state->remove_result;
}

static RecordNewFileResult fake_record(void *context, const NewFileRecord *record)
{
    FakeState *state = context;

    ++state->record_calls;
    if (strcmp(record->user_name, "alice") != 0 ||
        strcmp(record->md5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") != 0 ||
        strcmp(record->storage_key, "group1/demo-key") != 0 || record->size != 42) {
        return RECORD_NEW_FILE_INVALID_ARGUMENT;
    }
    return state->record_result;
}

static int fake_confirm(void *context, const char *user_name, const char *md5,
                        const char *storage_key)
{
    FakeState *state = context;

    ++state->confirm_calls;
    if (strcmp(user_name, "alice") != 0 ||
        strcmp(md5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") != 0 ||
        strcmp(storage_key, "group1/demo-key") != 0) return -1;
    return state->confirm_result;
}

static ClaimFileResult fake_claim_existing(void *context, const char *user_name,
                                           const char *md5, const char *file_name,
                                           StoredObject *stored)
{
    FakeState *state = context;

    ++state->claim_calls;
    if (strcmp(user_name, "alice") != 0 ||
        strcmp(md5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") != 0 ||
        strcmp(file_name, "demo.txt") != 0) {
        return CLAIM_FILE_DATABASE_FAILURE;
    }
    snprintf(stored->storage_key, sizeof(stored->storage_key), "group1/winner-key");
    snprintf(stored->url, sizeof(stored->url),
             "http://storage.local/group1/winner-key");
    return state->claim_result;
}

static FirstUploadResult run_upload(FakeState *state, StoredObject *stored)
{
    StorageClient storage = {state, fake_upload, fake_remove};
    UploadRepository repository = {
        state,
        fake_record,
        fake_confirm,
        fake_claim_existing
    };
    FirstUploadRequest request = {
        "/tmp/demo.txt",
        "alice",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "demo.txt",
        "txt",
        42
    };

    return execute_first_upload(&storage, &repository, &request, stored);
}

static void reset_state(FakeState *state)
{
    memset(state, 0, sizeof(*state));
    state->record_result = RECORD_NEW_FILE_CREATED;
    state->confirm_result = 1;
    state->claim_result = CLAIM_FILE_LINKED;
}

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

int main(void)
{
    FakeState state;
    StoredObject stored;

    reset_state(&state);
    EXPECT(run_upload(&state, &stored) == FIRST_UPLOAD_OK, "successful upload");
    EXPECT(state.upload_calls == 1 && state.record_calls == 1 && state.remove_calls == 0,
           "success call sequence");
    EXPECT(strcmp(stored.storage_key, "group1/demo-key") == 0, "success output");

    reset_state(&state);
    state.upload_result = -1;
    EXPECT(run_upload(&state, NULL) == FIRST_UPLOAD_STORAGE_FAILED, "storage failure");
    EXPECT(state.record_calls == 0 && state.remove_calls == 0, "storage failure stops workflow");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_DATABASE_FAILURE;
    EXPECT(run_upload(&state, NULL) == FIRST_UPLOAD_DATABASE_FAILED, "database rollback compensation");
    EXPECT(state.remove_calls == 1 && strcmp(state.removed_key, "group1/demo-key") == 0,
           "database failure removes uploaded object");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_PHYSICAL_CONFLICT;
    EXPECT(run_upload(&state, &stored) == FIRST_UPLOAD_OK, "physical conflict is reused");
    EXPECT(state.remove_calls == 1 && state.claim_calls == 1,
           "conflict removes duplicate then claims winner");
    EXPECT(strcmp(stored.storage_key, "group1/winner-key") == 0,
           "conflict returns winner object");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_PHYSICAL_CONFLICT;
    state.claim_result = CLAIM_FILE_ALREADY_OWNED;
    EXPECT(run_upload(&state, &stored) == FIRST_UPLOAD_OK,
           "same-user concurrent upload is idempotent");
    EXPECT(state.remove_calls == 1 && state.claim_calls == 1,
           "same-user conflict removes duplicate without incrementing twice");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_PHYSICAL_CONFLICT;
    state.claim_result = CLAIM_FILE_DATABASE_FAILURE;
    EXPECT(run_upload(&state, NULL) == FIRST_UPLOAD_DATABASE_FAILED,
           "claim failure is visible after conflict cleanup");
    EXPECT(state.remove_calls == 1 && state.claim_calls == 1,
           "claim failure occurs after duplicate cleanup");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_PHYSICAL_CONFLICT;
    state.remove_result = -1;
    EXPECT(run_upload(&state, NULL) == FIRST_UPLOAD_CLEANUP_FAILED,
           "conflict cleanup failure is visible");
    EXPECT(state.claim_calls == 0,
           "failed duplicate cleanup does not create a logical reference");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_DATABASE_FAILURE;
    state.remove_result = -1;
    EXPECT(run_upload(&state, NULL) == FIRST_UPLOAD_CLEANUP_FAILED, "cleanup failure is visible");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_COMMIT_UNKNOWN;
    state.confirm_result = 1;
    EXPECT(run_upload(&state, &stored) == FIRST_UPLOAD_OK, "unknown commit confirmed");
    EXPECT(state.confirm_calls == 1 && state.remove_calls == 0,
           "confirmed commit keeps uploaded object");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_COMMIT_UNKNOWN;
    state.confirm_result = 0;
    EXPECT(run_upload(&state, NULL) == FIRST_UPLOAD_DATABASE_FAILED, "unknown commit absent");
    EXPECT(state.confirm_calls == 1 && state.remove_calls == 1,
           "absent commit removes uploaded object");

    reset_state(&state);
    state.record_result = RECORD_NEW_FILE_COMMIT_UNKNOWN;
    state.confirm_result = -1;
    EXPECT(run_upload(&state, NULL) == FIRST_UPLOAD_COMMIT_UNRESOLVED,
           "unresolved commit remains pending");
    EXPECT(state.confirm_calls == 1 && state.remove_calls == 0,
           "unresolved commit does not risk deleting referenced object");

    puts("upload_service tests passed");
    return 0;
}
