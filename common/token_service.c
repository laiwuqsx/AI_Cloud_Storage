#include "token_service.h"

#include <fcntl.h>
#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int random_hex_token(char *output, size_t output_size)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[32];
    int fd;
    size_t i;

    if (!output || output_size < sizeof(bytes) * 2 + 1) return -1;
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    close(fd);
    for (i = 0; i < sizeof(bytes); ++i) {
        output[i * 2] = hex[bytes[i] >> 4];
        output[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    output[64] = '\0';
    return 0;
}

int create_session_token(const char *user, char *token, size_t token_size)
{
    struct timeval timeout = {1, 500000};
    redisContext *redis = NULL;
    redisReply *reply = NULL;
    const char *host = getenv("REDIS_HOST");
    const char *port_text = getenv("REDIS_PORT");
    int port = port_text ? atoi(port_text) : 6379;
    int ttl = getenv("TOKEN_TTL_SECONDS") ? atoi(getenv("TOKEN_TTL_SECONDS")) : 86400;
    int result = -1;

    if (!user || random_hex_token(token, token_size) != 0 || ttl <= 0) return -1;
    redis = redisConnectWithTimeout(host ? host : "127.0.0.1", port, timeout);
    if (!redis || redis->err) goto done;
    reply = redisCommand(redis, "SETEX token:%s %d %s", token, ttl, user);
    if (reply && reply->type == REDIS_REPLY_STATUS) result = 0;

done:
    if (reply) freeReplyObject(reply);
    if (redis) redisFree(redis);
    return result;
}
