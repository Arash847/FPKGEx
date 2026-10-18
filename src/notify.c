/*
 * notify.c - PS5 system notification (toast) support.
 *
 * The kernel exports sceKernelSendNotificationRequest on builds that support
 * it, so it is referenced weakly: on a kernel without the export the pointer
 * is null and the caller simply skips the toast instead of failing to load.
 */
#include "notify.h"

#include <stdio.h>
#include <string.h>

typedef struct notify_request {
    char reserved[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int)
    __attribute__((weak));

int
fpkgex_notify(const char *message, const char *submessage) {
    notify_request_t req;

    if(!sceKernelSendNotificationRequest) return -1;

    memset(&req, 0, sizeof(req));
    if(submessage && submessage[0]) {
        snprintf(req.message, sizeof(req.message), "%s\n%s",
                 message ? message : "fPKGEx", submessage);
    } else {
        snprintf(req.message, sizeof(req.message), "%s",
                 message ? message : "fPKGEx");
    }
    if(!req.message[0]) return -1;

    return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0) == 0 ? 0 : -1;
}
