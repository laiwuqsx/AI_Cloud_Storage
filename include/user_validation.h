#ifndef AI_CLOUD_USER_VALIDATION_H
#define AI_CLOUD_USER_VALIDATION_H

int validate_username(const char *value);
int validate_nickname(const char *value);
int validate_password_md5(const char *value);
int validate_file_name(const char *value);
int create_salt(char output[33]);
void make_password_digest(const char *salt, const char *client_password_md5,
                          char output[33]);

#endif
