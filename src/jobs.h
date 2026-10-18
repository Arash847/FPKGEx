/*
 * jobs.h - Extraction job state.
 */
#ifndef FPKEX_JOBS_H
#define FPKEX_JOBS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    JOB_IDLE = 0,
    JOB_RUNNING,
    JOB_DONE,
    JOB_FAILED,
    JOB_CANCELLED
} job_state_t;

typedef struct {
    job_state_t state;
    char        package[1024];
    char        dest[1024];
    char        message[512];

    uint64_t    total_bytes;
    uint64_t    done_bytes;
    int         file_index;
    int         file_total;
    int         files_written;

    char        current_file[512];
} job_t;

void jobs_init(void);

/* Starts an extraction. Returns 0 when accepted. */
int  jobs_start(const char *package_path, const char *dest_dir);

/* Cancels the running job, if any. */
void jobs_cancel(void);

/* Fills `out` with a JSON object describing the current job. */
void jobs_status_json(char *out, size_t out_size);

int  jobs_is_busy(void);

#endif /* FPKEX_JOBS_H */
