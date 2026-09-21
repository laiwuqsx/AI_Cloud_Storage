#ifndef AI_CLOUD_CLEANUP_REPOSITORY_H
#define AI_CLOUD_CLEANUP_REPOSITORY_H

#include <mysql/mysql.h>

#define CLEANUP_STORAGE_KEY_CAPACITY 257
#define CLEANUP_REASON_CAPACITY 65

typedef struct {
    unsigned long long id;
    char storage_key[CLEANUP_STORAGE_KEY_CAPACITY];
    char reason[CLEANUP_REASON_CAPACITY];
    unsigned int retry_count;
} StorageCleanupJob;

/* Idempotently creates or refreshes a pending cleanup for one storage key. */
int enqueue_storage_cleanup(const char *storage_key, const char *reason,
                            const char *last_error);

/* Enqueues on a caller-owned MySQL connection without committing its transaction. */
int enqueue_storage_cleanup_in_transaction(MYSQL *connection,
                                           const char *storage_key,
                                           const char *reason,
                                           const char *last_error);

/* 0: claimed, 1: no pending job, -1: database failure. */
int claim_next_storage_cleanup(StorageCleanupJob *job);

/* 0: state changed, 1: job was no longer running, -1: database failure. */
int complete_storage_cleanup(unsigned long long job_id);
int retry_storage_cleanup(unsigned long long job_id, const char *last_error);
int retry_storage_cleanup_after(unsigned long long job_id, const char *last_error,
                                unsigned int retry_after_seconds);
int fail_storage_cleanup(unsigned long long job_id, const char *last_error);

/* Returns the number requeued, or -1 on database failure. */
int requeue_stale_storage_cleanups(unsigned int stale_after_seconds);

#endif
