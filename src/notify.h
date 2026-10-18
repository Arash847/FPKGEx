/*
 * notify.h - PS5 system notification (toast) support.
 */
#ifndef FPKEX_NOTIFY_H
#define FPKEX_NOTIFY_H

/*
 * Shows a system notification. Returns 0 on success, -1 when the kernel entry
 * point is unavailable. Safe to call anywhere; it never aborts the caller.
 */
int fpkgex_notify(const char *message, const char *submessage);

#endif /* FPKEX_NOTIFY_H */
