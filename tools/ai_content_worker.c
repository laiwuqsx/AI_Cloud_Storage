#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ai_content_repository.h"
#include "ai_content_task.h"
#include "dashscope_client.h"
#include "fastdfs_storage_client.h"
#include "rabbitmq_content_consumer.h"
#include "runtime_config.h"
#include "storage_client.h"

#define DEFAULT_RETRY_SECONDS 15U
#define DEFAULT_MAX_RETRIES 5U
#define DEFAULT_STALE_SECONDS 300U
#define RECEIVE_TIMEOUT_MS 1000U

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static unsigned int config_unsigned(const char *name, unsigned int fallback,
                                    unsigned int maximum)
{
    const char *text = runtime_config_get(name, NULL);
    char *end;
    unsigned long value;

    if (!text || text[0] == '\0') return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > maximum) return fallback;
    return (unsigned int)value;
}

static void wait_seconds(unsigned int seconds)
{
    while (seconds > 0 && !stop_requested) seconds = sleep(seconds);
}

static int make_temp_path(const char *base_dir, char *directory,
                          size_t directory_size, char *path, size_t path_size)
{
    int length;

    length = snprintf(directory, directory_size, "%s/ai-content-XXXXXX", base_dir);
    if (length < 0 || (size_t)length >= directory_size || !mkdtemp(directory)) return -1;
    length = snprintf(path, path_size, "%s/source", directory);
    if (length < 0 || (size_t)length >= path_size) {
        rmdir(directory);
        return -1;
    }
    return 0;
}

static void cleanup_temp_path(const char *directory, const char *path)
{
    if (path && path[0] != '\0') unlink(path);
    if (directory && directory[0] != '\0') rmdir(directory);
}

static int settle_retry(RabbitMqContentConsumer *consumer,
                        const RabbitMqContentDelivery *delivery,
                        unsigned int retry_seconds)
{
    wait_seconds(retry_seconds);
    if (stop_requested) return -1;
    return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
}

static int handle_processing_error(RabbitMqContentConsumer *consumer,
                                   const RabbitMqContentDelivery *delivery,
                                   const AiContentTask *task,
                                   const AiContentSource *source,
                                   DashScopeResult failure_kind,
                                   const char *message,
                                   unsigned int max_retries,
                                   unsigned int retry_seconds)
{
    int update_result;

    if (failure_kind == DASHSCOPE_PERMANENT_ERROR ||
        source->retry_count >= max_retries - 1U) {
        update_result = fail_ai_content_task(task, message, source->lease_generation);
        if (update_result == 0 || update_result == 1)
            return rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
    }
    update_result = retry_ai_content_task_after(task, message, retry_seconds,
                                                source->lease_generation);
    if (update_result != 0 && update_result != 1)
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
    return settle_retry(consumer, delivery, retry_seconds);
}

static int process_delivery(RabbitMqContentConsumer *consumer,
                            const RabbitMqContentDelivery *delivery,
                            const StorageClient *storage,
                            const DashScopeClient *dashscope,
                            const char *temp_dir,
                            unsigned int max_retries,
                            unsigned int retry_seconds,
                            unsigned int stale_seconds)
{
    AiContentTask task;
    AiContentSource source;
    AiContentClaimResult claim;
    char work_directory[512] = "";
    char local_path[544] = "";
    char description[DASHSCOPE_DESCRIPTION_CAPACITY];
    char error[513] = "";
    float embedding[AI_CONTENT_EXPECTED_DIMENSION];
    DashScopeResult ai_result;
    int result;

    if (delivery->event_id == 0 || delivery->payload[0] == '\0' ||
        parse_ai_content_task(delivery->event_id, delivery->payload, &task) != 0) {
        fprintf(stderr, "ai content worker: rejecting invalid message\n");
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 0);
    }
    claim = claim_ai_content_task(&task, &source);
    if (claim == AI_CONTENT_ALREADY_READY || claim == AI_CONTENT_STALE ||
        claim == AI_CONTENT_MISSING || claim == AI_CONTENT_TERMINAL) {
        return rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
    }
    if (claim == AI_CONTENT_BUSY) {
        wait_seconds(retry_seconds);
        if (!stop_requested) requeue_stale_ai_content_tasks(stale_seconds);
        if (stop_requested) return -1;
        return rabbitmq_content_consumer_reject(consumer,
                                                delivery->delivery_tag, 1);
    }
    if (claim == AI_CONTENT_DEFERRED || claim == AI_CONTENT_CLAIM_ERROR) {
        return settle_retry(consumer, delivery, retry_seconds);
    }
    if (claim != AI_CONTENT_CLAIMED)
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);

    if (make_temp_path(temp_dir, work_directory, sizeof(work_directory),
                       local_path, sizeof(local_path)) != 0) {
        snprintf(error, sizeof(error), "unable to create private worker temp directory");
        return handle_processing_error(consumer, delivery, &task, &source,
                                       DASHSCOPE_TRANSIENT_ERROR, error,
                                       max_retries, retry_seconds);
    }
    if (storage_client_download(storage, source.storage_key, local_path) != 0) {
        snprintf(error, sizeof(error), "FastDFS download failed");
        cleanup_temp_path(work_directory, local_path);
        return handle_processing_error(consumer, delivery, &task, &source,
                                       DASHSCOPE_TRANSIENT_ERROR, error,
                                       max_retries, retry_seconds);
    }
    chmod(local_path, S_IRUSR | S_IWUSR);
    memset(description, 0, sizeof(description));
    ai_result = dashscope_describe_image(dashscope, local_path, source.type,
                                         description, sizeof(description),
                                         error, sizeof(error));
    cleanup_temp_path(work_directory, local_path);
    if (ai_result != DASHSCOPE_OK)
        return handle_processing_error(consumer, delivery, &task, &source,
                                       ai_result, error, max_retries,
                                       retry_seconds);
    memset(error, 0, sizeof(error));
    ai_result = dashscope_embed_text(dashscope, description, embedding,
                                    AI_CONTENT_EXPECTED_DIMENSION,
                                    error, sizeof(error));
    if (ai_result != DASHSCOPE_OK)
        return handle_processing_error(consumer, delivery, &task, &source,
                                       ai_result, error, max_retries,
                                       retry_seconds);
    result = complete_ai_content_task(&task, description, embedding,
                                      sizeof(embedding),
                                      dashscope->embedding_model,
                                      AI_CONTENT_EXPECTED_DIMENSION,
                                      source.lease_generation);
    if (result == 0) {
        printf("ai content worker: indexed md5=%s event=%llu\n",
               task.md5, task.event_id);
        fflush(stdout);
        return rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
    }
    if (result == 1)
        return rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
    return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
}

