#include "fcgi_stdio.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chunk_assembler.h"
#include "chunk_upload_repository.h"
#include "chunk_storage.h"
#include "cleanup_repository.h"
#include "fastdfs_storage_client.h"
#include "file_repository.h"
#include "http_response.h"
#include "json_util.h"
#include "runtime_config.h"
#include "token_service.h"
#include "upload_id.h"
#include "upload_intake.h"
#include "upload_service.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096
#define MIN_CHUNK_SIZE (1024U * 1024U)
#define MAX_CHUNK_SIZE (20U * 1024U * 1024U)
#define MAX_TOTAL_SIZE (50ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_TOTAL_CHUNKS 10000U
#define SESSION_TTL_SECONDS 86400U
#define ID_GENERATION_ATTEMPTS 3
#define CHUNK_READ_BUFFER_SIZE (64U * 1024U)

enum {
    CHUNK_INIT_OK = 0,
    CHUNK_INIT_INVALID_REQUEST = 1,
    CHUNK_INIT_TOKEN_ERROR = 2,
    CHUNK_INIT_DATABASE_ERROR = 3
};

enum {
    CHUNK_UPLOAD_OK = 0,
    CHUNK_UPLOAD_INVALID_REQUEST = 1,
    CHUNK_UPLOAD_TOKEN_ERROR = 2,
    CHUNK_UPLOAD_DATABASE_ERROR = 3,
    CHUNK_UPLOAD_SESSION_UNAVAILABLE = 4,
    CHUNK_UPLOAD_RECEIVE_ERROR = 5,
    CHUNK_UPLOAD_CONFLICT = 6,
    CHUNK_UPLOAD_STORAGE_ERROR = 7,
    CHUNK_UPLOAD_INCOMPLETE = 8,
    CHUNK_UPLOAD_COMMIT_UNRESOLVED = 9
};

static int read_body(char body[MAX_BODY_SIZE])
{
    const char *length_text = getenv("CONTENT_LENGTH");
    char *end;
    unsigned long length;

    if (!length_text || length_text[0] == '\0') return -1;
    length = strtoul(length_text, &end, 10);
    if (*end != '\0' || length == 0 || length >= MAX_BODY_SIZE) return -1;
    if (fread(body, 1, length, stdin) != length) return -1;
    body[length] = '\0';
    return 0;
}

