#ifndef AI_CLOUD_UPLOAD_INTAKE_H
#define AI_CLOUD_UPLOAD_INTAKE_H

#include <stddef.h>
#include <stdint.h>

#include "md5.h"

#define UPLOAD_TEMP_PATH_CAPACITY 512
#define UPLOAD_SIZE_UNKNOWN UINT64_MAX

typedef struct {
    int descriptor;
    int active;
    uint64_t size;
    uint64_t max_size;
    char path[UPLOAD_TEMP_PATH_CAPACITY];
    Md5Context md5;
} UploadIntake;

typedef struct {
    char path[UPLOAD_TEMP_PATH_CAPACITY];
    char md5[33];
    uint64_t size;
} ReceivedUpload;

typedef enum {
    UPLOAD_INTAKE_INVALID_ARGUMENT = -1,
    UPLOAD_INTAKE_IO_ERROR = -2,
    UPLOAD_INTAKE_SIZE_LIMIT = -3,
    UPLOAD_INTAKE_SIZE_MISMATCH = -4,
    UPLOAD_INTAKE_MD5_MISMATCH = -5,
    UPLOAD_INTAKE_OK = 0
} UploadIntakeResult;

UploadIntakeResult upload_intake_open(UploadIntake *intake, const char *temp_directory,
                                      uint64_t max_size);
UploadIntakeResult upload_intake_write(UploadIntake *intake,
                                       const unsigned char *data, size_t length);
UploadIntakeResult upload_intake_finish(UploadIntake *intake, uint64_t expected_size,
                                        const char *expected_md5, ReceivedUpload *upload);
void upload_intake_abort(UploadIntake *intake);
int received_upload_discard(ReceivedUpload *upload);

#endif
