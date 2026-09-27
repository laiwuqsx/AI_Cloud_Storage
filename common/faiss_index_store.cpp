#include "faiss_index_store.h"

#include <faiss/IndexFlat.h>
#include <faiss/MetaIndexes.h>
#include <faiss/index_io.h>

#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <vector>

namespace {

void set_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size > 0) std::snprintf(error, error_size, "%s", message);
}

bool valid_vector(const AiUserIndexVector &vector)
{
    double norm = 0.0;
    for (size_t i = 0; i < AI_CONTENT_EXPECTED_DIMENSION; ++i) {
        if (!std::isfinite(vector.embedding[i])) return false;
        norm += static_cast<double>(vector.embedding[i]) * vector.embedding[i];
    }
    return std::isfinite(norm) && norm > 0.99 && norm < 1.01 &&
           vector.vector_id > 0 &&
           vector.vector_id <= static_cast<unsigned long long>(
               std::numeric_limits<faiss::Index::idx_t>::max());
}

bool valid_embedding(const float *embedding, size_t dimension)
{
    double norm = 0.0;
    if (!embedding || dimension != AI_CONTENT_EXPECTED_DIMENSION) return false;
    for (size_t i = 0; i < dimension; ++i) {
        if (!std::isfinite(embedding[i])) return false;
        norm += static_cast<double>(embedding[i]) * embedding[i];
    }
    return std::isfinite(norm) && norm > 0.99 && norm < 1.01;
}

}  // namespace

extern "C" int faiss_index_validate(const char *path, size_t expected_count,
                                     char *error, size_t error_size)
{
    try {
        if (!path || path[0] == '\0') {
            set_error(error, error_size, "missing index path");
            return -1;
        }
        std::unique_ptr<faiss::Index> index(faiss::read_index(path));
        if (!index || index->d != AI_CONTENT_EXPECTED_DIMENSION ||
            index->metric_type != faiss::METRIC_INNER_PRODUCT ||
            index->ntotal != static_cast<faiss::Index::idx_t>(expected_count) ||
            dynamic_cast<faiss::IndexIDMap2 *>(index.get()) == nullptr) {
            set_error(error, error_size, "FAISS index validation failed");
            return -1;
        }
        return 0;
    } catch (const std::exception &exception) {
        set_error(error, error_size, exception.what());
        return -1;
    }
}

extern "C" int faiss_index_write(const char *path,
                                  const AiUserIndexVector *vectors,
                                  size_t count, char *error, size_t error_size)
{
    try {
        if (!path || path[0] == '\0' || (count > 0 && !vectors)) {
            set_error(error, error_size, "invalid FAISS write arguments");
            return -1;
        }
        faiss::IndexFlatIP flat(AI_CONTENT_EXPECTED_DIMENSION);
        faiss::IndexIDMap2 index(&flat);
        std::vector<float> values(count * AI_CONTENT_EXPECTED_DIMENSION);
        std::vector<faiss::Index::idx_t> ids(count);
        for (size_t row = 0; row < count; ++row) {
            if (!valid_vector(vectors[row])) {
                set_error(error, error_size, "invalid embedding or vector id");
                return -1;
            }
            ids[row] = static_cast<faiss::Index::idx_t>(vectors[row].vector_id);
            for (size_t column = 0; column < AI_CONTENT_EXPECTED_DIMENSION; ++column)
                values[row * AI_CONTENT_EXPECTED_DIMENSION + column] =
                    vectors[row].embedding[column];
        }
        if (count > 0)
            index.add_with_ids(static_cast<faiss::Index::idx_t>(count), values.data(),
                               ids.data());
        faiss::write_index(&index, path);
        return faiss_index_validate(path, count, error, error_size);
    } catch (const std::exception &exception) {
        set_error(error, error_size, exception.what());
        return -1;
    }
}

extern "C" int faiss_index_search(const char *path, const float *query,
                                    size_t dimension, size_t limit,
                                    FaissSearchResult *results,
                                    size_t *result_count,
                                    char *error, size_t error_size)
{
    try {
        if (!path || path[0] == '\0' || !valid_embedding(query, dimension) ||
            limit == 0 || limit > 50 || !results || !result_count) {
            set_error(error, error_size, "invalid FAISS search arguments");
            return -1;
        }
        *result_count = 0;
        std::unique_ptr<faiss::Index> index(faiss::read_index(path));
        if (!index || index->d != AI_CONTENT_EXPECTED_DIMENSION ||
            index->metric_type != faiss::METRIC_INNER_PRODUCT ||
            dynamic_cast<faiss::IndexIDMap2 *>(index.get()) == nullptr) {
            set_error(error, error_size, "unsupported FAISS index");
            return -1;
        }
        std::vector<float> distances(limit);
        std::vector<faiss::Index::idx_t> labels(limit);
        index->search(1, query, static_cast<faiss::Index::idx_t>(limit),
                      distances.data(), labels.data());
        for (size_t i = 0; i < limit; ++i) {
            if (labels[i] < 0) break;
            if (!std::isfinite(distances[i])) continue;
            results[*result_count].vector_id =
                static_cast<unsigned long long>(labels[i]);
            results[*result_count].score = distances[i];
            ++*result_count;
        }
        return 0;
    } catch (const std::exception &exception) {
        set_error(error, error_size, exception.what());
        return -1;
    }
}
