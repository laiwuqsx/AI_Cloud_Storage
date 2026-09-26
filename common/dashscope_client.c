#include "dashscope_client.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#define MAX_IMAGE_BYTES (20U * 1024U * 1024U)
#define MAX_RESPONSE_BYTES (2U * 1024U * 1024U)

typedef struct {
    char *data;
    size_t size;
} ResponseBuffer;

static void set_error(char *error, size_t error_size, const char *message)
{
    if (!error || error_size == 0) return;
    snprintf(error, error_size, "%s", message ? message : "unknown error");
}

static size_t receive_response(void *contents, size_t size, size_t count, void *opaque)
{
    ResponseBuffer *buffer = opaque;
    size_t bytes = size * count;
    char *expanded;

    if (bytes > MAX_RESPONSE_BYTES || buffer->size > MAX_RESPONSE_BYTES - bytes)
        return 0;
    expanded = realloc(buffer->data, buffer->size + bytes + 1);
    if (!expanded) return 0;
    buffer->data = expanded;
    memcpy(buffer->data + buffer->size, contents, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = '\0';
    return bytes;
}

static int curl_http_post(void *unused, const char *url, const char *api_key,
                          const char *json_body, char **response_body,
                          long *http_status)
{
    CURL *curl;
    CURLcode result;
    struct curl_slist *headers = NULL;
    ResponseBuffer response = {NULL, 0};
    char authorization[1024];
    int length;

    (void)unused;
    if (!url || !api_key || !json_body || !response_body || !http_status) return -1;
    *response_body = NULL;
    *http_status = 0;
    length = snprintf(authorization, sizeof(authorization),
                      "Authorization: Bearer %s", api_key);
    if (length < 0 || (size_t)length >= sizeof(authorization)) return -1;
    curl = curl_easy_init();
    if (!curl) return -1;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, authorization);
    if (!headers || curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         (long)strlen(json_body)) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response) != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(response.data);
        return -1;
    }
    result = curl_easy_perform(curl);
    if (result == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        free(response.data);
        return -1;
    }
    if (!response.data) {
        response.data = malloc(1);
        if (!response.data) return -1;
        response.data[0] = '\0';
    }
    *response_body = response.data;
    return 0;
}

static const char *image_mime_type(const char *file_type)
{
    char lowered[16];
    size_t index, length;

    if (!file_type) return NULL;
    while (*file_type == '.') ++file_type;
    length = strlen(file_type);
    if (length == 0 || length >= sizeof(lowered)) return NULL;
    for (index = 0; index < length; ++index)
        lowered[index] = (char)tolower((unsigned char)file_type[index]);
    lowered[length] = '\0';
    if (strcmp(lowered, "jpg") == 0 || strcmp(lowered, "jpeg") == 0)
        return "image/jpeg";
    if (strcmp(lowered, "png") == 0) return "image/png";
    if (strcmp(lowered, "webp") == 0) return "image/webp";
    if (strcmp(lowered, "gif") == 0) return "image/gif";
    return NULL;
}