static void handle_init_request(void)
{
    const char *method = getenv("REQUEST_METHOD");
    char body[MAX_BODY_SIZE];
    char user[33];
    char token[65];
    char file_name[129];
    char file_md5[33];
    char upload_id[UPLOAD_ID_HEX_LENGTH + 1];
    uint64_t total_size;
    uint64_t requested_chunk_size;
    uint64_t chunks;
    NewChunkUploadSession session;
    CreateChunkSessionResult result = CREATE_CHUNK_SESSION_DATABASE_ERROR;
    int attempt;

    if (!method || strcmp(method, "POST") != 0 || read_body(body) != 0 ||
        json_get_string(body, "user", user, sizeof(user)) != 0 ||
        json_get_string(body, "token", token, sizeof(token)) != 0 ||
        json_get_string(body, "file_name", file_name, sizeof(file_name)) != 0 ||
        json_get_string(body, "md5", file_md5, sizeof(file_md5)) != 0 ||
        json_get_uint64(body, "total_size", &total_size) != 0 ||
        json_get_uint64(body, "chunk_size", &requested_chunk_size) != 0 ||
        !validate_username(user) || !validate_file_name(file_name) ||
        !validate_password_md5(file_md5) || total_size == 0 ||
        total_size > MAX_TOTAL_SIZE || requested_chunk_size < MIN_CHUNK_SIZE ||
        requested_chunk_size > MAX_CHUNK_SIZE) {
        write_json_response(CHUNK_INIT_INVALID_REQUEST,
                            "invalid chunk upload init request", NULL);
        return;
    }
    chunks = ((total_size - 1U) / requested_chunk_size) + 1U;
    if (chunks == 0 || chunks > MAX_TOTAL_CHUNKS) {
        write_json_response(CHUNK_INIT_INVALID_REQUEST,
                            "invalid chunk upload plan", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(CHUNK_INIT_TOKEN_ERROR, "token error", NULL);
        return;
    }

    memset(&session, 0, sizeof(session));
    session.upload_id = upload_id;
    session.user_name = user;
    session.file_name = file_name;
    session.file_md5 = file_md5;
    session.total_size = total_size;
    session.chunk_size = (unsigned int)requested_chunk_size;
    session.total_chunks = (unsigned int)chunks;
    session.expires_in_seconds = SESSION_TTL_SECONDS;
    for (attempt = 0; attempt < ID_GENERATION_ATTEMPTS; ++attempt) {
        if (generate_upload_id(upload_id) != 0) break;
        result = create_chunk_upload_session(&session);
        if (result != CREATE_CHUNK_SESSION_DUPLICATE) break;
    }
    if (result != CREATE_CHUNK_SESSION_OK) {
        write_json_response(CHUNK_INIT_DATABASE_ERROR,
                            "unable to create chunk upload session", NULL);
        return;
    }

    write_json_header();
    printf("{\"code\":0,\"msg\":\"chunk upload initialized\","
           "\"upload_id\":\"%s\",\"chunk_size\":%u,"
           "\"total_chunks\":%u,\"expires_in\":%u,\"uploaded_chunks\":[]}\n",
           upload_id, session.chunk_size, session.total_chunks,
           session.expires_in_seconds);
}

static int parse_uint64_text(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (!text || text[0] == '\0' || !value) return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_chunk_path(const char *path,
                            char upload_id[UPLOAD_ID_HEX_LENGTH + 1],
                            unsigned int *chunk_index)
{
    static const char prefix[] = "/api/uploads/";
    static const char separator[] = "/chunks/";
    const char *cursor;
    char *end;
    unsigned long parsed_index;
    size_t index;

    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0 || !chunk_index) return -1;
    cursor = path + sizeof(prefix) - 1;
    for (index = 0; index < UPLOAD_ID_HEX_LENGTH; ++index) {
        if (!isxdigit((unsigned char)cursor[index]) ||
            (cursor[index] >= 'A' && cursor[index] <= 'F')) return -1;
        upload_id[index] = cursor[index];
    }
    upload_id[UPLOAD_ID_HEX_LENGTH] = '\0';
    cursor += UPLOAD_ID_HEX_LENGTH;
    if (strncmp(cursor, separator, sizeof(separator) - 1) != 0) return -1;
    cursor += sizeof(separator) - 1;
    if (!isdigit((unsigned char)*cursor)) return -1;
    errno = 0;
    parsed_index = strtoul(cursor, &end, 10);
    if (errno != 0 || *end != '\0' || parsed_index > UINT_MAX) return -1;
    *chunk_index = (unsigned int)parsed_index;
    return 0;
}

static int parse_status_path(const char *path,
                             char upload_id[UPLOAD_ID_HEX_LENGTH + 1])
{
    static const char prefix[] = "/api/uploads/";
    const char *value;

    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0) return -1;
    value = path + sizeof(prefix) - 1;
    if (!validate_upload_id(value)) return -1;
    memcpy(upload_id, value, UPLOAD_ID_HEX_LENGTH + 1);
    return 0;
}

static int parse_complete_path(const char *path,
                               char upload_id[UPLOAD_ID_HEX_LENGTH + 1])
{
    static const char prefix[] = "/api/uploads/";
    static const char suffix[] = "/complete";
    const char *value;

    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0) return -1;
    value = path + sizeof(prefix) - 1;
    if (strlen(value) != UPLOAD_ID_HEX_LENGTH + sizeof(suffix) - 1 ||
        strcmp(value + UPLOAD_ID_HEX_LENGTH, suffix) != 0) return -1;
    memcpy(upload_id, value, UPLOAD_ID_HEX_LENGTH);
    upload_id[UPLOAD_ID_HEX_LENGTH] = '\0';
    return validate_upload_id(upload_id) ? 0 : -1;
}

