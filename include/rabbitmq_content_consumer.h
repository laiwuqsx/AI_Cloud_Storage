#ifndef AI_CLOUD_RABBITMQ_CONTENT_CONSUMER_H
#define AI_CLOUD_RABBITMQ_CONTENT_CONSUMER_H

#include <stdint.h>

#define RABBITMQ_CONTENT_PAYLOAD_CAPACITY 4097

typedef struct {
    void *connection;
    int channel;
} RabbitMqContentConsumer;

typedef struct {
    uint64_t delivery_tag;
    unsigned long long event_id;
    int redelivered;
    char payload[RABBITMQ_CONTENT_PAYLOAD_CAPACITY];
} RabbitMqContentDelivery;

int rabbitmq_content_consumer_open(RabbitMqContentConsumer *consumer,
                                   const char *host, int port,
                                   const char *user, const char *password);
/* Returns 0 for a delivery, 1 for a timeout, and -1 for a connection error. */
int rabbitmq_content_consumer_receive(RabbitMqContentConsumer *consumer,
                                      RabbitMqContentDelivery *delivery,
                                      unsigned int timeout_ms);
int rabbitmq_content_consumer_ack(RabbitMqContentConsumer *consumer,
                                  uint64_t delivery_tag);
int rabbitmq_content_consumer_reject(RabbitMqContentConsumer *consumer,
                                     uint64_t delivery_tag, int requeue);
void rabbitmq_content_consumer_close(RabbitMqContentConsumer *consumer);

#endif
