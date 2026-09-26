#ifndef AI_CLOUD_DASHSCOPE_CLIENT_H
#define AI_CLOUD_DASHSCOPE_CLIENT_H

#include <stddef.h>

#define DASHSCOPE_MODEL_CAPACITY 65
#define DASHSCOPE_DESCRIPTION_CAPACITY 4097

typedef enum {
    DASHSCOPE_OK = 0,
    DASHSCOPE_TRANSIENT_ERROR = 1,
    DASHSCOPE_PERMANENT_ERROR = 2
} DashScopeResult;

typedef int (*DashScopeHttpPost)(void *context, const char *url,
                                 const char *api_key, const char *json_body,
                                 char **response_body, long *http_status);

typedef struct {
    const char *base_url;
    const char *api_key;
    const char *vision_model;
    const char *embedding_model;
    DashScopeHttpPost http_post;
    void *http_context;
} DashScopeClient;

int dashscope_client_init(DashScopeClient *client, const char *base_url,
                          const char *api_key, const char *vision_model,
                          const char *embedding_model);

DashScopeResult dashscope_describe_image(const DashScopeClient *client,
                                         const char *local_path,
                                         const char *file_type,
                                         char *description,
                                         size_t description_size,
                                         char *error, size_t error_size);

DashScopeResult dashscope_embed_text(const DashScopeClient *client,
                                     const char *text, float *embedding,
                                     unsigned int dimension,
                                     char *error, size_t error_size);

/* Exposed because FAISS cosine search requires normalized vectors. */
int normalize_embedding(float *embedding, unsigned int dimension);

#endif
