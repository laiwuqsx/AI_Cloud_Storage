#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "outbox_repository.h"
#include "rabbitmq_event_publisher.h"
#include "runtime_config.h"

static unsigned int rabbitmq_port(void)
{
    const char *text = runtime_config_get("RABBITMQ_PORT", "5672");
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > 65535) return 5672U;
    return (unsigned int)value;
}

static void write_event_metric(const char *status, unsigned long long value)
{
    printf("ai_cloud_storage_outbox_events{status=\"%s\"} %llu\n", status, value);
}

int main(void)
{
    RabbitMqEventPublisher publisher;
    OutboxMetrics metrics;
    unsigned long long dead_letter_count;
    int result = 1;

    runtime_config_init();
    if (get_outbox_metrics(&metrics) != 0) {
        fprintf(stderr, "outbox metrics: unable to read MySQL metrics\n");
        return 1;
    }
    if (rabbitmq_event_publisher_open(
            &publisher,
            runtime_config_get("RABBITMQ_HOST", "127.0.0.1"),
            (int)rabbitmq_port(),
            runtime_config_get("RABBITMQ_USER", "guest"),
            runtime_config_get("RABBITMQ_PASSWORD", "guest")) != 0) {
        fprintf(stderr, "outbox metrics: unable to connect to RabbitMQ\n");
        return 1;
    }
    if (rabbitmq_event_publisher_dead_letter_count(
            &publisher, &dead_letter_count) != 0) {
        fprintf(stderr, "outbox metrics: unable to read dead-letter queue\n");
        goto done;
    }

    puts("# HELP ai_cloud_storage_outbox_events Number of Outbox events by state.");
    puts("# TYPE ai_cloud_storage_outbox_events gauge");
    write_event_metric("pending", metrics.pending_count);
    write_event_metric("publishing", metrics.publishing_count);
    write_event_metric("published", metrics.published_count);
    write_event_metric("failed", metrics.failed_count);
    puts("# HELP ai_cloud_storage_outbox_ready_events Pending events ready to publish now.");
    puts("# TYPE ai_cloud_storage_outbox_ready_events gauge");
    printf("ai_cloud_storage_outbox_ready_events %llu\n", metrics.ready_count);
    puts("# HELP ai_cloud_storage_outbox_oldest_pending_age_seconds "
         "Age of the oldest pending Outbox event.");
    puts("# TYPE ai_cloud_storage_outbox_oldest_pending_age_seconds gauge");
    printf("ai_cloud_storage_outbox_oldest_pending_age_seconds %llu\n",
           metrics.oldest_pending_age_seconds);
    puts("# HELP ai_cloud_storage_outbox_oldest_ready_age_seconds "
         "How long the oldest publishable event has been overdue.");
    puts("# TYPE ai_cloud_storage_outbox_oldest_ready_age_seconds gauge");
    printf("ai_cloud_storage_outbox_oldest_ready_age_seconds %llu\n",
           metrics.oldest_ready_age_seconds);
    puts("# HELP ai_cloud_storage_ai_dead_letter_messages "
         "Messages currently waiting in the AI dead-letter queue.");
    puts("# TYPE ai_cloud_storage_ai_dead_letter_messages gauge");
    printf("ai_cloud_storage_ai_dead_letter_messages %llu\n", dead_letter_count);
    result = 0;

done:
    rabbitmq_event_publisher_close(&publisher);
    return result;
}
