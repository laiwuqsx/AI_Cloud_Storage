#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "cleanup_repository.h"
#include "fastdfs_storage_client.h"
#include "runtime_config.h"

#define DEFAULT_JOB_LIMIT 100UL
#define STALE_JOB_SECONDS 300U
#define DEFAULT_MAX_RETRIES 5U
#define DEFAULT_RETRY_BASE_SECONDS 30U
#define MAX_RETRY_DELAY_SECONDS 3600U

static int parse_limit(int argc, char **argv, unsigned long *limit)
{
    char *end;
    unsigned long value;

    if (argc == 1) {
        *limit = DEFAULT_JOB_LIMIT;
        return 0;
    }
    if (argc != 2 || argv[1][0] == '\0') return -1;
    errno = 0;
    value = strtoul(argv[1], &end, 10);
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

int main(int argc, char **argv)
{
    const char *client_config;
    const char *public_base_url;
    FastDfsStorageContext fastdfs_context;
    StorageClient storage;
    StorageCleanupJob job;
    unsigned long limit;
    unsigned long completed = 0;
    unsigned int max_retries;
    unsigned int retry_base_seconds;
    int claim_result;
    int stale_count;

    if (parse_limit(argc, argv, &limit) != 0) {
        fprintf(stderr, "usage: %s [maximum-jobs]\n", argv[0]);
        return 2;
    }
    runtime_config_init();
    max_retries = config_unsigned("CLEANUP_MAX_RETRIES", DEFAULT_MAX_RETRIES, 100U);
    retry_base_seconds = config_unsigned("CLEANUP_RETRY_BASE_SECONDS",
                                         DEFAULT_RETRY_BASE_SECONDS,
                                         MAX_RETRY_DELAY_SECONDS);
    client_config = runtime_config_get("FASTDFS_CLIENT_CONFIG", NULL);
    public_base_url = runtime_config_get("FASTDFS_PUBLIC_BASE_URL", "http://unused");
    fastdfs_storage_context_init(&fastdfs_context, client_config, public_base_url);
    if (fastdfs_storage_client_init(&storage, &fastdfs_context) != 0) {
        fprintf(stderr, "cleanup worker: invalid FastDFS configuration\n");
        return 2;
    }
    stale_count = requeue_stale_storage_cleanups(STALE_JOB_SECONDS);
    if (stale_count < 0) {
        fprintf(stderr, "cleanup worker: unable to recover stale jobs\n");
        return 1;
    }

    while (completed < limit) {
        claim_result = claim_next_storage_cleanup(&job);
        if (claim_result == 1) break;
        if (claim_result != 0) {
            fprintf(stderr, "cleanup worker: unable to claim a job\n");
            return 1;
        }
        if (storage_client_remove(&storage, job.storage_key) != 0) {
            unsigned int attempted = job.retry_count + 1U;

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
            return 1;
        }
        if (complete_storage_cleanup(job.id) != 0) {
            fprintf(stderr, "cleanup worker: deleted %s but could not complete job %llu\n",
                    job.storage_key, job.id);
            return 1;
        }
        printf("cleanup worker: deleted %s (%s)\n", job.storage_key, job.reason);
        ++completed;
    }
    printf("cleanup worker: completed %lu job(s), recovered %d stale job(s)\n",
           completed, stale_count);
    return 0;
}
