#include "token_service.h"

#include <fcntl.h>
#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime_config.h"

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
    const char *host = runtime_config_get("REDIS_HOST", NULL);
    const char *port_text = runtime_config_get("REDIS_PORT", NULL);
    int port = port_text ? atoi(port_text) : 6379;
    int ttl = atoi(runtime_config_get("TOKEN_TTL_SECONDS", "86400"));
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

int verify_session_token(const char *user, const char *token)
{
    struct timeval timeout = {1, 500000};
    redisContext *redis = NULL;
    redisReply *reply = NULL;
    const char *host = runtime_config_get("REDIS_HOST", NULL);
    const char *port_text = runtime_config_get("REDIS_PORT", NULL);
    int port = port_text ? atoi(port_text) : 6379;
    int result = -1;

    if (!user || !token || token[0] == '\0') return -1;
    redis = redisConnectWithTimeout(host ? host : "127.0.0.1", port, timeout);
    if (!redis || redis->err) goto done;
    reply = redisCommand(redis, "GET token:%s", token);
    if (reply && reply->type == REDIS_REPLY_STRING &&
        strcmp(reply->str, user) == 0) result = 0;

done:
    if (reply) freeReplyObject(reply);
    if (redis) redisFree(redis);
    return result;
}

int revoke_session_token(const char *user, const char *token)
{
    static const char script[] =
        "local owner = redis.call('GET', KEYS[1]); "
        "if not owner then return 0; end; "
        "if owner ~= ARGV[1] then return -1; end; "
        "return redis.call('DEL', KEYS[1]);";
    struct timeval timeout = {1, 500000};
    redisContext *redis = NULL;
    redisReply *reply = NULL;
    const char *host = runtime_config_get("REDIS_HOST", NULL);
    const char *port_text = runtime_config_get("REDIS_PORT", NULL);
    const char *arguments[5];
    char key[72];
    int port = port_text ? atoi(port_text) : 6379;
    int result = -1;

    if (!user || !token || token[0] == '\0' ||
        snprintf(key, sizeof(key), "token:%s", token) >= (int)sizeof(key)) return -1;
    redis = redisConnectWithTimeout(host ? host : "127.0.0.1", port, timeout);
    if (!redis || redis->err) goto done;

    arguments[0] = "EVAL";
    arguments[1] = script;
    arguments[2] = "1";
    arguments[3] = key;
    arguments[4] = user;
    reply = redisCommandArgv(redis, 5, arguments, NULL);
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        if (reply->integer == 0 || reply->integer == 1) result = 0;
        else if (reply->integer == -1) result = 1;
    }

done:
    if (reply) freeReplyObject(reply);
    if (redis) redisFree(redis);
    return result;
}
