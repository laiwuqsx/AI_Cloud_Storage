#include <stdio.h>

#include "cleanup_repository.h"
#include "runtime_config.h"

static void write_job_metric(const char *status, unsigned long long value)
{
    printf("ai_cloud_storage_cleanup_jobs{status=\"%s\"} %llu\n", status, value);
}

int main(void)
{
    StorageCleanupMetrics metrics;

    runtime_config_init();
    if (get_storage_cleanup_metrics(&metrics) != 0) {
        fprintf(stderr, "cleanup metrics: unable to read storage cleanup metrics\n");
        return 1;
    }

    puts("# HELP ai_cloud_storage_cleanup_jobs Number of cleanup jobs by state.");
    puts("# TYPE ai_cloud_storage_cleanup_jobs gauge");
    write_job_metric("pending", metrics.pending_count);
    write_job_metric("running", metrics.running_count);
    write_job_metric("done", metrics.done_count);
    write_job_metric("failed", metrics.failed_count);
    puts("# HELP ai_cloud_storage_cleanup_ready_jobs Pending cleanup jobs ready to run now.");
    puts("# TYPE ai_cloud_storage_cleanup_ready_jobs gauge");
    printf("ai_cloud_storage_cleanup_ready_jobs %llu\n", metrics.ready_count);
    puts("# HELP ai_cloud_storage_cleanup_oldest_pending_age_seconds "
         "Age of the oldest pending cleanup job.");
    puts("# TYPE ai_cloud_storage_cleanup_oldest_pending_age_seconds gauge");
    printf("ai_cloud_storage_cleanup_oldest_pending_age_seconds %llu\n",
           metrics.oldest_pending_age_seconds);
    puts("# HELP ai_cloud_storage_cleanup_oldest_ready_age_seconds "
         "How long the oldest runnable cleanup job has been overdue.");
    puts("# TYPE ai_cloud_storage_cleanup_oldest_ready_age_seconds gauge");
    printf("ai_cloud_storage_cleanup_oldest_ready_age_seconds %llu\n",
           metrics.oldest_ready_age_seconds);
    return 0;
}
