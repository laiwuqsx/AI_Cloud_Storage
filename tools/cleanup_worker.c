#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cleanup_repository.h"
#include "fastdfs_storage_client.h"
#include "runtime_config.h"

#define DEFAULT_JOB_LIMIT 100UL
#define STALE_JOB_SECONDS 300U
#define DEFAULT_MAX_RETRIES 5U
#define DEFAULT_RETRY_BASE_SECONDS 30U
#define DEFAULT_POLL_INTERVAL_SECONDS 10U
#define MAX_RETRY_DELAY_SECONDS 3600U

typedef struct {
    unsigned long processed;
    unsigned long completed;
    int had_error;
} BatchResult;

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int parse_options(int argc, char **argv, int *watch, unsigned long *limit)
{
    char *end;
    unsigned long value;
    int index = 1;

    *watch = 0;
    *limit = DEFAULT_JOB_LIMIT;
    if (index < argc && strcmp(argv[index], "--watch") == 0) {
        *watch = 1;
        ++index;
    }
    if (index == argc) return 0;
    if (index + 1 != argc || argv[index][0] == '\0') return -1;
    errno = 0;
    value = strtoul(argv[index], &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > 10000) return -1;
    *limit = value;
    return 0;
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

static unsigned int retry_delay(unsigned int retry_count, unsigned int base_seconds)
{
    unsigned int delay = base_seconds;
    unsigned int exponent = retry_count;

    while (exponent > 0 && delay < MAX_RETRY_DELAY_SECONDS) {
        if (delay > MAX_RETRY_DELAY_SECONDS / 2U) return MAX_RETRY_DELAY_SECONDS;
        delay *= 2U;
        --exponent;
    }
    return delay > MAX_RETRY_DELAY_SECONDS ? MAX_RETRY_DELAY_SECONDS : delay;
}

static BatchResult process_batch(StorageClient *storage, unsigned long limit,
                                 unsigned int max_retries,
                                 unsigned int retry_base_seconds)
{
    BatchResult result = {0, 0, 0};
    StorageCleanupJob job;

    while (result.processed < limit && !stop_requested) {
        int claim_result = claim_next_storage_cleanup(&job);

        if (claim_result == 1) break;
        if (claim_result != 0) {
            fprintf(stderr, "cleanup worker: unable to claim a job\n");
            result.had_error = 1;
            break;
        }
        ++result.processed;
        if (storage_client_remove(storage, job.storage_key) != 0) {
            unsigned int attempted = job.retry_count + 1U;

            result.had_error = 1;
            if (attempted >= max_retries) {
                if (fail_storage_cleanup(job.id, "FastDFS delete retry limit reached") != 0) {
                    fprintf(stderr, "cleanup worker: job %llu could not enter failed state\n",
                            job.id);
                } else {
                    fprintf(stderr, "cleanup worker: job %llu failed permanently after %u attempt(s)\n",
                            job.id, attempted);
                }
            } else {
                unsigned int delay = retry_delay(job.retry_count, retry_base_seconds);

                if (retry_storage_cleanup_after(job.id, "FastDFS delete retry failed",
                                                delay) != 0) {
                    fprintf(stderr, "cleanup worker: job %llu failed and could not be requeued\n",
                            job.id);
                } else {
                    fprintf(stderr, "cleanup worker: job %llu retry scheduled in %u second(s)\n",
                            job.id, delay);
                }
            }
            continue;
        }
        if (complete_storage_cleanup(job.id) != 0) {
            fprintf(stderr, "cleanup worker: deleted %s but could not complete job %llu\n",
                    job.storage_key, job.id);
            result.had_error = 1;
            continue;
        }
        printf("cleanup worker: deleted %s (%s)\n", job.storage_key, job.reason);
        ++result.completed;
    }
    return result;
}

int main(int argc, char **argv)
{
    const char *client_config;
    const char *public_base_url;
    FastDfsStorageContext fastdfs_context;
    StorageClient storage;
    unsigned long limit;
    unsigned int max_retries;
    unsigned int retry_base_seconds;
    unsigned int poll_interval_seconds;
    int watch;

    if (parse_options(argc, argv, &watch, &limit) != 0) {
        fprintf(stderr, "usage: %s [--watch] [maximum-jobs-per-cycle]\n", argv[0]);
        return 2;
    }
    runtime_config_init();
    max_retries = config_unsigned("CLEANUP_MAX_RETRIES", DEFAULT_MAX_RETRIES, 100U);
    retry_base_seconds = config_unsigned("CLEANUP_RETRY_BASE_SECONDS",
                                         DEFAULT_RETRY_BASE_SECONDS,
                                         MAX_RETRY_DELAY_SECONDS);
    poll_interval_seconds = config_unsigned("CLEANUP_POLL_INTERVAL_SECONDS",
                                            DEFAULT_POLL_INTERVAL_SECONDS, 3600U);
    client_config = runtime_config_get("FASTDFS_CLIENT_CONFIG", NULL);
    public_base_url = runtime_config_get("FASTDFS_PUBLIC_BASE_URL", "http://unused");
    fastdfs_storage_context_init(&fastdfs_context, client_config, public_base_url);
    if (fastdfs_storage_client_init(&storage, &fastdfs_context) != 0) {
        fprintf(stderr, "cleanup worker: invalid FastDFS configuration\n");
        return 2;
    }
    if (watch) {
        if (signal(SIGTERM, request_stop) == SIG_ERR ||
            signal(SIGINT, request_stop) == SIG_ERR) {
            fprintf(stderr, "cleanup worker: unable to install signal handlers\n");
            return 2;
        }
        printf("cleanup worker: watching every %u second(s), up to %lu job(s) per cycle\n",
               poll_interval_seconds, limit);
        fflush(stdout);
    }

    do {
        int stale_count = requeue_stale_storage_cleanups(STALE_JOB_SECONDS);
        BatchResult batch;

        if (stale_count < 0) {
            fprintf(stderr, "cleanup worker: unable to recover stale jobs\n");
            if (!watch) return 1;
            batch.processed = 0;
            batch.completed = 0;
            batch.had_error = 1;
        } else {
            batch = process_batch(&storage, limit, max_retries, retry_base_seconds);
            if (batch.processed > 0 || stale_count > 0 || !watch) {
                printf("cleanup worker: processed %lu job(s), completed %lu, recovered %d stale job(s)\n",
                       batch.processed, batch.completed, stale_count);
                fflush(stdout);
            }
        }
        if (!watch) return batch.had_error ? 1 : 0;
        if (!stop_requested && batch.processed < limit) sleep(poll_interval_seconds);
    } while (!stop_requested);

    puts("cleanup worker: stopped");
    return 0;
}
