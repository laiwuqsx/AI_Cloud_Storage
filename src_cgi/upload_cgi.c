#include "fcgi_stdio.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fastdfs_storage_client.h"
#include "file_repository.h"
#include "http_response.h"
#include "multipart_upload.h"
#include "runtime_config.h"
#include "token_service.h"
#include "upload_service.h"
#include "user_validation.h"

#define DEFAULT_MAX_UPLOAD_BYTES (10ULL * 1024ULL * 1024ULL)
#define MAX_MULTIPART_OVERHEAD (64ULL * 1024ULL)

enum {
    UPLOAD_RESPONSE_OK = 0,
    UPLOAD_RESPONSE_INVALID_REQUEST = 1,
    UPLOAD_RESPONSE_TOKEN_ERROR = 2,
    UPLOAD_RESPONSE_RECEIVE_ERROR = 3,
    UPLOAD_RESPONSE_STORAGE_ERROR = 4,
    UPLOAD_RESPONSE_DATABASE_ERROR = 5,
    UPLOAD_RESPONSE_CONFLICT = 6,
    UPLOAD_RESPONSE_CLEANUP_ERROR = 7,
    UPLOAD_RESPONSE_COMMIT_UNRESOLVED = 8
};

static int parse_uint64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;
    const char *cursor;

    if (!text || !value || text[0] == '\0') return -1;
    for (cursor = text; *cursor; ++cursor) {
        if (!isdigit((unsigned char)*cursor)) return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int valid_token(const char *token)
{
    size_t index;

    if (!token || strlen(token) != 64) return 0;
    for (index = 0; index < 64; ++index) {
        if (!isxdigit((unsigned char)token[index])) return 0;
    }
    return 1;
}

static uint64_t configured_max_upload_size(void)
{
    const char *text = runtime_config_get("UPLOAD_MAX_BYTES", NULL);
    uint64_t value;

    if (!text) return DEFAULT_MAX_UPLOAD_BYTES;
    if (parse_uint64(text, &value) != 0 || value == 0) return 0;
    return value;
}

static ssize_t read_fastcgi_body(void *unused, unsigned char *buffer, size_t capacity)
{
    size_t count;

    (void)unused;
    count = fread(buffer, 1, capacity, stdin);
    if (count == 0 && ferror(stdin)) return -1;
    return (ssize_t)count;
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

static void file_type_from_name(const char *file_name, char output[33])
{
    const char *dot = strrchr(file_name, '.');
    size_t index;
    size_t length;

    output[0] = '\0';
    if (!dot || dot == file_name || dot[1] == '\0') return;
    ++dot;
    length = strlen(dot);
    if (length > 32) return;
    for (index = 0; index < length; ++index) {
        if (!isalnum((unsigned char)dot[index])) return;
        output[index] = (char)tolower((unsigned char)dot[index]);
    }
    output[length] = '\0';
}

static void write_upload_success(const StoredObject *stored)
{
    write_json_header();
    printf("{\"code\":0,\"msg\":\"upload complete\",\"url\":\"%s\"}\n",
           stored->url);
}

static void write_workflow_error(FirstUploadResult result)
{
    switch (result) {
    case FIRST_UPLOAD_STORAGE_FAILED:
        write_json_response(UPLOAD_RESPONSE_STORAGE_ERROR, "storage upload failed", NULL);
        break;
    case FIRST_UPLOAD_DATABASE_FAILED:
        write_json_response(UPLOAD_RESPONSE_DATABASE_ERROR, "database transaction failed", NULL);
        break;
    case FIRST_UPLOAD_PHYSICAL_CONFLICT:
        write_json_response(UPLOAD_RESPONSE_CONFLICT, "file was uploaded concurrently", NULL);
        break;
    case FIRST_UPLOAD_CLEANUP_FAILED:
        write_json_response(UPLOAD_RESPONSE_CLEANUP_ERROR, "uploaded object cleanup failed", NULL);
        break;
    case FIRST_UPLOAD_COMMIT_UNRESOLVED:
        write_json_response(UPLOAD_RESPONSE_COMMIT_UNRESOLVED,
                            "database commit could not be confirmed", NULL);
        break;
    default:
        write_json_response(UPLOAD_RESPONSE_INVALID_REQUEST, "invalid upload workflow", NULL);
        break;
    }
}

static void handle_upload_request(void)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *content_type = getenv("CONTENT_TYPE");
    const char *content_length_text = getenv("CONTENT_LENGTH");
    const char *user = getenv("HTTP_X_UPLOAD_USER");
    const char *token = getenv("HTTP_X_UPLOAD_TOKEN");
    const char *expected_md5 = getenv("HTTP_X_UPLOAD_MD5");
    const char *expected_size_text = getenv("HTTP_X_UPLOAD_SIZE");
    const char *temp_directory = runtime_config_get("UPLOAD_TEMP_DIR", "/tmp");
    const char *fastdfs_config = runtime_config_get("FASTDFS_CLIENT_CONFIG", NULL);
    const char *public_base_url = runtime_config_get("FASTDFS_PUBLIC_BASE_URL", NULL);
    uint64_t content_length;
    uint64_t expected_size;
    uint64_t max_upload_size = configured_max_upload_size();
    UploadIntake intake;
    ReceivedUpload received;
    MultipartFileInfo file_info;
    FastDfsStorageContext fastdfs_context;
    StorageClient storage;
    UploadRepository repository = {NULL, repository_record, repository_confirm};
    FirstUploadRequest request;
    StoredObject stored;
    FirstUploadResult upload_result;
    char file_type[33];

    if (!method || strcmp(method, "POST") != 0 || !content_type ||
        parse_uint64(content_length_text, &content_length) != 0 || content_length == 0 ||
        parse_uint64(expected_size_text, &expected_size) != 0 || expected_size == 0 ||
        max_upload_size == 0 || expected_size > max_upload_size ||
        max_upload_size > UINT64_MAX - MAX_MULTIPART_OVERHEAD ||
        content_length > max_upload_size + MAX_MULTIPART_OVERHEAD ||
        !validate_username(user) || !validate_password_md5(expected_md5) ||
        !valid_token(token) || !fastdfs_config || fastdfs_config[0] == '\0' ||
        !public_base_url || public_base_url[0] == '\0') {
        write_json_response(UPLOAD_RESPONSE_INVALID_REQUEST, "invalid upload request", NULL);
        return;
    }
    if (verify_session_token(user, token) != 0) {
        write_json_response(UPLOAD_RESPONSE_TOKEN_ERROR, "token error", NULL);
        return;
    }
    if (upload_intake_open(&intake, temp_directory ? temp_directory : "/tmp",
                           max_upload_size) != UPLOAD_INTAKE_OK) {
        write_json_response(UPLOAD_RESPONSE_RECEIVE_ERROR,
                            "unable to create temporary upload", NULL);
        return;
    }
    if (multipart_receive_single_file(read_fastcgi_body, NULL, content_length,
                                      content_type, &intake, &file_info) !=
        MULTIPART_UPLOAD_OK) {
        write_json_response(UPLOAD_RESPONSE_RECEIVE_ERROR, "invalid multipart upload", NULL);
        return;
    }
    if (!validate_file_name(file_info.file_name) ||
        upload_intake_finish(&intake, expected_size, expected_md5, &received) !=
            UPLOAD_INTAKE_OK) {
        upload_intake_abort(&intake);
        write_json_response(UPLOAD_RESPONSE_RECEIVE_ERROR,
                            "file size or MD5 verification failed", NULL);
        return;
    }

    fastdfs_storage_context_init(&fastdfs_context, fastdfs_config, public_base_url);
    if (fastdfs_storage_client_init(&storage, &fastdfs_context) != 0) {
        received_upload_discard(&received);
        write_json_response(UPLOAD_RESPONSE_STORAGE_ERROR, "storage configuration error", NULL);
        return;
    }
    file_type_from_name(file_info.file_name, file_type);
    request.local_path = received.path;
    request.user_name = user;
    request.md5 = received.md5;
    request.file_name = file_info.file_name;
    request.type = file_type;
    request.size = received.size;
    upload_result = execute_first_upload(&storage, &repository, &request, &stored);
    if (received_upload_discard(&received) != 0) {
        fprintf(stderr, "upload temporary file cleanup failed\n");
    }
    if (upload_result == FIRST_UPLOAD_OK) {
        write_upload_success(&stored);
    } else {
        write_workflow_error(upload_result);
    }
}

int main(void)
{
    runtime_config_init();
    while (FCGI_Accept() >= 0) handle_upload_request();
    return 0;
}
