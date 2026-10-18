/*
 * main.c - fPKGEx payload entry point.
 *
 * Serves the web UI on the console and runs extractions locally. Copy the
 * built fpkgex.elf to a payload folder and launch it from Payload Manager, then
 * open http://<PS5_IP>:5911/ in a browser.
 */
#include "api.h"
#include "httpd.h"
#include "jobs.h"
#include "notify.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef FPKEX_PORT
#define FPKEX_PORT 5911
#endif
#ifndef FPKEX_VERSION
#define FPKEX_VERSION "dev"
#endif

#define FPKEX_PAYLOAD_NAME "fpkgex.elf"
#define LISTEN_RETRIES    5

static void
on_ready(unsigned short port, void *arg) {
    char ip[64];
    char url[160];
    (void)arg;

    api_detect_lan_ip(ip, sizeof(ip));
    snprintf(url, sizeof(url), "http://%s:%u/", ip, (unsigned)port);

    /* Tell the user where to point a browser. Best effort: on kernels without
     * the notification entry point this is a no-op. */
    if(fpkgex_notify("fPKGEx ready", url) != 0) {
        fprintf(stderr, "fPKGEx: system notification unavailable\n");
    }

    fprintf(stderr, "fPKGEx %s ready at %s\n", FPKEX_VERSION, url);
    fflush(stderr);
}

int
main(int argc, char **argv) {
    int rc = -1;
    int attempt;

    (void)argc;
    (void)argv;

#if defined(SYS_thr_set_name)
    /* Names the process so it is recognisable in the console's task list. */
    syscall(SYS_thr_set_name, -1, FPKEX_PAYLOAD_NAME);
#endif

    api_init();

    for(attempt = 1; attempt <= LISTEN_RETRIES; attempt++) {
        fprintf(stderr, "fPKGEx: listening on port %d (attempt %d/%d)\n",
                FPKEX_PORT, attempt, LISTEN_RETRIES);
        fflush(stderr);
        rc = httpd_serve((unsigned short)FPKEX_PORT, api_route, NULL,
                         on_ready, NULL);
        if(rc == 0) return 0;
        sleep(1);
    }

    fprintf(stderr, "fPKGEx: failed to start web server\n");
    return rc;
}
