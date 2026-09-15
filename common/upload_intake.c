#define _POSIX_C_SOURCE 200809L

#include "upload_intake.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int valid_expected_md5(const char *value)
{
    size_t index;

    if (!value) return 1;
    if (strlen(value) != 32) return 0;
    for (index = 0; index < 32; ++index) {
        if (!isxdigit((unsigned char)value[index])) return 0;
    }
    return 1;
}

static int md5_equals(const char *left, const char *right)
{
    size_t index;

    for (index = 0; index < 32; ++index) {
        if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) return 0;
    }
    return 1;
}

void upload_intake_abort(UploadIntake *intake)
{
    if (!intake) return;
    if (intake->descriptor >= 0) close(intake->descriptor);
    if (intake->path[0] != '\0') unlink(intake->path);
    intake->descriptor = -1;
    intake->active = 0;
    intake->path[0] = '\0';
    intake->size = 0;
}

UploadIntakeResult upload_intake_open(UploadIntake *intake, const char *temp_directory,
                                      uint64_t max_size)
{
    int length;

    if (!intake || !temp_directory || temp_directory[0] == '\0' || max_size == 0) {
        return UPLOAD_INTAKE_INVALID_ARGUMENT;
    }
    memset(intake, 0, sizeof(*intake));
    intake->descriptor = -1;
    length = snprintf(intake->path, sizeof(intake->path),
                      "%s%sai-cloud-upload-XXXXXX", temp_directory,
                      temp_directory[strlen(temp_directory) - 1] == '/' ? "" : "/");
    if (length < 0 || (size_t)length >= sizeof(intake->path)) {
        intake->path[0] = '\0';
        return UPLOAD_INTAKE_INVALID_ARGUMENT;
    }
    intake->descriptor = mkstemp(intake->path);
    if (intake->descriptor < 0) {
        intake->path[0] = '\0';
        return UPLOAD_INTAKE_IO_ERROR;
    }
    if (fcntl(intake->descriptor, F_SETFD, FD_CLOEXEC) != 0) {
        upload_intake_abort(intake);
        return UPLOAD_INTAKE_IO_ERROR;
    }
    intake->active = 1;
    intake->max_size = max_size;
    md5_init(&intake->md5);
    return UPLOAD_INTAKE_OK;
}

UploadIntakeResult upload_intake_write(UploadIntake *intake,
                                       const unsigned char *data, size_t length)
{
    size_t written = 0;

    if (!intake || !intake->active || intake->descriptor < 0 || (!data && length > 0)) {
        return UPLOAD_INTAKE_INVALID_ARGUMENT;
    }
    if ((uint64_t)length > intake->max_size - intake->size) {
        upload_intake_abort(intake);
        return UPLOAD_INTAKE_SIZE_LIMIT;
    }
    while (written < length) {
        ssize_t result = write(intake->descriptor, data + written, length - written);

        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            upload_intake_abort(intake);
            return UPLOAD_INTAKE_IO_ERROR;
        }
        written += (size_t)result;
    }
    md5_update(&intake->md5, data, length);
    intake->size += (uint64_t)length;
    return UPLOAD_INTAKE_OK;
}

static UploadIntakeResult reject_finished_upload(UploadIntake *intake,
                                                 UploadIntakeResult result)
{
    if (intake->path[0] != '\0') unlink(intake->path);
    intake->path[0] = '\0';
    intake->size = 0;
    return result;
}

UploadIntakeResult upload_intake_finish(UploadIntake *intake, uint64_t expected_size,
                                        const char *expected_md5, ReceivedUpload *upload)
{
    char digest[33];

    if (!intake || !intake->active || intake->descriptor < 0) {
        return UPLOAD_INTAKE_INVALID_ARGUMENT;
    }
    if (upload) memset(upload, 0, sizeof(*upload));
    if (!upload || !valid_expected_md5(expected_md5)) {
        upload_intake_abort(intake);
        return UPLOAD_INTAKE_INVALID_ARGUMENT;
    }
    if (close(intake->descriptor) != 0) {
        intake->descriptor = -1;
        intake->active = 0;
        return reject_finished_upload(intake, UPLOAD_INTAKE_IO_ERROR);
    }
    intake->descriptor = -1;
    intake->active = 0;
    md5_final(&intake->md5, digest);
    if (expected_size != UPLOAD_SIZE_UNKNOWN && intake->size != expected_size) {
        return reject_finished_upload(intake, UPLOAD_INTAKE_SIZE_MISMATCH);
    }
    if (expected_md5 && !md5_equals(digest, expected_md5)) {
        return reject_finished_upload(intake, UPLOAD_INTAKE_MD5_MISMATCH);
    }

    memcpy(upload->path, intake->path, strlen(intake->path) + 1);
    memcpy(upload->md5, digest, sizeof(upload->md5));
    upload->size = intake->size;
    intake->path[0] = '\0';
    intake->size = 0;
    return UPLOAD_INTAKE_OK;
}

int received_upload_discard(ReceivedUpload *upload)
{
    int result;

    if (!upload || upload->path[0] == '\0') return -1;
    result = unlink(upload->path);
    if (result != 0 && errno != ENOENT) return -1;
    memset(upload, 0, sizeof(*upload));
    return 0;
}
