#define _POSIX_C_SOURCE 200809L

#include "chunk_assembler.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXPECT(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } \
} while (0)

static int write_fixture(char path[], const char *contents)
{
    int descriptor = mkstemp(path);
    size_t length = strlen(contents);

    if (descriptor < 0 || write(descriptor, contents, length) != (ssize_t)length ||
        close(descriptor) != 0) return -1;
    return 0;
}

int main(void)
{
    char first[] = "/tmp/chunk-assembler-first-XXXXXX";
    char second[] = "/tmp/chunk-assembler-second-XXXXXX";
    ChunkAssemblyPart parts[2];
    ReceivedUpload assembled;
    FILE *output;
    char contents[12] = {0};

    EXPECT(write_fixture(first, "hello ") == 0, "create first fixture");
    EXPECT(write_fixture(second, "world") == 0, "create second fixture");
    parts[0].path = first;
    parts[0].size = 6;
    parts[1].path = second;
    parts[1].size = 5;
    EXPECT(assemble_chunk_upload(parts, 2, "/tmp", 11,
                                 "5eb63bbbe01eeed093cb22bb8f5acdc3",
                                 &assembled) == CHUNK_ASSEMBLY_OK,
           "assemble and verify chunks");
    output = fopen(assembled.path, "rb");
    EXPECT(output != NULL && fread(contents, 1, 11, output) == 11,
           "read assembled file");
    EXPECT(fclose(output) == 0 && strcmp(contents, "hello world") == 0,
           "assembled bytes preserve order");
    EXPECT(received_upload_discard(&assembled) == 0, "remove assembled file");

    parts[1].size = 4;
    EXPECT(assemble_chunk_upload(parts, 2, "/tmp", 10,
                                 "00000000000000000000000000000000",
                                 &assembled) == CHUNK_ASSEMBLY_PART_SIZE_MISMATCH,
           "reject part larger than recorded metadata");
    unlink(first);
    unlink(second);
    puts("chunk_assembler tests passed");
    return 0;
}