static char *read_file(const char *path, size_t *size)
{
    FILE *file;
    long length;
    char *content;

    if (!path || !size) return NULL;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        (unsigned long)length > MAX_IMAGE_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    content = malloc((size_t)length);
    if (!content || fread(content, 1, (size_t)length, file) != (size_t)length) {
        free(content);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return content;
}

static char *base64_encode(const unsigned char *input, size_t length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t output_length, input_index = 0, output_index = 0;
    char *output;

    if (!input || length > (SIZE_MAX - 2U) / 3U) return NULL;
    output_length = 4U * ((length + 2U) / 3U);
    output = malloc(output_length + 1U);
    if (!output) return NULL;
    while (input_index + 3U <= length) {
        unsigned int value = ((unsigned int)input[input_index] << 16) |
                             ((unsigned int)input[input_index + 1] << 8) |
                             input[input_index + 2];
        input_index += 3U;
        output[output_index++] = alphabet[(value >> 18) & 63U];
        output[output_index++] = alphabet[(value >> 12) & 63U];
        output[output_index++] = alphabet[(value >> 6) & 63U];
        output[output_index++] = alphabet[value & 63U];
    }
    if (input_index < length) {
        unsigned int value = (unsigned int)input[input_index] << 16;
        output[output_index++] = alphabet[(value >> 18) & 63U];
        if (input_index + 1U < length) {
            value |= (unsigned int)input[input_index + 1U] << 8;
            output[output_index++] = alphabet[(value >> 12) & 63U];
            output[output_index++] = alphabet[(value >> 6) & 63U];
        } else {
            output[output_index++] = alphabet[(value >> 12) & 63U];
            output[output_index++] = '=';
        }
        output[output_index++] = '=';
    }
    output[output_index] = '\0';
    return output;
}

static DashScopeResult classify_http_result(int transport_result, long status,
                                             char *error, size_t error_size)
{
    if (transport_result != 0) {
        set_error(error, error_size, "DashScope network request failed");
        return DASHSCOPE_TRANSIENT_ERROR;
    }
    if (status >= 200 && status < 300) return DASHSCOPE_OK;
    if (status == 408 || status == 409 || status == 429 || status >= 500) {
        set_error(error, error_size, "DashScope temporary HTTP failure");
        return DASHSCOPE_TRANSIENT_ERROR;
    }
    set_error(error, error_size, "DashScope rejected the request");
    return DASHSCOPE_PERMANENT_ERROR;
}

static int make_endpoint(const char *base, const char *path,
                         char *output, size_t output_size)
{
    size_t base_length;
    int length;

    if (!base || !path || !output) return -1;
    base_length = strlen(base);
    while (base_length > 0 && base[base_length - 1] == '/') --base_length;
    length = snprintf(output, output_size, "%.*s/%s", (int)base_length, base, path);
    return length < 0 || (size_t)length >= output_size ? -1 : 0;
}

int dashscope_client_init(DashScopeClient *client, const char *base_url,
                          const char *api_key, const char *vision_model,
                          const char *embedding_model)
{
    if (!client || !base_url || base_url[0] == '\0' || !api_key ||
        api_key[0] == '\0' || !vision_model || vision_model[0] == '\0' ||
        !embedding_model || embedding_model[0] == '\0') return -1;
    memset(client, 0, sizeof(*client));
    client->base_url = base_url;
    client->api_key = api_key;
    client->vision_model = vision_model;
    client->embedding_model = embedding_model;
    client->http_post = curl_http_post;
    return 0;
}

DashScopeResult dashscope_describe_image(const DashScopeClient *client,
                                         const char *local_path,
                                         const char *file_type,
                                         char *description,
                                         size_t description_size,
                                         char *error, size_t error_size)
{
    const char *mime = image_mime_type(file_type);
    char endpoint[1024];
    char *file_content = NULL, *encoded = NULL, *data_url = NULL;
    char *request_body = NULL, *response_body = NULL;
    size_t file_size, data_url_size;
    cJSON *root = NULL, *messages, *message, *content, *image, *image_url, *text;
    cJSON *response = NULL, *choices, *first, *response_message, *response_content;
    DashScopeResult result = DASHSCOPE_PERMANENT_ERROR;
    long http_status = 0;
    int transport_result;

    if (!client || !client->http_post || !description || description_size < 2 ||
        !mime || make_endpoint(client->base_url, "chat/completions", endpoint,
                               sizeof(endpoint)) != 0) {
        set_error(error, error_size, "unsupported image type or invalid configuration");
        return DASHSCOPE_PERMANENT_ERROR;
    }
    file_content = read_file(local_path, &file_size);
    encoded = file_content ? base64_encode((unsigned char *)file_content, file_size) : NULL;
    if (!encoded) {
        set_error(error, error_size, "unable to read image content");
        goto done;
    }
    data_url_size = strlen(mime) + strlen(encoded) + 14U;
    data_url = malloc(data_url_size);
    if (!data_url) goto done;
    snprintf(data_url, data_url_size, "data:%s;base64,%s", mime, encoded);

    root = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    message = cJSON_CreateObject();
    content = cJSON_CreateArray();
    image = cJSON_CreateObject();
    image_url = cJSON_CreateObject();
    text = cJSON_CreateObject();
    if (!root || !messages || !message || !content || !image || !image_url || !text)
        goto done;
    cJSON_AddStringToObject(root, "model", client->vision_model);
    cJSON_AddNumberToObject(root, "temperature", 0);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddItemToArray(messages, message);
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddItemToObject(message, "content", content);
    cJSON_AddStringToObject(image, "type", "image_url");
    cJSON_AddItemToObject(image, "image_url", image_url);
    cJSON_AddStringToObject(image_url, "url", data_url);
    cJSON_AddItemToArray(content, image);
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text",
        "请用简洁中文描述图片中的主体、场景和重要细节，用于语义检索。只返回描述文本。");
    cJSON_AddItemToArray(content, text);
    request_body = cJSON_PrintUnformatted(root);
    if (!request_body) goto done;
    transport_result = client->http_post(client->http_context, endpoint,
                                         client->api_key, request_body,
                                         &response_body, &http_status);
    result = classify_http_result(transport_result, http_status, error, error_size);
    if (result != DASHSCOPE_OK) goto done;
    response = cJSON_Parse(response_body);
    choices = response ? cJSON_GetObjectItemCaseSensitive(response, "choices") : NULL;
    first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    response_message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
    response_content = response_message ?
        cJSON_GetObjectItemCaseSensitive(response_message, "content") : NULL;
    if (!cJSON_IsString(response_content) || !response_content->valuestring ||
        response_content->valuestring[0] == '\0' ||
        strlen(response_content->valuestring) >= description_size) {
        set_error(error, error_size, "invalid DashScope vision response");
        result = DASHSCOPE_TRANSIENT_ERROR;
        goto done;
    }
    memcpy(description, response_content->valuestring,
           strlen(response_content->valuestring) + 1U);
    result = DASHSCOPE_OK;

done:
    free(file_content);
    free(encoded);
    free(data_url);
    cJSON_free(request_body);
    free(response_body);
    cJSON_Delete(root);
    cJSON_Delete(response);
    if (result != DASHSCOPE_OK && error && error[0] == '\0')
        set_error(error, error_size, "unable to build DashScope vision request");
    return result;
}

