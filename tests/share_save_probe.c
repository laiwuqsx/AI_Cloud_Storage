#include <stdio.h>

#include "file_repository.h"
#include "runtime_config.h"

int main(int argc, char **argv)
{
    SaveSharedFileResult result;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <user> <share_id>\n", argv[0]);
        return 2;
    }
    runtime_config_init();
    result = save_shared_file(argv[1], argv[2]);
    if (result == SAVE_SHARED_FILE_SAVED) {
        puts("saved");
        return 0;
    }
    if (result == SAVE_SHARED_FILE_ALREADY_OWNED) {
        puts("already_saved");
        return 0;
    }
    fprintf(stderr, "share save failed: %d\n", (int)result);
    return 1;
}
