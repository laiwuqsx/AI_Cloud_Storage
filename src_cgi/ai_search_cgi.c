#include "fcgi_stdio.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai_content_repository.h"
#include "ai_search_repository.h"
#include "dashscope_client.h"
#include "faiss_index_store.h"
#include "http_response.h"
#include "json_util.h"
#include "md5.h"
#include "runtime_config.h"
#include "token_service.h"
#include "user_validation.h"

#define MAX_BODY_SIZE 4096
#define MAX_QUERY_SIZE 513
#define SEARCH_LIMIT 10U

static int read_body(char *body, size_t size)
{
    const char *length_text = getenv("CONTENT_LENGTH");
    char *end;
    unsigned long length;

    if (!length_text || length_text[0] == '\0') return -1;
    errno = 0;
    length = strtoul(length_text, &end, 10);
    if (errno != 0 || *end != '\0' || length == 0 || length >= size) return -1;
    if (fread(body, 1, (size_t)length, stdin) != (size_t)length) return -1;
    body[length] = '\0';
    return 0;
}

static int valid_query(const char *query)
{
    size_t index, length;

    if (!query || (length = strlen(query)) == 0 || length >= MAX_QUERY_SIZE)
        return 0;
    for (index = 0; index < length; ++index) {
        unsigned char value = (unsigned char)query[index];
        if (value < 0x20 && value != '\t' && value != '\n' && value != '\r')
            return 0;
    }
    return 1;
}

static int make_index_path(const char *directory, const char *user,
                           char *path, size_t path_size)
{
    char user_md5[33];
    int length;

    md5_hex((const unsigned char *)user, strlen(user), user_md5);
    length = snprintf(path, path_size, "%s/%s.index.bin", directory, user_md5);
    return length > 0 && (size_t)length < path_size ? 0 : -1;
}

static void write_json_string(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");

    putchar('"');
    while (*cursor) {
        if (*cursor == '"' || *cursor == '\\') putchar('\\');
        if (*cursor == '\n') fputs("\\n", stdout);
        else if (*cursor == '\r') fputs("\\r", stdout);
        else if (*cursor == '\t') fputs("\\t", stdout);
        else if (*cursor >= 0x20) putchar(*cursor);
        ++cursor;
    }
    putchar('"');
}

static void write_results(const AiSearchFile *files, size_t count)
{
    size_t index;

    write_json_header();
    fputs("{\"code\":0,\"files\":[", stdout);
    for (index = 0; index < count; ++index) {
        if (index > 0) putchar(',');
        printf("{\"user_file_id\":%llu,\"score\":%.7g,\"md5\":",
               files[index].user_file_id, (double)files[index].score);
        write_json_string(files[index].md5);
        fputs(",\"file_name\":", stdout); write_json_string(files[index].file_name);
        printf(",\"size\":%llu,\"type\":", files[index].size);
        write_json_string(files[index].type);
        fputs(",\"create_time\":", stdout);
        write_json_string(files[index].create_time);
        putchar('}');
    }
    fputs("]}\n", stdout);
}

int main(void)
{
    const char *api_key, *base_url, *embedding_model, *index_directory;
    char body[MAX_BODY_SIZE], user[33], token[65], query[MAX_QUERY_SIZE];
    char index_path[1024], error[513];
    float embedding[AI_CONTENT_EXPECTED_DIMENSION];
    FaissSearchResult candidates[SEARCH_LIMIT];
    AiSearchFile files[SEARCH_LIMIT];
    size_t candidate_count, file_count;
    DashScopeClient dashscope;

    runtime_config_init();
    api_key = runtime_config_get("DASHSCOPE_API_KEY", NULL);
    base_url = runtime_config_get("DASHSCOPE_BASE_URL",
        "https://dashscope.aliyuncs.com/compatible-mode/v1");
    embedding_model = runtime_config_get("DASHSCOPE_EMBEDDING_MODEL",
                                         "text-embedding-v4");
    index_directory = runtime_config_get("FAISS_INDEX_DIR", "/data/faiss/users");
    while (FCGI_Accept() >= 0) {
        if (!getenv("REQUEST_METHOD") ||
            strcmp(getenv("REQUEST_METHOD"), "POST") != 0) {
            write_json_response(1, "method not allowed", NULL);
            continue;
        }
        if (read_body(body, sizeof(body)) != 0 ||
            json_get_string(body, "user", user, sizeof(user)) != 0 ||
            json_get_string(body, "token", token, sizeof(token)) != 0 ||
            json_get_string(body, "query", query, sizeof(query)) != 0 ||
            !validate_username(user) || !valid_query(query)) {
            write_json_response(3, "invalid search request", NULL);
            continue;
        }
        if (verify_session_token(user, token) != 0) {
            write_json_response(4, "token error", NULL);
            continue;
        }
        if (!api_key || api_key[0] == '\0' ||
            dashscope_client_init(&dashscope, base_url, api_key, "unused",
                                  embedding_model) != 0) {
            write_json_response(6, "AI search is not configured", NULL);
            continue;
        }
        memset(error, 0, sizeof(error));
        if (dashscope_embed_text(&dashscope, query, embedding,
                AI_CONTENT_EXPECTED_DIMENSION, error, sizeof(error)) != DASHSCOPE_OK) {
            write_json_response(6, "query embedding failed", NULL);
            continue;
        }
        if (make_index_path(index_directory, user, index_path,
                            sizeof(index_path)) != 0) {
            write_json_response(6, "invalid index path", NULL);
            continue;
        }
        if (access(index_path, R_OK) != 0) {
            write_results(files, 0);
            continue;
        }
        if (faiss_index_search(index_path, embedding,
                AI_CONTENT_EXPECTED_DIMENSION, SEARCH_LIMIT, candidates,
                &candidate_count, error, sizeof(error)) != 0) {
            write_json_response(6, "index search failed", NULL);
            continue;
        }
        if (filter_owned_ai_search_results(user, candidates, candidate_count,
                                           files, SEARCH_LIMIT,
                                           &file_count) != 0) {
            write_json_response(6, "database error", NULL);
            continue;
        }
        write_results(files, file_count);
    }
    return 0;
}
