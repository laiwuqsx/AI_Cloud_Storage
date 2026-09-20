#ifndef AI_CLOUD_SHARE_ACCESS_SERVICE_H
#define AI_CLOUD_SHARE_ACCESS_SERVICE_H

typedef enum {
    SHARE_ACCESS_ERROR = -1,
    SHARE_ACCESS_GRANTED = 0,
    SHARE_ACCESS_UNAVAILABLE = 1,
    SHARE_ACCESS_CODE_REQUIRED = 2,
    SHARE_ACCESS_CODE_INVALID = 3,
    SHARE_ACCESS_RATE_LIMITED = 4
} ShareAccessResult;

ShareAccessResult authorize_share_access(const char *share_id,
                                         const char *submitted_code,
                                         const char *actor);

#endif