static void write_json_string(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");

    putchar('"');
    while (*cursor) {
        if (*cursor == '"' || *cursor == '\\') putchar('\\');
        if (*cursor == '\n') fputs("\\n", stdout);
        else if (*cursor == '\r') fputs("\\r", stdout);
        else if (*cursor == '\t') fputs("\\t", stdout);
        else if (*cursor >= 0x20) putchar(*cursor);
        ++cursor;
    }
    putchar('"');
}

static void write_status_success(const char *upload_id,
                                 const ChunkUploadStatus *status)
{
    size_t index;

    write_json_header();
    fputs("{\"code\":0,\"msg\":\"chunk upload status\",\"upload_id\":", stdout);
    write_json_string(upload_id);
    fputs(",\"file_name\":", stdout);
    write_json_string(status->file_name);
    fputs(",\"md5\":", stdout);
    write_json_string(status->file_md5);
    printf(",\"total_size\":%llu,\"chunk_size\":%u,\"total_chunks\":%u,",
           (unsigned long long)status->total_size,
           status->chunk_size, status->total_chunks);
    fputs("\"status\":", stdout);
    write_json_string(status->status);
    printf(",\"expires_in\":%llu,\"uploaded_count\":%llu,"
           "\"all_chunks_uploaded\":%s,"
           "\"uploaded_chunks\":[",
           (unsigned long long)status->expires_in_seconds,
           (unsigned long long)status->ready_count,
           status->ready_count == status->total_chunks ? "true" : "false");
    for (index = 0; index < status->ready_count; ++index) {
        if (index > 0) putchar(',');
        printf("%u", status->ready_chunks[index]);
    }
    fputs("]}\n", stdout);
}

static void handle_status_request(const char *path)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *user = getenv("HTTP_X_UPLOAD_USER");
    const char *token = getenv("HTTP_X_UPLOAD_TOKEN");
    char upload_id[UPLOAD_ID_HEX_LENGTH + 1];
    ChunkUploadStatus status;
    GetChunkStatusResult result;

    if (!method || strcmp(method, "GET") != 0 ||
        parse_status_path(path, upload_id) != 0 || !validate_username(user)) {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST,
                            "invalid chunk status request", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(CHUNK_UPLOAD_TOKEN_ERROR, "token error", NULL);
        return;
    }
    result = get_chunk_upload_status(upload_id, user, &status);
    if (result == GET_CHUNK_STATUS_UNAVAILABLE) {
        write_json_response(CHUNK_UPLOAD_SESSION_UNAVAILABLE,
                            "chunk upload session unavailable", NULL);
        return;
    }
    if (result != GET_CHUNK_STATUS_OK) {
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR, "database error", NULL);
        return;
    }
    write_status_success(upload_id, &status);
}

static int receive_chunk_body(const char *directory, uint64_t expected_size,
                              const char *expected_md5, ReceivedUpload *received)
{
    UploadIntake intake;
    unsigned char buffer[CHUNK_READ_BUFFER_SIZE];
    uint64_t remaining = expected_size;

    if (upload_intake_open(&intake, directory, expected_size) != UPLOAD_INTAKE_OK) return -1;
    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        size_t count = fread(buffer, 1, wanted, stdin);

        if (count == 0 || upload_intake_write(&intake, buffer, count) != UPLOAD_INTAKE_OK) {
            upload_intake_abort(&intake);
            return -1;
        }
        remaining -= (uint64_t)count;
    }
    if (upload_intake_finish(&intake, expected_size, expected_md5, received) !=
        UPLOAD_INTAKE_OK) {
        upload_intake_abort(&intake);
        return -1;
    }
    return 0;
}

static void write_chunk_success(unsigned int chunk_index, int idempotent)
{
    write_json_header();
    printf("{\"code\":0,\"msg\":\"chunk uploaded\",\"chunk_index\":%u,"
           "\"idempotent\":%s}\n", chunk_index, idempotent ? "true" : "false");
}