int normalize_embedding(float *embedding, unsigned int dimension)
{
    double sum = 0.0, norm;
    unsigned int index;

    if (!embedding || dimension == 0) return -1;
    for (index = 0; index < dimension; ++index) {
        if (!isfinite(embedding[index])) return -1;
        sum += (double)embedding[index] * embedding[index];
    }
    if (!isfinite(sum) || sum <= 0.0) return -1;
    norm = sqrt(sum);
    for (index = 0; index < dimension; ++index)
        embedding[index] = (float)(embedding[index] / norm);
    return 0;
}

DashScopeResult dashscope_embed_text(const DashScopeClient *client,
                                     const char *text, float *embedding,
                                     unsigned int dimension,
                                     char *error, size_t error_size)
{
    char endpoint[1024];
    char *request_body = NULL, *response_body = NULL;
    cJSON *root = NULL, *response = NULL, *data, *first, *values;
    DashScopeResult result = DASHSCOPE_PERMANENT_ERROR;
    long http_status = 0;
    unsigned int index;
    int transport_result;

    if (!client || !client->http_post || !text || text[0] == '\0' || !embedding ||
        dimension == 0 || make_endpoint(client->base_url, "embeddings", endpoint,
                                        sizeof(endpoint)) != 0) {
        set_error(error, error_size, "invalid embedding request");
        return DASHSCOPE_PERMANENT_ERROR;
    }
    root = cJSON_CreateObject();
    if (!root) goto done;
    cJSON_AddStringToObject(root, "model", client->embedding_model);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddNumberToObject(root, "dimensions", dimension);
    cJSON_AddStringToObject(root, "encoding_format", "float");
    request_body = cJSON_PrintUnformatted(root);
    if (!request_body) goto done;
    transport_result = client->http_post(client->http_context, endpoint,
                                         client->api_key, request_body,
                                         &response_body, &http_status);
    result = classify_http_result(transport_result, http_status, error, error_size);
    if (result != DASHSCOPE_OK) goto done;
    response = cJSON_Parse(response_body);
    data = response ? cJSON_GetObjectItemCaseSensitive(response, "data") : NULL;
    first = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
    values = first ? cJSON_GetObjectItemCaseSensitive(first, "embedding") : NULL;
    if (!cJSON_IsArray(values) || cJSON_GetArraySize(values) != (int)dimension) {
        set_error(error, error_size, "unexpected embedding dimension");
        result = DASHSCOPE_TRANSIENT_ERROR;
        goto done;
    }
    for (index = 0; index < dimension; ++index) {
        cJSON *value = cJSON_GetArrayItem(values, (int)index);
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
            set_error(error, error_size, "invalid embedding value");
            result = DASHSCOPE_TRANSIENT_ERROR;
            goto done;
        }
        embedding[index] = (float)value->valuedouble;
    }
    if (normalize_embedding(embedding, dimension) != 0) {
        set_error(error, error_size, "embedding cannot be normalized");
        result = DASHSCOPE_TRANSIENT_ERROR;
        goto done;
    }
    result = DASHSCOPE_OK;

done:
    cJSON_free(request_body);
    free(response_body);
    cJSON_Delete(root);
    cJSON_Delete(response);
    if (result != DASHSCOPE_OK && error && error[0] == '\0')
        set_error(error, error_size, "unable to build DashScope embedding request");
    return result;
}
