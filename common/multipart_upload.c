#include "multipart_upload.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define INPUT_BUFFER_SIZE 4096
#define HEADER_LINE_CAPACITY 1024
#define HEADER_TOTAL_LIMIT 8192
#define BOUNDARY_CAPACITY 71

typedef struct {
    MultipartReadFunction read_function;
    void *read_context;
    uint64_t remaining;
    unsigned char buffer[INPUT_BUFFER_SIZE];
    size_t position;
    size_t length;
} BufferedInput;

static int ascii_case_equal(const char *left, const char *right, size_t length)
{
    size_t index;

    if (!left || !right) return 0;
    for (index = 0; index < length; ++index) {
        if (left[index] == '\0' || right[index] == '\0') return 0;
        if (tolower((unsigned char)left[index]) !=
            tolower((unsigned char)right[index])) return 0;
    }
    return 1;
}

static const char *ascii_case_find(const char *text, const char *needle)
{
    size_t needle_length;

    if (!text || !needle) return NULL;
    needle_length = strlen(needle);
    for (; *text; ++text) {
        if (strlen(text) < needle_length) return NULL;
        if (ascii_case_equal(text, needle, needle_length)) return text;
    }
    return NULL;
}

static int parse_boundary(const char *content_type, char boundary[BOUNDARY_CAPACITY])
{
    const char *parameter;
    const char *value;
    const char *end;
    size_t length;
    size_t index;

    if (!content_type ||
        !ascii_case_equal(content_type, "multipart/form-data", 19)) return -1;
    if (content_type[19] != '\0' && content_type[19] != ';' &&
        !isspace((unsigned char)content_type[19])) return -1;
    parameter = ascii_case_find(content_type + 19, "boundary=");
    if (!parameter) return -1;
    value = parameter + strlen("boundary=");
    if (*value == '"') {
        ++value;
        end = strchr(value, '"');
        if (!end) return -1;
    } else {
        end = value;
        while (*end && *end != ';' && !isspace((unsigned char)*end)) ++end;
    }
    length = (size_t)(end - value);
    if (length == 0 || length >= BOUNDARY_CAPACITY) return -1;
    for (index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (character < 33 || character > 126 || character == '"') return -1;
    }
    memcpy(boundary, value, length);
    boundary[length] = '\0';
    return 0;
}

static int fill_input(BufferedInput *input)
{
    size_t wanted;
    ssize_t count;

    if (input->position < input->length) return 1;
    if (input->remaining == 0) return 0;
    wanted = input->remaining < sizeof(input->buffer)
                 ? (size_t)input->remaining : sizeof(input->buffer);
    count = input->read_function(input->read_context, input->buffer, wanted);
    if (count <= 0 || (size_t)count > wanted) return -1;
    input->remaining -= (uint64_t)count;
    input->position = 0;
    input->length = (size_t)count;
    return 1;
}

static ssize_t buffered_read(BufferedInput *input, unsigned char *output, size_t capacity)
{
    size_t copied = 0;

    while (copied < capacity) {
        size_t available;
        size_t take;
        int fill_result = fill_input(input);

        if (fill_result < 0) return -1;
        if (fill_result == 0) break;
        available = input->length - input->position;
        take = capacity - copied < available ? capacity - copied : available;
        memcpy(output + copied, input->buffer + input->position, take);
        input->position += take;
        copied += take;
    }
    return (ssize_t)copied;
}

static int read_line(BufferedInput *input, char *line, size_t capacity, size_t *total)
{
    size_t length = 0;
    int saw_carriage_return = 0;

    if (!line || capacity < 2) return -1;
    for (;;) {
        unsigned char character;
        ssize_t count = buffered_read(input, &character, 1);

        if (count != 1) return -1;
        ++*total;
        if (*total > HEADER_TOTAL_LIMIT) return -1;
        if (saw_carriage_return) {
            if (character != '\n') return -1;
            line[length] = '\0';
            return 0;
        }
        if (character == '\r') {
            saw_carriage_return = 1;
            continue;
        }
        if (character == '\n' || length + 1 >= capacity) return -1;
        line[length++] = (char)character;
    }
}

static int copy_quoted_attribute(const char *line, const char *attribute,
                                 char *output, size_t output_size)
{
    const char *begin = ascii_case_find(line, attribute);
    const char *end;
    size_t length;

    if (!begin) return -1;
    begin += strlen(attribute);
    end = strchr(begin, '"');
    if (!end) return -1;
    length = (size_t)(end - begin);
    if (length == 0 || length >= output_size) return -1;
    memcpy(output, begin, length);
    output[length] = '\0';
    return 0;
}

