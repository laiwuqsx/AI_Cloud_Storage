#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "dashscope_client.h"

typedef struct {
    int calls;
    long status;
    int fail_transport;
} FakeHttp;

static int fake_post(void *opaque, const char *url, const char *api_key,
                     const char *json_body, char **response_body,
                     long *http_status)
{
    FakeHttp *state = opaque;

    ++state->calls;
    if (strcmp(api_key, "test-key") != 0) return -1;
    if (state->fail_transport) return -1;
    *http_status = state->status;
    if (strstr(url, "/chat/completions")) {
        if (!strstr(json_body, "data:image/jpeg;base64,AQIDBA==")) return -1;
        *response_body = strdup(
            "{\"choices\":[{\"message\":{\"content\":\"一只猫坐在窗边\"}}]}");
        return *response_body ? 0 : -1;
    }
    if (strstr(url, "/embeddings")) {
        cJSON *root = cJSON_CreateObject();
        cJSON *data = cJSON_CreateArray();
        cJSON *item = cJSON_CreateObject();
        cJSON *embedding = cJSON_CreateArray();
        int index;

        if (!strstr(json_body, "\"dimensions\":1024")) return -1;
        cJSON_AddItemToObject(root, "data", data);
        cJSON_AddItemToArray(data, item);
        cJSON_AddItemToObject(item, "embedding", embedding);
        for (index = 0; index < 1024; ++index)
            cJSON_AddItemToArray(embedding, cJSON_CreateNumber(1.0));
        *response_body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return *response_body ? 0 : -1;
    }
    return -1;
}

#define EXPECT(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char path[] = "/tmp/ai-cloud-image-XXXXXX";
    const unsigned char image[] = {1, 2, 3, 4};
    char description[DASHSCOPE_DESCRIPTION_CAPACITY];
    char error[256] = "";
    float embedding[1024];
    DashScopeClient client;
    FakeHttp http = {0, 200, 0};
    int descriptor;
    double sum = 0.0;
    int index;

    descriptor = mkstemp(path);
    EXPECT(descriptor >= 0, "temporary image creation");
    EXPECT(write(descriptor, image, sizeof(image)) == (ssize_t)sizeof(image),
           "temporary image write");
    close(descriptor);
    EXPECT(dashscope_client_init(&client, "https://example.test/v1/",
                                 "test-key", "vision-test",
                                 "embedding-test") == 0,
           "client initialization");
    client.http_post = fake_post;
    client.http_context = &http;
    EXPECT(dashscope_describe_image(&client, path, "JPG", description,
                                    sizeof(description), error,
                                    sizeof(error)) == DASHSCOPE_OK,
           "image description request");
    EXPECT(strcmp(description, "一只猫坐在窗边") == 0,
           "image description parsing");
    EXPECT(dashscope_embed_text(&client, description, embedding, 1024,
                                error, sizeof(error)) == DASHSCOPE_OK,
           "embedding request");
    for (index = 0; index < 1024; ++index)
        sum += (double)embedding[index] * embedding[index];
    EXPECT(fabs(sum - 1.0) < 0.0001, "embedding L2 normalization");
    EXPECT(http.calls == 2, "two API calls");

    http.status = 429;
    memset(error, 0, sizeof(error));
    EXPECT(dashscope_embed_text(&client, description, embedding, 1024,
                                error, sizeof(error)) == DASHSCOPE_TRANSIENT_ERROR,
           "rate limit is retryable");
    http.status = 401;
    EXPECT(dashscope_embed_text(&client, description, embedding, 1024,
                                error, sizeof(error)) == DASHSCOPE_PERMANENT_ERROR,
           "authentication failure is terminal");
    EXPECT(dashscope_describe_image(&client, path, "exe", description,
                                    sizeof(description), error,
                                    sizeof(error)) == DASHSCOPE_PERMANENT_ERROR,
           "unsupported file type is terminal");
    unlink(path);
    puts("dashscope_client tests passed");
    return 0;
}
