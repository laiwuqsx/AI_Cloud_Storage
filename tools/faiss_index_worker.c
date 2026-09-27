#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ai_user_index_repository.h"
#include "ai_user_index_task.h"
#include "faiss_index_store.h"
#include "md5.h"
#include "rabbitmq_content_consumer.h"
#include "runtime_config.h"

#define DEFAULT_RETRY_SECONDS 10U
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

static int ensure_private_directory(const char *path)
{
    struct stat state;

    if (mkdir(path, S_IRWXU) == 0) return 0;
    if (errno != EEXIST || stat(path, &state) != 0 || !S_ISDIR(state.st_mode))
        return -1;
    return chmod(path, S_IRWXU);
}

static int make_user_paths(const char *directory, const char *user_name,
                           char *index_path, size_t index_size,
                           char *lock_path, size_t lock_size)
{
    char user_md5[33];
    int index_length, lock_length;

    md5_hex((const unsigned char *)user_name, strlen(user_name), user_md5);
    index_length = snprintf(index_path, index_size, "%s/%s.index.bin",
                            directory, user_md5);
    lock_length = snprintf(lock_path, lock_size, "%s/%s.lock",
                           directory, user_md5);
    return index_length > 0 && (size_t)index_length < index_size &&
           lock_length > 0 && (size_t)lock_length < lock_size ? 0 : -1;
}

static int install_snapshot(const char *index_path,
                            const AiUserIndexSnapshot *snapshot,
                            char *error, size_t error_size)
{
    char temporary_path[1024];
    int descriptor = -1, directory_descriptor = -1;
    int length, result = -1;
    char *slash;

    length = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.XXXXXX",
                      index_path);
    if (length < 0 || (size_t)length >= sizeof(temporary_path)) {
        snprintf(error, error_size, "index path is too long");
        return -1;
    }
    descriptor = mkstemp(temporary_path);
    if (descriptor < 0 || fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        snprintf(error, error_size, "unable to create private index temp file");
        goto done;
    }
    close(descriptor);
    descriptor = -1;
    if (faiss_index_write(temporary_path, snapshot->vectors, snapshot->count,
                          error, error_size) != 0) goto done;
    descriptor = open(temporary_path, O_RDONLY);
    if (descriptor < 0 || fsync(descriptor) != 0) {
        snprintf(error, error_size, "unable to fsync index temp file");
        goto done;
    }
    close(descriptor);
    descriptor = -1;
    if (rename(temporary_path, index_path) != 0) {
        snprintf(error, error_size, "unable to atomically replace index");
        goto done;
    }
    slash = strrchr(temporary_path, '/');
    if (slash) *slash = '\0';
    directory_descriptor = open(slash ? temporary_path : ".", O_RDONLY);
    if (directory_descriptor < 0 || fsync(directory_descriptor) != 0) {
        snprintf(error, error_size, "unable to fsync index directory");
        goto done;
    }
    result = 0;

done:
    if (descriptor >= 0) close(descriptor);
    if (directory_descriptor >= 0) close(directory_descriptor);
    if (result != 0) unlink(temporary_path);
    return result;
}

static int defer_claimed_task(RabbitMqContentConsumer *consumer,
                              const RabbitMqContentDelivery *delivery,
                              const AiUserIndexTask *task,
                              const AiUserIndexSource *source,
                              const char *error, unsigned int max_retries,
                              unsigned int retry_seconds)
{
    int state_result;

    if (source->retry_count + 1U >= max_retries)
        state_result = fail_ai_user_index_task(task, error,
                                              source->lease_generation);
    else
        state_result = retry_ai_user_index_task_after(task, error, retry_seconds,
                                                     source->lease_generation);
    if (state_result == 1)
        return rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
    if (state_result != 0)
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
    if (source->retry_count + 1U >= max_retries)
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 0);
    wait_seconds(retry_seconds);
    if (stop_requested) return -1;
    return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
}

