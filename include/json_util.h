#ifndef AI_CLOUD_JSON_UTIL_H
#define AI_CLOUD_JSON_UTIL_H

#include <stddef.h>

/* Minimal JSON helpers for the first FastCGI endpoint.
 * They handle flat string fields only. cJSON will replace them later. */
int json_get_string(const char *json, const char *key, char *out, size_t out_size);
int json_is_valid_object(const char *json);

#endif