static int parse_part_headers(BufferedInput *input, const char *boundary,
                              MultipartFileInfo *file_info)
{
    char line[HEADER_LINE_CAPACITY];
    char expected_first_line[BOUNDARY_CAPACITY + 3];
    size_t total = 0;
    int saw_disposition = 0;
    int saw_blank_line = 0;
    int length;

    length = snprintf(expected_first_line, sizeof(expected_first_line), "--%s", boundary);
    if (length < 0 || (size_t)length >= sizeof(expected_first_line) ||
        read_line(input, line, sizeof(line), &total) != 0 ||
        strcmp(line, expected_first_line) != 0) return -1;

    while (read_line(input, line, sizeof(line), &total) == 0) {
        if (line[0] == '\0') {
            saw_blank_line = 1;
            break;
        }
        if (ascii_case_equal(line, "Content-Disposition:", 20)) {
            char part_name[32];

            if (!ascii_case_find(line, "form-data") ||
                copy_quoted_attribute(line, "name=\"", part_name,
                                      sizeof(part_name)) != 0 ||
                strcmp(part_name, "file") != 0 ||
                copy_quoted_attribute(line, "filename=\"", file_info->file_name,
                                      sizeof(file_info->file_name)) != 0) return -1;
            saw_disposition = 1;
        } else if (ascii_case_equal(line, "Content-Type:", 13)) {
            const char *value = line + 13;
            size_t value_length;

            while (*value && isspace((unsigned char)*value)) ++value;
            value_length = strlen(value);
            if (value_length == 0 || value_length >= sizeof(file_info->media_type)) return -1;
            memcpy(file_info->media_type, value, value_length + 1);
        }
    }
    if (!saw_disposition || !saw_blank_line) return -1;
    if (file_info->media_type[0] == '\0') {
        memcpy(file_info->media_type, "application/octet-stream",
               sizeof("application/octet-stream"));
    }
    return 0;
}

static unsigned char *find_bytes(unsigned char *haystack, size_t haystack_length,
                                 const unsigned char *needle, size_t needle_length)
{
    size_t index;

    if (needle_length == 0 || haystack_length < needle_length) return NULL;
    for (index = 0; index + needle_length <= haystack_length; ++index) {
        if (memcmp(haystack + index, needle, needle_length) == 0) return haystack + index;
    }
    return NULL;
}

static int only_optional_crlf_remains(BufferedInput *input,
                                      const unsigned char *buffer, size_t length)
{
    unsigned char suffix[3];
    size_t used = 0;
    ssize_t count;

    if (length > 2) return 0;
    if (length > 0) {
        memcpy(suffix, buffer, length);
        used = length;
    }
    while (used < sizeof(suffix)) {
        count = buffered_read(input, suffix + used, sizeof(suffix) - used);
        if (count < 0) return 0;
        if (count == 0) break;
        used += (size_t)count;
    }
    return used == 0 || (used == 2 && suffix[0] == '\r' && suffix[1] == '\n');
}

static MultipartUploadResult stream_file_body(BufferedInput *input, const char *boundary,
                                               UploadIntake *intake)
{
    unsigned char pending[INPUT_BUFFER_SIZE + BOUNDARY_CAPACITY + 8];
    unsigned char incoming[INPUT_BUFFER_SIZE];
    unsigned char delimiter[BOUNDARY_CAPACITY + 8];
    size_t pending_length = 0;
    size_t delimiter_length;
    int delimiter_result;

    delimiter_result = snprintf((char *)delimiter, sizeof(delimiter), "\r\n--%s--", boundary);
    if (delimiter_result < 0 || (size_t)delimiter_result >= sizeof(delimiter)) {
        return MULTIPART_UPLOAD_MALFORMED;
    }
    delimiter_length = (size_t)delimiter_result;

    for (;;) {
        unsigned char *found;
        ssize_t count = buffered_read(input, incoming, sizeof(incoming));

        if (count < 0) return MULTIPART_UPLOAD_IO_ERROR;
        if (count == 0) return MULTIPART_UPLOAD_MALFORMED;
        memcpy(pending + pending_length, incoming, (size_t)count);
        pending_length += (size_t)count;
        found = find_bytes(pending, pending_length, delimiter, delimiter_length);
        if (found) {
            size_t file_length = (size_t)(found - pending);
            size_t suffix_offset = file_length + delimiter_length;

            if (file_length > 0 &&
                upload_intake_write(intake, pending, file_length) != UPLOAD_INTAKE_OK) {
                return MULTIPART_UPLOAD_FILE_ERROR;
            }
            if (!only_optional_crlf_remains(input, pending + suffix_offset,
                                            pending_length - suffix_offset)) {
                return MULTIPART_UPLOAD_MALFORMED;
            }
            return MULTIPART_UPLOAD_OK;
        }
        if (pending_length >= delimiter_length) {
            size_t flush_length = pending_length - (delimiter_length - 1);

            if (upload_intake_write(intake, pending, flush_length) != UPLOAD_INTAKE_OK) {
                return MULTIPART_UPLOAD_FILE_ERROR;
            }
            memmove(pending, pending + flush_length, pending_length - flush_length);
            pending_length -= flush_length;
        }
    }
}

MultipartUploadResult multipart_receive_single_file(
    MultipartReadFunction read_function, void *read_context, uint64_t content_length,
    const char *content_type, UploadIntake *intake, MultipartFileInfo *file_info)
{
    BufferedInput input;
    char boundary[BOUNDARY_CAPACITY];
    MultipartUploadResult result;

    if (!read_function || content_length == 0 || !content_type || !intake ||
        !intake->active || !file_info) return MULTIPART_UPLOAD_INVALID_ARGUMENT;
    memset(file_info, 0, sizeof(*file_info));
    if (parse_boundary(content_type, boundary) != 0) {
        upload_intake_abort(intake);
        return MULTIPART_UPLOAD_INVALID_CONTENT_TYPE;
    }
    memset(&input, 0, sizeof(input));
    input.read_function = read_function;
    input.read_context = read_context;
    input.remaining = content_length;
    if (parse_part_headers(&input, boundary, file_info) != 0) {
        upload_intake_abort(intake);
        return MULTIPART_UPLOAD_MALFORMED;
    }
    result = stream_file_body(&input, boundary, intake);
    if (result != MULTIPART_UPLOAD_OK) upload_intake_abort(intake);
    return result;
}
