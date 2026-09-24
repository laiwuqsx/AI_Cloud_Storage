#ifndef AI_CLOUD_CHUNK_ASSEMBLER_H
#define AI_CLOUD_CHUNK_ASSEMBLER_H

#include <stddef.h>
#include <stdint.h>

#include "upload_intake.h"

typedef struct {
    const char *path;
    uint64_t size;
} ChunkAssemblyPart;

typedef enum {
    CHUNK_ASSEMBLY_INVALID_ARGUMENT = -1,
    CHUNK_ASSEMBLY_IO_ERROR = -2,
    CHUNK_ASSEMBLY_PART_SIZE_MISMATCH = -3,
    CHUNK_ASSEMBLY_FILE_MISMATCH = -4,
    CHUNK_ASSEMBLY_OK = 0
} ChunkAssemblyResult;

ChunkAssemblyResult assemble_chunk_upload(const ChunkAssemblyPart *parts,
                                          size_t part_count,
                                          const char *temp_directory,
                                          uint64_t expected_size,
                                          const char *expected_md5,
                                          ReceivedUpload *assembled);

#endif
