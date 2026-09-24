#ifndef AI_CLOUD_OUTBOX_REPOSITORY_H
#define AI_CLOUD_OUTBOX_REPOSITORY_H

#include <mysql/mysql.h>

#include "ai_index_event.h"

/* Writes AI state and its durable event on a caller-owned MySQL transaction. */
int enqueue_ai_index_event_in_transaction(MYSQL *connection,
                                          const AiIndexEvent *event);

#endif