int main(void)
{
    const char *rabbit_host, *rabbit_user, *rabbit_password;
    const char *api_key, *base_url, *vision_model, *embedding_model;
    const char *fastdfs_config, *public_url, *temp_dir;
    unsigned int rabbit_port, max_retries, retry_seconds, stale_seconds;
    FastDfsStorageContext storage_context;
    StorageClient storage;
    DashScopeClient dashscope;

    runtime_config_init();
    api_key = runtime_config_get("DASHSCOPE_API_KEY", NULL);
    if (!api_key || api_key[0] == '\0') {
        fprintf(stderr, "ai content worker: DASHSCOPE_API_KEY is required; no messages consumed\n");
        return 2;
    }
    rabbit_host = runtime_config_get("RABBITMQ_HOST", "127.0.0.1");
    rabbit_user = runtime_config_get("RABBITMQ_USER", "guest");
    rabbit_password = runtime_config_get("RABBITMQ_PASSWORD", "guest");
    rabbit_port = config_unsigned("RABBITMQ_PORT", 5672U, 65535U);
    base_url = runtime_config_get("DASHSCOPE_BASE_URL",
        "https://dashscope.aliyuncs.com/compatible-mode/v1");
    vision_model = runtime_config_get("DASHSCOPE_VISION_MODEL", "qwen-vl-plus");
    embedding_model = runtime_config_get("DASHSCOPE_EMBEDDING_MODEL",
                                         "text-embedding-v4");
    fastdfs_config = runtime_config_get("FASTDFS_CLIENT_CONFIG",
                                        "/etc/fdfs/client.conf");
    public_url = runtime_config_get("FASTDFS_PUBLIC_BASE_URL",
                                    "http://nginx/storage");
    temp_dir = runtime_config_get("AI_CONTENT_TEMP_DIR", "/tmp");
    max_retries = config_unsigned("AI_CONTENT_MAX_RETRIES",
                                  DEFAULT_MAX_RETRIES, 100U);
    retry_seconds = config_unsigned("AI_CONTENT_RETRY_SECONDS",
                                    DEFAULT_RETRY_SECONDS, 3600U);
    stale_seconds = config_unsigned("AI_CONTENT_STALE_SECONDS",
                                    DEFAULT_STALE_SECONDS, 86400U);
    if (signal(SIGTERM, request_stop) == SIG_ERR ||
        signal(SIGINT, request_stop) == SIG_ERR) return 2;
    fastdfs_storage_context_init(&storage_context, fastdfs_config, public_url);
    if (fastdfs_storage_client_init(&storage, &storage_context) != 0 ||
        dashscope_client_init(&dashscope, base_url, api_key, vision_model,
                              embedding_model) != 0) {
        fprintf(stderr, "ai content worker: invalid configuration\n");
        return 2;
    }

    while (!stop_requested) {
        RabbitMqContentConsumer consumer;

        memset(&consumer, 0, sizeof(consumer));
        if (requeue_stale_ai_content_tasks(stale_seconds) < 0)
            fprintf(stderr, "ai content worker: stale lease recovery failed\n");
        if (rabbitmq_content_consumer_open(&consumer, rabbit_host,
                                           (int)rabbit_port, rabbit_user,
                                           rabbit_password) != 0) {
            fprintf(stderr, "ai content worker: RabbitMQ connection failed\n");
            wait_seconds(retry_seconds);
            continue;
        }
        while (!stop_requested) {
            RabbitMqContentDelivery delivery;
            int receive_result = rabbitmq_content_consumer_receive(
                &consumer, &delivery, RECEIVE_TIMEOUT_MS);

            if (receive_result == 1) continue;
            if (receive_result != 0 ||
                process_delivery(&consumer, &delivery, &storage, &dashscope,
                                 temp_dir, max_retries, retry_seconds,
                                 stale_seconds) != 0) {
                fprintf(stderr, "ai content worker: consumer connection will be reopened\n");
                break;
            }
        }
        rabbitmq_content_consumer_close(&consumer);
        if (!stop_requested) wait_seconds(retry_seconds);
    }
    puts("ai content worker: stopped");
    return 0;
}
