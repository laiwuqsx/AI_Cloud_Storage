#include "share_access_service.h"

#include <hiredis/hiredis.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_repository.h"
#include "runtime_config.h"
#include "share_code.h"

#define ACTOR_DIGEST_LENGTH 16

static int positive_config(const char *name, const char *fallback)
{
    const char *text = runtime_config_get(name, fallback);
    char *end = NULL;
    long value;

    if (!text || *text == '\0') return -1;
    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value <= 0 || value > 86400) return -1;
    return (int)value;
}

static int make_failure_key(const char *share_id, const char *actor,
                            char *key, size_t key_size)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    char actor_hex[ACTOR_DIGEST_LENGTH * 2 + 1];
    size_t index;

    if (!share_id || !actor || !key || actor[0] == '\0' ||
        EVP_Digest(actor, strlen(actor), digest, &digest_length,
                   EVP_sha256(), NULL) != 1 || digest_length < ACTOR_DIGEST_LENGTH)
        return -1;
    for (index = 0; index < ACTOR_DIGEST_LENGTH; ++index) {
        actor_hex[index * 2] = hex[digest[index] >> 4];
        actor_hex[index * 2 + 1] = hex[digest[index] & 0x0f];
    }
    actor_hex[sizeof(actor_hex) - 1] = '\0';
    return snprintf(key, key_size, "share_code_fail:%s:%s", share_id, actor_hex) <
                   (int)key_size ? 0 : -1;
}

static redisContext *connect_redis(void)
{
    struct timeval timeout = {1, 500000};
    const char *host = runtime_config_get("REDIS_HOST", "127.0.0.1");
    int port = atoi(runtime_config_get("REDIS_PORT", "6379"));
    redisContext *redis;

    if (port <= 0 || port > 65535) return NULL;
    redis = redisConnectWithTimeout(host, port, timeout);
    if (!redis || redis->err) {
        if (redis) redisFree(redis);
        return NULL;
    }
    return redis;
}

static int current_failures(redisContext *redis, const char *key, int *count)
{
    redisReply *reply;
    char *end = NULL;
    long value = 0;

    if (!redis || !key || !count) return -1;
    reply = redisCommand(redis, "GET %s", key);
    if (!reply) return -1;
    if (reply->type == REDIS_REPLY_STRING) {
        value = strtol(reply->str, &end, 10);
        if (!end || *end != '\0' || value < 0 || value > INT_MAX) {
            freeReplyObject(reply);
            return -1;
        }
    } else if (reply->type != REDIS_REPLY_NIL) {
        freeReplyObject(reply);
        return -1;
    }
    *count = (int)value;
    freeReplyObject(reply);
    return 0;
}

static int record_failure(redisContext *redis, const char *key, int window,
                          int *count)
{
    static const char script[] =
        "local n=redis.call('INCR',KEYS[1]); "
        "if n==1 then redis.call('EXPIRE',KEYS[1],ARGV[1]); end; return n;";
    const char *arguments[6];
    size_t lengths[6];
    char window_text[16];
    redisReply *reply;

    snprintf(window_text, sizeof(window_text), "%d", window);
    arguments[0] = "EVAL";
    arguments[1] = script;
    arguments[2] = "1";
    arguments[3] = key;
    arguments[4] = window_text;
    arguments[5] = NULL;
    lengths[0] = 4;
    lengths[1] = strlen(script);
    lengths[2] = 1;
    lengths[3] = strlen(key);
    lengths[4] = strlen(window_text);
    reply = redisCommandArgv(redis, 5, arguments, lengths);
    if (!reply) return -1;
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer < 1) {
        freeReplyObject(reply);
        return -1;
    }
    *count = (int)reply->integer;
    freeReplyObject(reply);
    return 0;
}

ShareAccessResult authorize_share_access(const char *share_id,
                                         const char *submitted_code,
                                         const char *actor)
{
    ShareAccessPolicy policy;
    redisContext *redis = NULL;
    redisReply *reply = NULL;
    char key[160];
    int lookup, failures, max_failures, window;
    ShareAccessResult result = SHARE_ACCESS_ERROR;

    lookup = find_share_access_policy(share_id, &policy);
    if (lookup == 1) return SHARE_ACCESS_UNAVAILABLE;
    if (lookup != 0) return SHARE_ACCESS_ERROR;
    if (!policy.requires_code) return SHARE_ACCESS_GRANTED;
    if (!submitted_code || submitted_code[0] == '\0') return SHARE_ACCESS_CODE_REQUIRED;
    max_failures = positive_config("SHARE_CODE_MAX_FAILURES", "5");
    window = positive_config("SHARE_CODE_FAILURE_WINDOW_SECONDS", "300");
    if (max_failures < 1 || window < 1 || make_failure_key(share_id, actor, key, sizeof(key)) != 0)
        return SHARE_ACCESS_ERROR;
    redis = connect_redis();
    if (!redis || current_failures(redis, key, &failures) != 0) goto done;
    if (failures >= max_failures) {
        result = SHARE_ACCESS_RATE_LIMITED;
        goto done;
    }
    if (verify_share_code_digest(submitted_code, policy.salt, policy.hash)) {
        reply = redisCommand(redis, "DEL %s", key);
        if (!reply || reply->type != REDIS_REPLY_INTEGER) goto done;
        result = SHARE_ACCESS_GRANTED;
        goto done;
    }
    if (record_failure(redis, key, window, &failures) != 0) goto done;
    result = failures >= max_failures ? SHARE_ACCESS_RATE_LIMITED
                                      : SHARE_ACCESS_CODE_INVALID;

done:
    if (reply) freeReplyObject(reply);
    if (redis) redisFree(redis);
    return result;
}
