#ifndef AI_CLOUD_AI_SEARCH_REPOSITORY_H
#define AI_CLOUD_AI_SEARCH_REPOSITORY_H

#include <stddef.h>

#include "faiss_index_store.h"

typedef struct {
    unsigned long long user_file_id;
    unsigned long long size;
    float score;
    char md5[33];
    char file_name[129];
    char type[33];
    char create_time[32];
} AiSearchFile;

/* Preserves FAISS rank while dropping stale or unauthorized vector IDs. */
int filter_owned_ai_search_results(const char *user_name,
                                   const FaissSearchResult *candidates,
                                   size_t candidate_count,
                                   AiSearchFile *files, size_t capacity,
                                   size_t *file_count);

#endif