static void handle_chunk_request(const char *path)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *user = getenv("HTTP_X_UPLOAD_USER");
    const char *token = getenv("HTTP_X_UPLOAD_TOKEN");
    const char *chunk_md5 = getenv("HTTP_X_CHUNK_MD5");
    const char *content_length_text = getenv("CONTENT_LENGTH");
    const char *chunk_root = runtime_config_get("CHUNK_UPLOAD_DIR", "/data/chunk-uploads");
    char upload_id[UPLOAD_ID_HEX_LENGTH + 1];
    char final_path[CHUNK_STORED_PATH_CAPACITY];
    unsigned int chunk_index;
    uint64_t content_length;
    uint64_t expected_size;
    ChunkUploadSessionPlan plan;
    FindChunkSessionResult find_result;
    ReceivedUpload received;
    ChunkUploadPart part;
    ReserveChunkPartResult reserve_result;
    int install_result;

    if (!method || strcmp(method, "PUT") != 0 ||
        parse_chunk_path(path, upload_id, &chunk_index) != 0 ||
        !validate_username(user) || !validate_password_md5(chunk_md5) ||
        parse_uint64_text(content_length_text, &content_length) != 0 ||
        content_length == 0 || content_length > MAX_CHUNK_SIZE) {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST, "invalid chunk request", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(CHUNK_UPLOAD_TOKEN_ERROR, "token error", NULL);
        return;
    }
    find_result = find_receiving_chunk_session(upload_id, user, &plan);
    if (find_result == FIND_CHUNK_SESSION_UNAVAILABLE) {
        write_json_response(CHUNK_UPLOAD_SESSION_UNAVAILABLE,
                            "chunk upload session unavailable", NULL);
        return;
    }
    if (find_result != FIND_CHUNK_SESSION_OK) {
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR, "database error", NULL);
        return;
    }
    if (chunk_index >= plan.total_chunks) {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST, "chunk index out of range", NULL);
        return;
    }
    expected_size = chunk_index + 1U == plan.total_chunks
        ? plan.total_size - (uint64_t)plan.chunk_size * (plan.total_chunks - 1U)
        : plan.chunk_size;
    if (content_length != expected_size || expected_size == 0) {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST, "chunk size mismatch", NULL);
        return;
    }
    if (chunk_storage_path(chunk_root, upload_id, chunk_index,
                           final_path, sizeof(final_path)) != 0) {
        write_json_response(CHUNK_UPLOAD_STORAGE_ERROR, "chunk storage unavailable", NULL);
        return;
    }
    if (receive_chunk_body(chunk_root, expected_size, chunk_md5, &received) != 0) {
        write_json_response(CHUNK_UPLOAD_RECEIVE_ERROR,
                            "chunk size or MD5 verification failed", NULL);
        return;
    }

    memset(&part, 0, sizeof(part));
    part.upload_id = upload_id;
    part.chunk_index = chunk_index;
    part.size = received.size;
    part.chunk_md5 = received.md5;
    part.stored_path = final_path;
    reserve_result = reserve_chunk_upload_part(&part);
    if (reserve_result == RESERVE_CHUNK_PART_CONFLICT) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_CONFLICT,
                            "chunk index already contains different content", NULL);
        return;
    }
    if (reserve_result == RESERVE_CHUNK_PART_DATABASE_ERROR) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR, "database error", NULL);
        return;
    }
    install_result = chunk_storage_install(received.path, final_path);
    if (install_result < 0) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_STORAGE_ERROR, "unable to store chunk", NULL);
        return;
    }
    if (reserve_result != RESERVE_CHUNK_PART_READY &&
        mark_chunk_upload_part_ready(&part) != 0) {
        received_upload_discard(&received);
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR,
                            "unable to confirm stored chunk", NULL);
        return;
    }
    received_upload_discard(&received);
    write_chunk_success(chunk_index,
                        reserve_result != RESERVE_CHUNK_PART_NEW || install_result == 1);
}

static RecordNewFileResult repository_record(void *unused, const NewFileRecord *record)
{
    (void)unused;
    return record_new_file_upload(record);
}

static int repository_confirm(void *unused, const char *user_name, const char *md5,
                              const char *storage_key)
{
    (void)unused;
    return confirm_new_file_upload(user_name, md5, storage_key);
}

