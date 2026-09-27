#include "faiss_index_store.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main()
{
    char path[] = "/tmp/ai-cloud-faiss-XXXXXX";
    char error[256] = "";
    AiUserIndexVector vectors[2];
    int descriptor = mkstemp(path);

    assert(descriptor >= 0);
    close(descriptor);
    std::memset(vectors, 0, sizeof(vectors));
    vectors[0].vector_id = 101;
    vectors[0].embedding[0] = 1.0F;
    vectors[1].vector_id = 202;
    vectors[1].embedding[1] = 1.0F;
    assert(faiss_index_write(path, vectors, 2, error, sizeof(error)) == 0);
    assert(faiss_index_validate(path, 2, error, sizeof(error)) == 0);
    assert(faiss_index_validate(path, 1, error, sizeof(error)) == -1);
    vectors[1].embedding[1] = 0.5F;
    assert(faiss_index_write(path, vectors, 2, error, sizeof(error)) == -1);
    unlink(path);
    std::puts("faiss index store tests passed");
    return 0;
}
