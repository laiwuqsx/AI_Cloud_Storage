#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "md5.h"
#include "multipart_upload.h"

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
    size_t max_read;
} MemoryReader;

static ssize_t memory_read(void *context, unsigned char *buffer, size_t capacity)
{
    MemoryReader *reader = context;
    size_t remaining = reader->length - reader->position;
    size_t count = capacity < remaining ? capacity : remaining;

    if (count > reader->max_read) count = reader->max_read;
    if (count == 0) return 0;
    memcpy(buffer, reader->data + reader->position, count);
    reader->position += count;
    return (ssize_t)count;
}

static int file_equals(const char *path, const unsigned char *expected, size_t expected_length)
{
    unsigned char buffer[128];
    int descriptor = open(path, O_RDONLY);
    ssize_t count;

    if (descriptor < 0 || expected_length > sizeof(buffer)) return 0;
    count = read(descriptor, buffer, sizeof(buffer));
    close(descriptor);
    return count == (ssize_t)expected_length &&
           memcmp(buffer, expected, expected_length) == 0;
}

int main(void)
{
    static const char boundary[] = "AaB03x-test-boundary";
    static const unsigned char file_data[] = "hello\r\n--not-the-boundary\r\nworld";
    char body[1024];
    char content_type[128];
    char digest[33];
    int body_length;
    MemoryReader reader;
    UploadIntake intake;
    ReceivedUpload upload;
    MultipartFileInfo info;

    body_length = snprintf(
        body, sizeof(body),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"notes.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "%s"
        "\r\n--%s--\r\n",
        boundary, file_data, boundary);
    EXPECT(body_length > 0 && (size_t)body_length < sizeof(body), "build multipart body");
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=\"%s\"", boundary);
    reader.data = (const unsigned char *)body;
    reader.length = (size_t)body_length;
    reader.position = 0;
    reader.max_read = 3;

    EXPECT(upload_intake_open(&intake, "/tmp", 1024) == UPLOAD_INTAKE_OK,
           "open multipart intake");
    EXPECT(multipart_receive_single_file(memory_read, &reader, reader.length,
                                         content_type, &intake, &info) ==
               MULTIPART_UPLOAD_OK, "stream multipart with split boundaries");
    md5_hex(file_data, sizeof(file_data) - 1, digest);
    EXPECT(upload_intake_finish(&intake, sizeof(file_data) - 1, digest, &upload) ==
               UPLOAD_INTAKE_OK, "finish multipart upload");
    EXPECT(strcmp(info.file_name, "notes.txt") == 0, "parse file name");
    EXPECT(strcmp(info.media_type, "text/plain") == 0, "parse media type");
    EXPECT(file_equals(upload.path, file_data, sizeof(file_data) - 1),
           "write only file bytes");
    EXPECT(received_upload_discard(&upload) == 0, "discard multipart file");

    reader.position = 0;
    EXPECT(upload_intake_open(&intake, "/tmp", 8) == UPLOAD_INTAKE_OK,
           "open limited multipart intake");
    EXPECT(multipart_receive_single_file(memory_read, &reader, reader.length,
                                         content_type, &intake, &info) ==
               MULTIPART_UPLOAD_FILE_ERROR, "propagate upload size limit");
    EXPECT(intake.active == 0 && intake.path[0] == '\0', "clean rejected multipart file");

    reader.data = (const unsigned char *)"not multipart";
    reader.length = strlen((const char *)reader.data);
    reader.position = 0;
    EXPECT(upload_intake_open(&intake, "/tmp", 1024) == UPLOAD_INTAKE_OK,
           "open malformed multipart intake");
    EXPECT(multipart_receive_single_file(memory_read, &reader, reader.length,
                                         "text/plain", &intake, &info) ==
               MULTIPART_UPLOAD_INVALID_CONTENT_TYPE, "reject invalid content type");
    EXPECT(intake.active == 0 && intake.path[0] == '\0', "clean malformed upload");

    puts("multipart upload tests passed");
    return 0;
}
