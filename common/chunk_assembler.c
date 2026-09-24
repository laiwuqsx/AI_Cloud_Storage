#include "chunk_assembler.h"

#include <errno.h>
#include <stdio.h>

#define ASSEMBLY_BUFFER_SIZE (64U * 1024U)

ChunkAssemblyResult assemble_chunk_upload(const ChunkAssemblyPart *parts,
                                          size_t part_count,
                                          const char *temp_directory,
                                          uint64_t expected_size,
                                          const char *expected_md5,
                                          ReceivedUpload *assembled)
{
    UploadIntake intake;
    unsigned char buffer[ASSEMBLY_BUFFER_SIZE];
    size_t index;

    if (!parts || part_count == 0 || !temp_directory || !expected_md5 ||
        !assembled || expected_size == 0) return CHUNK_ASSEMBLY_INVALID_ARGUMENT;
    if (upload_intake_open(&intake, temp_directory, expected_size) != UPLOAD_INTAKE_OK) {
        return CHUNK_ASSEMBLY_IO_ERROR;
    }

    for (index = 0; index < part_count; ++index) {
        FILE *input;
        uint64_t copied = 0;

        if (!parts[index].path || parts[index].path[0] == '\0' || parts[index].size == 0) {
            upload_intake_abort(&intake);
            return CHUNK_ASSEMBLY_INVALID_ARGUMENT;
        }
        input = fopen(parts[index].path, "rb");
        if (!input) {
            upload_intake_abort(&intake);
            return CHUNK_ASSEMBLY_IO_ERROR;
        }
        while (copied < parts[index].size) {
            uint64_t remaining = parts[index].size - copied;
            size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            size_t count = fread(buffer, 1, wanted, input);

            if (count == 0 ||
                upload_intake_write(&intake, buffer, count) != UPLOAD_INTAKE_OK) {
                fclose(input);
                upload_intake_abort(&intake);
                return CHUNK_ASSEMBLY_IO_ERROR;
            }
            copied += (uint64_t)count;
        }
        errno = 0;
        if (fgetc(input) != EOF || ferror(input)) {
            fclose(input);
            upload_intake_abort(&intake);
            return CHUNK_ASSEMBLY_PART_SIZE_MISMATCH;
        }
        if (fclose(input) != 0) {
            upload_intake_abort(&intake);
            return CHUNK_ASSEMBLY_IO_ERROR;
        }
    }

    if (upload_intake_finish(&intake, expected_size, expected_md5, assembled) !=
        UPLOAD_INTAKE_OK) {
        upload_intake_abort(&intake);
        return CHUNK_ASSEMBLY_FILE_MISMATCH;
    }
    return CHUNK_ASSEMBLY_OK;
}
