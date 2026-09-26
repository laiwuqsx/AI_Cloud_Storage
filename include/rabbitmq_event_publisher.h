#ifndef AI_CLOUD_RABBITMQ_EVENT_PUBLISHER_H
#define AI_CLOUD_RABBITMQ_EVENT_PUBLISHER_H

#include <stddef.h>

typedef struct {
    void *connection;
    int channel;
} RabbitMqEventPublisher;

int rabbitmq_event_publisher_open(RabbitMqEventPublisher *publisher,
                                  const char *host, int port,
                                  const char *user, const char *password);
int rabbitmq_event_publisher_publish(RabbitMqEventPublisher *publisher,
                                     unsigned long long event_id,
                                     const char *event_type,
                                     const char *payload);
int rabbitmq_event_publisher_dead_letter_count(
    RabbitMqEventPublisher *publisher, unsigned long long *message_count);
void rabbitmq_event_publisher_close(RabbitMqEventPublisher *publisher);

/* Exposed for small unit tests and for consumers to share the routing contract. */
const char *rabbitmq_event_routing_key(const char *event_type);

#endif
