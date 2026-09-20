#ifndef AI_CLOUD_SHARE_ID_H
#define AI_CLOUD_SHARE_ID_H

#define SHARE_ID_HEX_LENGTH 64

int generate_share_id(char output[SHARE_ID_HEX_LENGTH + 1]);
int validate_share_id(const char *value);

#endif
