#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "upload_intake.h"

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

static int file_contains(const char *path, const char *expected)
{
    char buffer[64] = {0};
    int descriptor = open(path, O_RDONLY);
    ssize_t length;

    if (descriptor < 0) return 0;
    length = read(descriptor, buffer, sizeof(buffer) - 1);
    close(descriptor);
    return length == (ssize_t)strlen(expected) && strcmp(buffer, expected) == 0;
}

int main(void)
{
    UploadIntake intake;
    ReceivedUpload upload;
    char rejected_path[UPLOAD_TEMP_PATH_CAPACITY];

    EXPECT(upload_intake_open(&intake, "/tmp", 1024) == UPLOAD_INTAKE_OK,
           "open upload intake");
    EXPECT(upload_intake_write(&intake, (const unsigned char *)"hello ", 6) ==
               UPLOAD_INTAKE_OK, "write first chunk");
    EXPECT(upload_intake_write(&intake, (const unsigned char *)"world", 5) ==
               UPLOAD_INTAKE_OK, "write second chunk");
    EXPECT(upload_intake_finish(&intake, 11, "5EB63BBBE01EEED093CB22BB8F5ACDC3", &upload) ==
               UPLOAD_INTAKE_OK, "finish and verify upload");
    EXPECT(upload.size == 11, "actual byte count");
    EXPECT(strcmp(upload.md5, "5eb63bbbe01eeed093cb22bb8f5acdc3") == 0,
           "streamed MD5");
    EXPECT(file_contains(upload.path, "hello world"), "temporary file content");
    snprintf(rejected_path, sizeof(rejected_path), "%s", upload.path);
    EXPECT(received_upload_discard(&upload) == 0, "discard completed upload");
    EXPECT(access(rejected_path, F_OK) != 0, "completed file removed");

    EXPECT(upload_intake_open(&intake, "/tmp", 5) == UPLOAD_INTAKE_OK,
           "open size-limited intake");
    snprintf(rejected_path, sizeof(rejected_path), "%s", intake.path);
    EXPECT(upload_intake_write(&intake, (const unsigned char *)"123456", 6) ==
               UPLOAD_INTAKE_SIZE_LIMIT, "enforce size limit");
    EXPECT(access(rejected_path, F_OK) != 0, "oversized temporary file removed");

    EXPECT(upload_intake_open(&intake, "/tmp", 10) == UPLOAD_INTAKE_OK,
           "open size-check intake");
    snprintf(rejected_path, sizeof(rejected_path), "%s", intake.path);
    EXPECT(upload_intake_write(&intake, (const unsigned char *)"abc", 3) ==
               UPLOAD_INTAKE_OK, "write size-check content");
    EXPECT(upload_intake_finish(&intake, 4, NULL, &upload) ==
               UPLOAD_INTAKE_SIZE_MISMATCH, "declared size mismatch");
    EXPECT(access(rejected_path, F_OK) != 0, "size mismatch file removed");

    EXPECT(upload_intake_open(&intake, "/tmp", 10) == UPLOAD_INTAKE_OK,
           "open digest-check intake");
    snprintf(rejected_path, sizeof(rejected_path), "%s", intake.path);
    EXPECT(upload_intake_write(&intake, (const unsigned char *)"abc", 3) ==
               UPLOAD_INTAKE_OK, "write digest-check content");
    EXPECT(upload_intake_finish(&intake, UPLOAD_SIZE_UNKNOWN,
                                "00000000000000000000000000000000", &upload) ==
               UPLOAD_INTAKE_MD5_MISMATCH, "declared MD5 mismatch");
    EXPECT(access(rejected_path, F_OK) != 0, "MD5 mismatch file removed");

    EXPECT(upload_intake_open(&intake, "/tmp", 10) == UPLOAD_INTAKE_OK,
           "open invalid-digest intake");
    snprintf(rejected_path, sizeof(rejected_path), "%s", intake.path);
    EXPECT(upload_intake_write(&intake, (const unsigned char *)"abc", 3) ==
               UPLOAD_INTAKE_OK, "write invalid-digest content");
    EXPECT(upload_intake_finish(&intake, UPLOAD_SIZE_UNKNOWN, "bad-md5", &upload) ==
               UPLOAD_INTAKE_INVALID_ARGUMENT, "reject invalid declared MD5");
    EXPECT(access(rejected_path, F_OK) != 0, "invalid declared MD5 file removed");
    EXPECT(upload.path[0] == '\0' && upload.size == 0, "failed upload output cleared");

    puts("upload_intake tests passed");
    return 0;
}
