#ifndef AI_CLOUD_MULTIPART_UPLOAD_H
#define AI_CLOUD_MULTIPART_UPLOAD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "upload_intake.h"

#define MULTIPART_FILE_NAME_CAPACITY 129
#define MULTIPART_MEDIA_TYPE_CAPACITY 65

typedef ssize_t (*MultipartReadFunction)(void *context, unsigned char *buffer,
                                         size_t capacity);

typedef struct {
    char file_name[MULTIPART_FILE_NAME_CAPACITY];
    char media_type[MULTIPART_MEDIA_TYPE_CAPACITY];
} MultipartFileInfo;

typedef enum {
    MULTIPART_UPLOAD_INVALID_ARGUMENT = -1,
    MULTIPART_UPLOAD_INVALID_CONTENT_TYPE = -2,
    MULTIPART_UPLOAD_MALFORMED = -3,
    MULTIPART_UPLOAD_IO_ERROR = -4,
    MULTIPART_UPLOAD_FILE_ERROR = -5,
    MULTIPART_UPLOAD_OK = 0
} MultipartUploadResult;

/* Streams exactly one multipart file part into an already-open UploadIntake. */
MultipartUploadResult multipart_receive_single_file(
    MultipartReadFunction read_function, void *read_context, uint64_t content_length,
    const char *content_type, UploadIntake *intake, MultipartFileInfo *file_info);

#endif
