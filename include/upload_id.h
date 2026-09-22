#ifndef AI_CLOUD_UPLOAD_ID_H
#define AI_CLOUD_UPLOAD_ID_H

#define UPLOAD_ID_HEX_LENGTH 64

int generate_upload_id(char output[UPLOAD_ID_HEX_LENGTH + 1]);
int validate_upload_id(const char *value);

#endif
