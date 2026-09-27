#ifndef AI_CLOUD_FAISS_INDEX_STORE_H
#define AI_CLOUD_FAISS_INDEX_STORE_H

#include <stddef.h>

#include "ai_user_index_repository.h"

typedef struct {
    unsigned long long vector_id;
    float score;
} FaissSearchResult;

#ifdef __cplusplus
extern "C" {
#endif

/* Writes, reloads and validates a complete IndexIDMap2(IndexFlatIP) file. */
int faiss_index_write(const char *path, const AiUserIndexVector *vectors,
                      size_t count, char *error, size_t error_size);
int faiss_index_validate(const char *path, size_t expected_count,
                         char *error, size_t error_size);
int faiss_index_search(const char *path, const float *query, size_t dimension,
                       size_t limit, FaissSearchResult *results,
                       size_t *result_count, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