static int process_delivery(RabbitMqContentConsumer *consumer,
                            const RabbitMqContentDelivery *delivery,
                            const char *index_directory,
                            unsigned int max_retries,
                            unsigned int retry_seconds,
                            unsigned int stale_seconds)
{
    AiUserIndexTask task;
    AiUserIndexSource source;
    AiUserIndexSnapshot snapshot;
    AiUserIndexClaimResult claim;
    char index_path[1024], lock_path[1024], error[513] = "";
    int lock_descriptor = -1, completion, result = -1;

    if (parse_ai_user_index_task(delivery->event_id, delivery->payload, &task) != 0)
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 0);
    if (make_user_paths(index_directory, task.user_name, index_path,
                        sizeof(index_path), lock_path, sizeof(lock_path)) != 0)
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 0);
    lock_descriptor = open(lock_path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (lock_descriptor < 0 || flock(lock_descriptor, LOCK_EX) != 0) {
        if (lock_descriptor >= 0) close(lock_descriptor);
        wait_seconds(retry_seconds);
        if (stop_requested) return -1;
        return rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
    }

    claim = claim_ai_user_index_task(&task, &source);
    if (claim == AI_USER_INDEX_ALREADY_APPLIED || claim == AI_USER_INDEX_STALE ||
        claim == AI_USER_INDEX_MISSING || claim == AI_USER_INDEX_TERMINAL) {
        result = rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
        goto done;
    }
    if (claim == AI_USER_INDEX_BUSY) {
        wait_seconds(retry_seconds);
        if (!stop_requested) requeue_stale_ai_user_index_tasks(stale_seconds);
        if (stop_requested) goto done;
        result = rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
        goto done;
    }
    if (claim == AI_USER_INDEX_WAITING_CONTENT ||
        claim == AI_USER_INDEX_DEFERRED || claim == AI_USER_INDEX_CLAIM_ERROR) {
        wait_seconds(retry_seconds);
        if (stop_requested) goto done;
        result = rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
        goto done;
    }
    if (claim != AI_USER_INDEX_CLAIMED) {
        result = rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
        goto done;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (load_ai_user_index_snapshot(&task, source.lease_generation, &snapshot) != 0) {
        result = defer_claimed_task(consumer, delivery, &task, &source,
                                    "unable to load user index snapshot",
                                    max_retries, retry_seconds);
        goto done;
    }
    if (install_snapshot(index_path, &snapshot, error, sizeof(error)) != 0) {
        result = defer_claimed_task(consumer, delivery, &task, &source,
                                    error[0] ? error : "FAISS index write failed",
                                    max_retries, retry_seconds);
        free_ai_user_index_snapshot(&snapshot);
        goto done;
    }
    if (task.type == AI_INDEX_EVENT_USER_FILE_ADDED)
        completion = complete_ai_user_index_add(&task,
            snapshot.next_index_version, source.lease_generation);
    else
        completion = complete_ai_user_index_remove(&task,
            snapshot.next_index_version, source.lease_generation);
    if (completion == 0) {
        printf("faiss index worker: user=%s event=%llu vectors=%zu version=%llu\n",
               task.user_name, task.event_id, snapshot.count,
               snapshot.next_index_version);
        fflush(stdout);
        result = rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
    } else if (completion == 1) {
        result = rabbitmq_content_consumer_ack(consumer, delivery->delivery_tag);
    } else {
        result = rabbitmq_content_consumer_reject(consumer, delivery->delivery_tag, 1);
    }
    free_ai_user_index_snapshot(&snapshot);

done:
    flock(lock_descriptor, LOCK_UN);
    close(lock_descriptor);
    return result;
}

int main(void)
{
    const char *rabbit_host, *rabbit_user, *rabbit_password, *index_directory;
    unsigned int rabbit_port, max_retries, retry_seconds, stale_seconds;

    runtime_config_init();
    rabbit_host = runtime_config_get("RABBITMQ_HOST", "127.0.0.1");
    rabbit_user = runtime_config_get("RABBITMQ_USER", "guest");
    rabbit_password = runtime_config_get("RABBITMQ_PASSWORD", "guest");
    index_directory = runtime_config_get("FAISS_INDEX_DIR", "/data/faiss/users");
    rabbit_port = config_unsigned("RABBITMQ_PORT", 5672U, 65535U);
    max_retries = config_unsigned("AI_INDEX_MAX_RETRIES", DEFAULT_MAX_RETRIES, 100U);
    retry_seconds = config_unsigned("AI_INDEX_RETRY_SECONDS", DEFAULT_RETRY_SECONDS,
                                    3600U);
    stale_seconds = config_unsigned("AI_INDEX_STALE_SECONDS", DEFAULT_STALE_SECONDS,
                                    86400U);
    if (signal(SIGTERM, request_stop) == SIG_ERR ||
        signal(SIGINT, request_stop) == SIG_ERR ||
        ensure_private_directory(index_directory) != 0) {
        fprintf(stderr, "faiss index worker: invalid runtime setup\n");
        return 2;
    }

    while (!stop_requested) {
        RabbitMqContentConsumer consumer;

        memset(&consumer, 0, sizeof(consumer));
        if (requeue_stale_ai_user_index_tasks(stale_seconds) < 0)
            fprintf(stderr, "faiss index worker: stale lease recovery failed\n");
        if (rabbitmq_user_index_consumer_open(&consumer, rabbit_host,
                (int)rabbit_port, rabbit_user, rabbit_password) != 0) {
            fprintf(stderr, "faiss index worker: RabbitMQ connection failed\n");
            wait_seconds(retry_seconds);
            continue;
        }
        while (!stop_requested) {
            RabbitMqContentDelivery delivery;
            int receive_result = rabbitmq_content_consumer_receive(
                &consumer, &delivery, RECEIVE_TIMEOUT_MS);

            if (receive_result == 1) continue;
            if (receive_result != 0 || process_delivery(&consumer, &delivery,
                    index_directory, max_retries, retry_seconds,
                    stale_seconds) != 0) {
                fprintf(stderr, "faiss index worker: consumer connection will be reopened\n");
                break;
            }
        }
        rabbitmq_content_consumer_close(&consumer);
        if (!stop_requested) wait_seconds(retry_seconds);
    }
    puts("faiss index worker: stopped");
    return 0;
}
