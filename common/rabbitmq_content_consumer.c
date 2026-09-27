#include "rabbitmq_content_consumer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>

#define AI_EXCHANGE "ai.events"
#define AI_DEAD_LETTER_EXCHANGE "ai.events.dlx"
#define AI_CONTENT_QUEUE "ai.content"
#define AI_USER_INDEX_QUEUE "ai.user-index"

static int rpc_succeeded(amqp_rpc_reply_t reply)
{
    return reply.reply_type == AMQP_RESPONSE_NORMAL;
}

static int declare_topology(amqp_connection_state_t connection, int channel,
                            const char *queue_name, const char *routing_key)
{
    amqp_table_entry_t dead_letter_entry;
    amqp_table_t queue_arguments;

    amqp_exchange_declare(connection, channel, amqp_cstring_bytes(AI_EXCHANGE),
                          amqp_cstring_bytes("topic"), 0, 1, 0, 0,
                          amqp_empty_table);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection))) return -1;
    amqp_exchange_declare(connection, channel,
                          amqp_cstring_bytes(AI_DEAD_LETTER_EXCHANGE),
                          amqp_cstring_bytes("topic"), 0, 1, 0, 0,
                          amqp_empty_table);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection))) return -1;
    dead_letter_entry.key = amqp_cstring_bytes("x-dead-letter-exchange");
    dead_letter_entry.value.kind = AMQP_FIELD_KIND_UTF8;
    dead_letter_entry.value.value.bytes =
        amqp_cstring_bytes(AI_DEAD_LETTER_EXCHANGE);
    queue_arguments.num_entries = 1;
    queue_arguments.entries = &dead_letter_entry;
    amqp_queue_declare(connection, channel, amqp_cstring_bytes(queue_name),
                       0, 1, 0, 0, queue_arguments);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection))) return -1;
    amqp_queue_bind(connection, channel, amqp_cstring_bytes(queue_name),
                    amqp_cstring_bytes(AI_EXCHANGE),
                    amqp_cstring_bytes(routing_key), amqp_empty_table);
    return rpc_succeeded(amqp_get_rpc_reply(connection)) ? 0 : -1;
}

static int parse_event_id(amqp_bytes_t bytes, unsigned long long *event_id)
{
    unsigned long long value = 0;
    size_t index;
    const unsigned char *data = bytes.bytes;

    if (!event_id || !data || bytes.len == 0 || bytes.len > 20) return -1;
    for (index = 0; index < bytes.len; ++index) {
        unsigned int digit;
        if (data[index] < '0' || data[index] > '9') return -1;
        digit = (unsigned int)(data[index] - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL) return -1;
        value = value * 10ULL + digit;
    }
    if (value == 0) return -1;
    *event_id = value;
    return 0;
}

static int open_consumer(RabbitMqContentConsumer *consumer,
                         const char *host, int port,
                         const char *user, const char *password,
                         const char *queue_name, const char *routing_key)
{
    amqp_connection_state_t connection;
    amqp_socket_t *socket;
    int channel = 1;

    if (!consumer || !host || !user || !password || port < 1 || port > 65535)
        return -1;
    memset(consumer, 0, sizeof(*consumer));
    connection = amqp_new_connection();
    if (!connection) return -1;
    socket = amqp_tcp_socket_new(connection);
    if (!socket || amqp_socket_open(socket, host, port) != AMQP_STATUS_OK) goto fail;
    if (!rpc_succeeded(amqp_login(connection, "/", 0, 131072, 30,
                                  AMQP_SASL_METHOD_PLAIN, user, password))) goto fail;
    amqp_channel_open(connection, channel);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection)) ||
        declare_topology(connection, channel, queue_name, routing_key) != 0) goto fail;
    amqp_basic_qos(connection, channel, 0, 1, 0);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection))) goto fail;
    amqp_basic_consume(connection, channel, amqp_cstring_bytes(queue_name),
                       amqp_empty_bytes, 0, 0, 0, amqp_empty_table);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection))) goto fail;
    consumer->connection = connection;
    consumer->channel = channel;
    return 0;

fail:
    amqp_destroy_connection(connection);
    return -1;
}

int rabbitmq_content_consumer_open(RabbitMqContentConsumer *consumer,
                                   const char *host, int port,
                                   const char *user, const char *password)
{
    return open_consumer(consumer, host, port, user, password,
                         AI_CONTENT_QUEUE, "content.*");
}

int rabbitmq_user_index_consumer_open(RabbitMqContentConsumer *consumer,
                                      const char *host, int port,
                                      const char *user, const char *password)
{
    return open_consumer(consumer, host, port, user, password,
                         AI_USER_INDEX_QUEUE, "user.*");
}

int rabbitmq_content_consumer_receive(RabbitMqContentConsumer *consumer,
                                      RabbitMqContentDelivery *delivery,
                                      unsigned int timeout_ms)
{
    amqp_connection_state_t connection;
    amqp_envelope_t envelope;
    amqp_rpc_reply_t reply;
    struct timeval timeout;
    amqp_basic_properties_t *properties;

    if (!consumer || !consumer->connection || !delivery || timeout_ms == 0)
        return -1;
    connection = (amqp_connection_state_t)consumer->connection;
    timeout.tv_sec = (long)(timeout_ms / 1000U);
    timeout.tv_usec = (long)((timeout_ms % 1000U) * 1000U);
    amqp_maybe_release_buffers(connection);
    reply = amqp_consume_message(connection, &envelope, &timeout, 0);
    if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION &&
        reply.library_error == AMQP_STATUS_TIMEOUT) return 1;
    if (!rpc_succeeded(reply)) return -1;
    memset(delivery, 0, sizeof(*delivery));
    delivery->delivery_tag = envelope.delivery_tag;
    delivery->redelivered = envelope.redelivered ? 1 : 0;
    properties = &envelope.message.properties;
    if (!(properties->_flags & AMQP_BASIC_MESSAGE_ID_FLAG) ||
        parse_event_id(properties->message_id, &delivery->event_id) != 0 ||
        envelope.message.body.len >= sizeof(delivery->payload)) {
        amqp_destroy_envelope(&envelope);
        return 0;
    }
    memcpy(delivery->payload, envelope.message.body.bytes,
           envelope.message.body.len);
    delivery->payload[envelope.message.body.len] = '\0';
    amqp_destroy_envelope(&envelope);
    return 0;
}

int rabbitmq_content_consumer_ack(RabbitMqContentConsumer *consumer,
                                  uint64_t delivery_tag)
{
    if (!consumer || !consumer->connection || delivery_tag == 0) return -1;
    return amqp_basic_ack((amqp_connection_state_t)consumer->connection,
                          consumer->channel, delivery_tag, 0) == AMQP_STATUS_OK ? 0 : -1;
}

int rabbitmq_content_consumer_reject(RabbitMqContentConsumer *consumer,
                                     uint64_t delivery_tag, int requeue)
{
    if (!consumer || !consumer->connection || delivery_tag == 0) return -1;
    return amqp_basic_reject((amqp_connection_state_t)consumer->connection,
                             consumer->channel, delivery_tag,
                             requeue ? 1 : 0) == AMQP_STATUS_OK ? 0 : -1;
}

void rabbitmq_content_consumer_close(RabbitMqContentConsumer *consumer)
{
    amqp_connection_state_t connection;

    if (!consumer || !consumer->connection) return;
    connection = (amqp_connection_state_t)consumer->connection;
    amqp_channel_close(connection, consumer->channel, AMQP_REPLY_SUCCESS);
    amqp_connection_close(connection, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(connection);
    memset(consumer, 0, sizeof(*consumer));
}
