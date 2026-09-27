#include "runtime_config.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    char *value;
} ConfigEntry;

static ConfigEntry entries[] = {
    {"MYSQL_HOST", NULL},
    {"MYSQL_USER", NULL},
    {"MYSQL_PASSWORD", NULL},
    {"MYSQL_DATABASE", NULL},
    {"REDIS_HOST", NULL},
    {"REDIS_PORT", NULL},
    {"TOKEN_TTL_SECONDS", NULL},
    {"SHARE_TTL_SECONDS", NULL},
    {"UPLOAD_MAX_BYTES", NULL},
    {"UPLOAD_TEMP_DIR", NULL},
    {"FASTDFS_CLIENT_CONFIG", NULL},
    {"FASTDFS_PUBLIC_BASE_URL", NULL},
    {"CLEANUP_MAX_RETRIES", NULL},
    {"CLEANUP_RETRY_BASE_SECONDS", NULL},
    {"CLEANUP_POLL_INTERVAL_SECONDS", NULL},
    {"CHUNK_UPLOAD_DIR", NULL},
    {"RABBITMQ_HOST", NULL},
    {"RABBITMQ_PORT", NULL},
    {"RABBITMQ_USER", NULL},
    {"RABBITMQ_PASSWORD", NULL},
    {"OUTBOX_MAX_RETRIES", NULL},
    {"OUTBOX_RETRY_BASE_SECONDS", NULL},
    {"OUTBOX_POLL_INTERVAL_SECONDS", NULL},
    {"OUTBOX_STALE_SECONDS", NULL},
    {"DASHSCOPE_BASE_URL", NULL},
    {"DASHSCOPE_API_KEY", NULL},
    {"DASHSCOPE_VISION_MODEL", NULL},
    {"DASHSCOPE_EMBEDDING_MODEL", NULL},
    {"AI_CONTENT_MAX_RETRIES", NULL},
    {"AI_CONTENT_RETRY_SECONDS", NULL},
    {"AI_CONTENT_STALE_SECONDS", NULL},
    {"AI_CONTENT_TEMP_DIR", NULL},
    {"FAISS_INDEX_DIR", NULL},
    {"AI_INDEX_MAX_RETRIES", NULL},
    {"AI_INDEX_RETRY_SECONDS", NULL},
    {"AI_INDEX_STALE_SECONDS", NULL}
};
static int initialized;

static char *duplicate_value(const char *value)
{
    size_t length;
    char *copy;

    if (!value) return NULL;
    length = strlen(value);
    copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length + 1);
    return copy;
}

void runtime_config_init(void)
{
    size_t index;

    if (initialized) return;
    for (index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index) {
        entries[index].value = duplicate_value(getenv(entries[index].name));
    }
    initialized = 1;
}

const char *runtime_config_get(const char *name, const char *fallback)
{
    size_t index;

    if (!initialized) runtime_config_init();
    if (!name) return fallback;
    for (index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index) {
        if (strcmp(entries[index].name, name) == 0) {
            return entries[index].value ? entries[index].value : fallback;
        }
    }
    return fallback;
}
