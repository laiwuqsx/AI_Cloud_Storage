#ifndef AI_CLOUD_OUTBOX_REPOSITORY_H
#define AI_CLOUD_OUTBOX_REPOSITORY_H

#include <mysql/mysql.h>

#include "ai_index_event.h"

#define OUTBOX_EVENT_TYPE_CAPACITY 65
#define OUTBOX_EVENT_PAYLOAD_CAPACITY 2049

typedef struct {
    unsigned long long id;
    char event_type[OUTBOX_EVENT_TYPE_CAPACITY];
    char payload[OUTBOX_EVENT_PAYLOAD_CAPACITY];
    unsigned int retry_count;
} OutboxPublishEvent;

typedef struct {
    unsigned long long pending_count;
    unsigned long long ready_count;
    unsigned long long publishing_count;
    unsigned long long published_count;
    unsigned long long failed_count;
    unsigned long long oldest_pending_age_seconds;
    unsigned long long oldest_ready_age_seconds;
} OutboxMetrics;

/* Writes AI state and its durable event on a caller-owned MySQL transaction. */
int enqueue_ai_index_event_in_transaction(MYSQL *connection,
                                          const AiIndexEvent *event);

/* 0: claimed, 1: no due event, -1: database failure. */
int claim_next_outbox_event(OutboxPublishEvent *event);

/* 0: state changed, 1: event is no longer publishing, -1: database failure. */
int mark_outbox_event_published(unsigned long long event_id);
int retry_outbox_event_after(unsigned long long event_id, const char *last_error,
                             unsigned int retry_after_seconds);
int fail_outbox_event(unsigned long long event_id, const char *last_error);

/* Returns the number recovered, or -1 on database failure. */
int requeue_stale_outbox_events(unsigned int stale_after_seconds);

/* Returns 0 on success and -1 when metrics cannot be read. */
int get_outbox_metrics(OutboxMetrics *metrics);

#endif
