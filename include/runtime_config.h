#ifndef AI_CLOUD_RUNTIME_CONFIG_H
#define AI_CLOUD_RUNTIME_CONFIG_H

/* Capture process configuration before FastCGI replaces the request environment. */
void runtime_config_init(void);
const char *runtime_config_get(const char *name, const char *fallback);

#endif