static ClaimFileResult repository_claim_existing(void *unused, const char *user_name,
                                                 const char *md5, const char *file_name,
                                                 StoredObject *stored_object)
{
    FileLocation location;
    ClaimFileResult result;

    (void)unused;
    result = claim_existing_file_with_location(user_name, md5, file_name, &location);
    if ((result == CLAIM_FILE_LINKED || result == CLAIM_FILE_ALREADY_OWNED) &&
        stored_object) {
        memcpy(stored_object->storage_key, location.storage_key,
               strlen(location.storage_key) + 1);
        memcpy(stored_object->url, location.url, strlen(location.url) + 1);
    }
    return result;
}

static int repository_schedule_cleanup(void *unused, const char *storage_key,
                                       const char *reason, const char *last_error)
{
    (void)unused;
    return enqueue_storage_cleanup(storage_key, reason, last_error);
}

static void file_type_from_name(const char *file_name, char output[33])
{
    const char *dot = strrchr(file_name, '.');
    size_t index, length;

    output[0] = '\0';
    if (!dot || dot == file_name || dot[1] == '\0') return;
    length = strlen(++dot);
    if (length > 32) return;
    for (index = 0; index < length; ++index) {
        if (!isalnum((unsigned char)dot[index])) {
            output[0] = '\0';
            return;
        }
        output[index] = (char)tolower((unsigned char)dot[index]);
    }
    output[length] = '\0';
}

static void cleanup_completed_parts(const ChunkUploadCompletion *completion)
{
    char directory[CHUNK_STORED_PATH_CAPACITY];
    char *slash;
    unsigned int index;

    if (!completion || !completion->parts || completion->total_chunks == 0) return;
    for (index = 0; index < completion->total_chunks; ++index) {
        if (unlink(completion->parts[index].stored_path) != 0 && errno != ENOENT) {
            fprintf(stderr, "unable to remove completed chunk %s\n",
                    completion->parts[index].stored_path);
        }
    }
    memcpy(directory, completion->parts[0].stored_path,
           strlen(completion->parts[0].stored_path) + 1);
    slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (rmdir(directory) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
            fprintf(stderr, "unable to remove chunk directory %s\n", directory);
        }
    }
}

static void write_completion_error(FirstUploadResult result)
{
    if (result == FIRST_UPLOAD_STORAGE_FAILED) {
        write_json_response(CHUNK_UPLOAD_STORAGE_ERROR, "storage upload failed", NULL);
    } else if (result == FIRST_UPLOAD_COMMIT_UNRESOLVED) {
        write_json_response(CHUNK_UPLOAD_COMMIT_UNRESOLVED,
                            "database commit could not be confirmed", NULL);
    } else if (result == FIRST_UPLOAD_CLEANUP_FAILED) {
        write_json_response(CHUNK_UPLOAD_STORAGE_ERROR,
                            "uploaded object cleanup failed", NULL);
    } else {
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR,
                            "database transaction failed", NULL);
    }
}

