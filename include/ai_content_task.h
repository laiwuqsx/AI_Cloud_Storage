#ifndef AI_CLOUD_AI_CONTENT_TASK_H
#define AI_CLOUD_AI_CONTENT_TASK_H

typedef struct {
    unsigned long long event_id;
    char md5[33];
    unsigned int embedding_version;
} AiContentTask;

/* Parses the trusted Outbox JSON contract after the AMQP message ID is read. */
int parse_ai_content_task(unsigned long long event_id, const char *payload,
                          AiContentTask *task);

#endif
