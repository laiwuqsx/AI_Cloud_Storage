#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "outbox_repository.h"
#include "rabbitmq_event_publisher.h"
#include "runtime_config.h"

#define DEFAULT_BATCH_LIMIT 100UL
#define DEFAULT_MAX_RETRIES 8U
#define DEFAULT_RETRY_BASE_SECONDS 5U
#define DEFAULT_POLL_INTERVAL_SECONDS 2U
#define DEFAULT_STALE_SECONDS 300U
#define MAX_RETRY_DELAY_SECONDS 3600U

typedef struct {
    unsigned long processed;
    unsigned long published;
    int connection_failed;
    int had_error;
} BatchResult;

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

static int parse_options(int argc, char **argv, int *watch, unsigned long *limit)
{
    char *end;
    unsigned long value;
    int index = 1;

    *watch = 0;
    *limit = DEFAULT_BATCH_LIMIT;
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

static int handle_publish_failure(const OutboxPublishEvent *event,
                                  unsigned int max_retries,
                                  unsigned int retry_base_seconds)
{
    unsigned int attempted = event->retry_count + 1U;

    if (attempted >= max_retries) {
        return fail_outbox_event(event->id, "RabbitMQ publish or confirm failed");
    }
    return retry_outbox_event_after(event->id,
                                    "RabbitMQ publish or confirm failed",
                                    retry_delay(event->retry_count,
                                                retry_base_seconds));
}

static BatchResult process_batch(RabbitMqEventPublisher *publisher,
                                 unsigned long limit, unsigned int max_retries,
                                 unsigned int retry_base_seconds)
{
    BatchResult result = {0, 0, 0, 0};

    while (result.processed < limit && !stop_requested) {
        OutboxPublishEvent event;
        int claim_result = claim_next_outbox_event(&event);

        if (claim_result == 1) break;
        if (claim_result != 0) {
            fprintf(stderr, "outbox publisher: unable to claim an event\n");
            result.had_error = 1;
            break;
        }
        ++result.processed;
        if (rabbitmq_event_publisher_publish(publisher, event.id,
                                             event.event_type,
                                             event.payload) != 0) {
            fprintf(stderr, "outbox publisher: event %llu publish failed\n", event.id);
            if (handle_publish_failure(&event, max_retries,
                                       retry_base_seconds) != 0) {
                fprintf(stderr, "outbox publisher: event %llu could not be requeued\n",
                        event.id);
            }
            result.connection_failed = 1;
            result.had_error = 1;
            break;
        }
        if (mark_outbox_event_published(event.id) != 0) {
            fprintf(stderr, "outbox publisher: event %llu confirmed but database update failed\n",
                    event.id);
            result.had_error = 1;
            continue;
        }
        printf("outbox publisher: published event %llu (%s)\n",
               event.id, event.event_type);
        ++result.published;
    }
    return result;
}

int main(int argc, char **argv)
{
    const char *host, *user, *password;
    RabbitMqEventPublisher publisher;
    unsigned int port, max_retries, retry_base_seconds, poll_interval_seconds;
    unsigned int stale_seconds;
    unsigned long limit;
    int connected = 0;
    int watch;

    if (parse_options(argc, argv, &watch, &limit) != 0) {
        fprintf(stderr, "usage: %s [--watch] [maximum-events-per-cycle]\n", argv[0]);
        return 2;
    }
    runtime_config_init();
    host = runtime_config_get("RABBITMQ_HOST", "127.0.0.1");
    user = runtime_config_get("RABBITMQ_USER", "guest");
    password = runtime_config_get("RABBITMQ_PASSWORD", "guest");
    port = config_unsigned("RABBITMQ_PORT", 5672U, 65535U);
    max_retries = config_unsigned("OUTBOX_MAX_RETRIES", DEFAULT_MAX_RETRIES, 100U);
    retry_base_seconds = config_unsigned("OUTBOX_RETRY_BASE_SECONDS",
                                         DEFAULT_RETRY_BASE_SECONDS,
                                         MAX_RETRY_DELAY_SECONDS);
    poll_interval_seconds = config_unsigned("OUTBOX_POLL_INTERVAL_SECONDS",
                                            DEFAULT_POLL_INTERVAL_SECONDS, 3600U);
    stale_seconds = config_unsigned("OUTBOX_STALE_SECONDS", DEFAULT_STALE_SECONDS,
                                    86400U);
    if (watch && (signal(SIGTERM, request_stop) == SIG_ERR ||
                  signal(SIGINT, request_stop) == SIG_ERR)) {
        fprintf(stderr, "outbox publisher: unable to install signal handlers\n");
        return 2;
    }

    memset(&publisher, 0, sizeof(publisher));
    do {
        BatchResult batch = {0, 0, 0, 1};
        int recovered = requeue_stale_outbox_events(stale_seconds);

        if (recovered < 0) {
            fprintf(stderr, "outbox publisher: unable to recover stale events\n");
        } else if (!connected &&
                   rabbitmq_event_publisher_open(&publisher, host, (int)port,
                                                 user, password) != 0) {
            fprintf(stderr, "outbox publisher: unable to connect to RabbitMQ\n");
        } else {
            connected = 1;
            batch = process_batch(&publisher, limit, max_retries,
                                  retry_base_seconds);
            if (batch.connection_failed) {
                rabbitmq_event_publisher_close(&publisher);
                connected = 0;
            }
            if (batch.processed > 0 || recovered > 0 || !watch) {
                printf("outbox publisher: processed %lu, published %lu, recovered %d\n",
                       batch.processed, batch.published, recovered);
                fflush(stdout);
            }
        }
        if (!watch) {
            if (connected) rabbitmq_event_publisher_close(&publisher);
            return batch.had_error ? 1 : 0;
        }
        if (!stop_requested && (batch.processed < limit || batch.connection_failed))
            sleep(poll_interval_seconds);
    } while (!stop_requested);

    if (connected) rabbitmq_event_publisher_close(&publisher);
    puts("outbox publisher: stopped");
    return 0;
}