static void handle_complete_request(const char *path)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *user = getenv("HTTP_X_UPLOAD_USER");
    const char *token = getenv("HTTP_X_UPLOAD_TOKEN");
    const char *chunk_root = runtime_config_get("CHUNK_UPLOAD_DIR", "/data/chunk-uploads");
    const char *fastdfs_config = runtime_config_get("FASTDFS_CLIENT_CONFIG", NULL);
    const char *public_base_url = runtime_config_get("FASTDFS_PUBLIC_BASE_URL", NULL);
    char upload_id[UPLOAD_ID_HEX_LENGTH + 1];
    char file_type[33];
    ChunkUploadCompletion completion;
    ClaimChunkCompletionResult claim_result;
    ChunkAssemblyPart *parts = NULL;
    ReceivedUpload assembled;
    FastDfsStorageContext fastdfs_context;
    StorageClient storage;
    UploadRepository repository = {
        .context = NULL,
        .record = repository_record,
        .confirm = repository_confirm,
        .claim_existing = repository_claim_existing,
        .schedule_cleanup = repository_schedule_cleanup
    };
    FirstUploadRequest request;
    FirstUploadResult upload_result;
    unsigned int index;

    if (!method || strcmp(method, "POST") != 0 ||
        parse_complete_path(path, upload_id) != 0 || !validate_username(user) ||
        !fastdfs_config || fastdfs_config[0] == '\0' ||
        !public_base_url || public_base_url[0] == '\0') {
        write_json_response(CHUNK_UPLOAD_INVALID_REQUEST,
                            "invalid completion request", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(CHUNK_UPLOAD_TOKEN_ERROR, "token error", NULL);
        return;
    }
    claim_result = claim_chunk_upload_completion(upload_id, user, &completion);
    if (claim_result == CLAIM_CHUNK_COMPLETION_ALREADY_COMPLETED) {
        write_json_response(CHUNK_UPLOAD_OK, "upload already completed",
                            "\"download_api\":\"/api/download\"");
        return;
    }
    if (claim_result == CLAIM_CHUNK_COMPLETION_INCOMPLETE) {
        write_json_response(CHUNK_UPLOAD_INCOMPLETE, "chunks incomplete", NULL);
        return;
    }
    if (claim_result == CLAIM_CHUNK_COMPLETION_UNAVAILABLE) {
        write_json_response(CHUNK_UPLOAD_SESSION_UNAVAILABLE,
                            "chunk upload session unavailable", NULL);
        return;
    }
    if (claim_result != CLAIM_CHUNK_COMPLETION_OK) {
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR, "database error", NULL);
        return;
    }

    parts = calloc(completion.total_chunks, sizeof(*parts));
    if (!parts) goto preparation_failed;
    for (index = 0; index < completion.total_chunks; ++index) {
        parts[index].path = completion.parts[index].stored_path;
        parts[index].size = completion.parts[index].size;
    }
    if (assemble_chunk_upload(parts, completion.total_chunks, chunk_root,
                              completion.total_size, completion.file_md5,
                              &assembled) != CHUNK_ASSEMBLY_OK) goto preparation_failed;
    free(parts);
    parts = NULL;

    fastdfs_storage_context_init(&fastdfs_context, fastdfs_config, public_base_url);
    if (fastdfs_storage_client_init(&storage, &fastdfs_context) != 0) {
        received_upload_discard(&assembled);
        finish_chunk_upload_completion(upload_id, user, 0);
        free_chunk_upload_completion(&completion);
        write_json_response(CHUNK_UPLOAD_STORAGE_ERROR,
                            "storage configuration error", NULL);
        return;
    }
    file_type_from_name(completion.file_name, file_type);
    request.local_path = assembled.path;
    request.user_name = user;
    request.md5 = assembled.md5;
    request.file_name = completion.file_name;
    request.type = file_type;
    request.size = assembled.size;
    upload_result = execute_first_upload(&storage, &repository, &request, NULL);
    received_upload_discard(&assembled);
    if (upload_result != FIRST_UPLOAD_OK) {
        finish_chunk_upload_completion(upload_id, user, 0);
        free_chunk_upload_completion(&completion);
        write_completion_error(upload_result);
        return;
    }
    if (finish_chunk_upload_completion(upload_id, user, 1) != 0) {
        finish_chunk_upload_completion(upload_id, user, 0);
        free_chunk_upload_completion(&completion);
        write_json_response(CHUNK_UPLOAD_DATABASE_ERROR,
                            "unable to finalize upload session", NULL);
        return;
    }
    cleanup_completed_parts(&completion);
    free_chunk_upload_completion(&completion);
    write_json_response(CHUNK_UPLOAD_OK, "upload complete",
                        "\"download_api\":\"/api/download\"");
    return;

preparation_failed:
    free(parts);
    finish_chunk_upload_completion(upload_id, user, 0);
    free_chunk_upload_completion(&completion);
    write_json_response(CHUNK_UPLOAD_RECEIVE_ERROR,
                        "chunk assembly or file verification failed", NULL);
}

int main(void)
{
    runtime_config_init();
    while (FCGI_Accept() >= 0) {
        const char *path = getenv("SCRIPT_NAME");

        if (path && strcmp(path, "/api/uploads/init") == 0) {
            handle_init_request();
        } else if (path && strstr(path, "/complete") != NULL) {
            handle_complete_request(path);
        } else if (path && strstr(path, "/chunks/") != NULL) {
            handle_chunk_request(path);
        } else {
            handle_status_request(path);
        }
    }
    return 0;
}
