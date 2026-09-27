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
