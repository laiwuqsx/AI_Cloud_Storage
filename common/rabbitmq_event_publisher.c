#include "rabbitmq_event_publisher.h"

#include <stdio.h>
#include <string.h>

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>

#define AI_EXCHANGE "ai.events"
#define AI_DEAD_LETTER_EXCHANGE "ai.events.dlx"
#define AI_CONTENT_QUEUE "ai.content"
#define AI_USER_QUEUE "ai.user-index"
#define AI_DEAD_LETTER_QUEUE "ai.dead-letter"

static int rpc_succeeded(amqp_rpc_reply_t reply)
{
    return reply.reply_type == AMQP_RESPONSE_NORMAL;
}

static int declare_queue(amqp_connection_state_t connection, int channel,
                         const char *name, amqp_table_t arguments)
{
    amqp_queue_declare(connection, channel, amqp_cstring_bytes(name), 0, 1, 0, 0,
                       arguments);
    return rpc_succeeded(amqp_get_rpc_reply(connection)) ? 0 : -1;
}

static int bind_queue(amqp_connection_state_t connection, int channel,
                      const char *queue, const char *exchange,
                      const char *routing_key)
{
    amqp_queue_bind(connection, channel, amqp_cstring_bytes(queue),
                    amqp_cstring_bytes(exchange), amqp_cstring_bytes(routing_key),
                    amqp_empty_table);
    return rpc_succeeded(amqp_get_rpc_reply(connection)) ? 0 : -1;
}

static int declare_topology(amqp_connection_state_t connection, int channel)
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
    if (declare_queue(connection, channel, AI_CONTENT_QUEUE, queue_arguments) != 0 ||
        declare_queue(connection, channel, AI_USER_QUEUE, queue_arguments) != 0 ||
        declare_queue(connection, channel, AI_DEAD_LETTER_QUEUE,
                      amqp_empty_table) != 0) return -1;
    if (bind_queue(connection, channel, AI_CONTENT_QUEUE, AI_EXCHANGE,
                   "content.*") != 0 ||
        bind_queue(connection, channel, AI_USER_QUEUE, AI_EXCHANGE,
                   "user.*") != 0 ||
        bind_queue(connection, channel, AI_DEAD_LETTER_QUEUE,
                   AI_DEAD_LETTER_EXCHANGE, "#") != 0) return -1;
    return 0;
}

const char *rabbitmq_event_routing_key(const char *event_type)
{
    if (!event_type) return NULL;
    if (strcmp(event_type, "FILE_CONTENT_READY") == 0) return "content.ready";
    if (strcmp(event_type, "USER_FILE_ADDED") == 0) return "user.added";
    if (strcmp(event_type, "USER_FILE_REMOVED") == 0) return "user.removed";
    return NULL;
}

int rabbitmq_event_publisher_open(RabbitMqEventPublisher *publisher,
                                  const char *host, int port,
                                  const char *user, const char *password)
{
    amqp_connection_state_t connection;
    amqp_socket_t *socket;
    int channel = 1;

    if (!publisher || !host || !user || !password || port < 1 || port > 65535)
        return -1;
    memset(publisher, 0, sizeof(*publisher));
    connection = amqp_new_connection();
    if (!connection) return -1;
    socket = amqp_tcp_socket_new(connection);
    if (!socket || amqp_socket_open(socket, host, port) != AMQP_STATUS_OK) goto fail;
    if (!rpc_succeeded(amqp_login(connection, "/", 0, 131072, 30,
                                  AMQP_SASL_METHOD_PLAIN, user, password))) goto fail;
    amqp_channel_open(connection, channel);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection))) goto fail;
    if (declare_topology(connection, channel) != 0) goto fail;
    amqp_confirm_select(connection, channel);
    if (!rpc_succeeded(amqp_get_rpc_reply(connection))) goto fail;
    publisher->connection = connection;
    publisher->channel = channel;
    return 0;

fail:
    amqp_destroy_connection(connection);
    return -1;
}

int rabbitmq_event_publisher_publish(RabbitMqEventPublisher *publisher,
                                     unsigned long long event_id,
                                     const char *event_type,
                                     const char *payload)
{
    amqp_connection_state_t connection;
    amqp_basic_properties_t properties;
    amqp_frame_t frame;
    const char *routing_key = rabbitmq_event_routing_key(event_type);
    char message_id[32];
    int message_id_length;

    if (!publisher || !publisher->connection || event_id == 0 || !routing_key ||
        !payload || payload[0] == '\0') return -1;
    message_id_length = snprintf(message_id, sizeof(message_id), "%llu", event_id);
    if (message_id_length < 1 || (size_t)message_id_length >= sizeof(message_id))
        return -1;
    connection = (amqp_connection_state_t)publisher->connection;
    memset(&properties, 0, sizeof(properties));
    properties._flags = AMQP_BASIC_CONTENT_TYPE_FLAG |
                        AMQP_BASIC_DELIVERY_MODE_FLAG |
                        AMQP_BASIC_MESSAGE_ID_FLAG |
                        AMQP_BASIC_TYPE_FLAG;
    properties.content_type = amqp_cstring_bytes("application/json");
    properties.delivery_mode = 2;
    properties.message_id = amqp_cstring_bytes(message_id);
    properties.type = amqp_cstring_bytes(event_type);
    if (amqp_basic_publish(connection, publisher->channel,
                           amqp_cstring_bytes(AI_EXCHANGE),
                           amqp_cstring_bytes(routing_key), 0, 0,
                           &properties, amqp_cstring_bytes(payload)) != AMQP_STATUS_OK)
        return -1;
    for (;;) {
        if (amqp_simple_wait_frame(connection, &frame) != AMQP_STATUS_OK) return -1;
        if (frame.frame_type != AMQP_FRAME_METHOD) continue;
        if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD) return 0;
        if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD ||
            frame.payload.method.id == AMQP_CHANNEL_CLOSE_METHOD ||
            frame.payload.method.id == AMQP_CONNECTION_CLOSE_METHOD) return -1;
    }
}

int rabbitmq_event_publisher_dead_letter_count(
    RabbitMqEventPublisher *publisher, unsigned long long *message_count)
{
    amqp_connection_state_t connection;
    amqp_queue_declare_ok_t *reply;

    if (!publisher || !publisher->connection || !message_count) return -1;
    connection = (amqp_connection_state_t)publisher->connection;
    reply = amqp_queue_declare(connection, publisher->channel,
                               amqp_cstring_bytes(AI_DEAD_LETTER_QUEUE),
                               1, 1, 0, 0, amqp_empty_table);
    if (!reply || !rpc_succeeded(amqp_get_rpc_reply(connection))) return -1;
    *message_count = (unsigned long long)reply->message_count;
    return 0;
}

void rabbitmq_event_publisher_close(RabbitMqEventPublisher *publisher)
{
    amqp_connection_state_t connection;

    if (!publisher || !publisher->connection) return;
    connection = (amqp_connection_state_t)publisher->connection;
    amqp_channel_close(connection, publisher->channel, AMQP_REPLY_SUCCESS);
    amqp_connection_close(connection, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(connection);
    memset(publisher, 0, sizeof(*publisher));
}
