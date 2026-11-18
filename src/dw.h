/*
 * DeadlineWatch — runtime deadline supervisor for QNX 8.0 (QEMU).
 * Shared types and declarations.
 *
 * QNX-ONLY program: it uses the QNX message-passing API (channels,
 * pulses) and POSIX real-time scheduling. It does not build on the host.
 *
 * NOTE (Stage 1 skeleton): this code is written against the QNX 8.0
 * documentation (see docs/design-8.0.md). It has NOT yet been compiled
 * against an installed SDP — the myQNX licence is pending. Every
 * QNX-specific call used here is a documented 8.0 API; verify signatures
 * against $QNX_TARGET/usr/include once the SDP is installed.
 * In particular, Adaptive Partitioning is NOT used: QNX OS 8.0 does not
 * support it (see docs/design-8.0.md). Enforcement is by strict-priority
 * scheduling — the supervisor demotes the rogue thread's priority.
 */
#ifndef DW_H
#define DW_H

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <pthread.h>
#include <sched.h>
#include <sys/neutrino.h>

/* Pulse codes sent to the supervisor channel (user codes must be 0..127). */
#define DW_PULSE_CONTROL_DONE    1
#define DW_PULSE_SENSOR_DONE     2
#define DW_PULSE_ROGUE_PROGRESS  3
#define DW_PULSE_TIMER           4

#define DW_THREAD_CONTROL 0
#define DW_THREAD_SENSOR  1
#define DW_NTRACKED       2   /* control + sensor are deadline-tracked */

#define DW_MISS_THRESHOLD    2    /* consecutive misses before throttling */
#define DW_RECOVER_CYCLES    50   /* clean control cycles before RECOVER */
#define DW_WATCHDOG_GRACE_MS 2    /* grace after a deadline before starvation is declared */
#define DW_TIMER_PERIOD_MS   1    /* supervisor watchdog tick */

typedef struct {
    int control_period_ms;    /* 10 */
    int sensor_period_ms;     /* 50 */
    int supervisor_prio;      /* 30 */
    int control_prio;         /* 20 */
    int sensor_prio;          /* 15 */
    int rogue_prio;           /* 25: above control, so it starves the loop when the guard is off */
    int rogue_demoted_prio;   /* 5: below control, so the loop preempts it when the guard is on */
    int rogue_duty_pct;       /* 85: rogue burns 85% of each work window */
    int rogue_work_window_ms; /* 10 */
    int rogue_threads;        /* 0 = auto: one rogue thread per online CPU */
    int guard_on;             /* 1 = enforce, 0 = measure only */
    int rogue_on;             /* 1 = start rogue threads, 0 = control-only run */
    int duration_s;           /* 30 */
    int control_work_iters;   /* stand-in workload size; calibrate on target */
    int sensor_work_iters;
    char log_path[256];       /* deadline-watch.csv */
    char config_path[256];    /* config/deadline-watch.conf */
} dw_config_t;

typedef struct {
    const char *name;
    int period_ms;
    long cycles;              /* reported cycles */
    long misses;              /* deadline misses */
    long worst_lateness_us;   /* worst (largest) lateness seen, microseconds */
    long total_lateness_us;   /* sum of signed lateness, for the mean */
    long consecutive_misses;
    long expected_cycle;      /* next cycle index not yet accounted for */
    long clean_cycles;        /* consecutive clean control cycles (recovery) */
} dw_thread_state_t;

typedef struct dw_ctx {
    dw_config_t cfg;
    int chid;                 /* supervisor channel */
    int coid;                 /* connection for pulses (threads + timer) */
    timer_t watchdog;         /* 1 ms timer */
    FILE *log;
    struct timespec start;    /* monotonic start time (cycle 0 epoch) */
    dw_thread_state_t track[DW_NTRACKED]; /* [0]=control, [1]=sensor */
    pthread_t control_tid;
    pthread_t sensor_tid;
    pthread_t *rogue_tids;
    int n_rogue;
    int rogue_prio_current;   /* rogue priority right now (starts at rogue_prio) */
    int throttled;            /* rogue has been demoted this run */
    int escalated;            /* rogue demoted to minimum priority */
    int recovered;            /* RECOVER logged for the current episode */
    long throttle_events;
    long rogue_progress;      /* progress pulses (x100 windows) in the current second */
    long rogue_windows_total; /* cumulative rogue windows reported */
    struct timespec last_rogue_log;
    volatile sig_atomic_t stop;
} dw_ctx_t;

/* config.c */
void dw_config_defaults(dw_config_t *cfg);
int dw_config_load_file(dw_config_t *cfg, const char *path);
int dw_config_parse_args(dw_config_t *cfg, int argc, char **argv);
void dw_config_print(const dw_config_t *cfg);

/* log.c — single writer: the supervisor thread only. */
int dw_log_open(dw_ctx_t *ctx);
void dw_log_close(dw_ctx_t *ctx);
void dw_log_row(dw_ctx_t *ctx, const struct timespec *now,
                const char *thread, const char *event,
                long cycle, long lateness_us, long worst_us,
                const char *detail);
void dw_log_summary(dw_ctx_t *ctx);

/* threads.c */
void *dw_control_thread(void *arg);
void *dw_sensor_thread(void *arg);
void *dw_rogue_thread(void *arg);

/* supervisor.c */
int dw_supervisor_run(dw_ctx_t *ctx);

/* time helpers (supervisor.c) */
int64_t dw_diff_us(const struct timespec *a, const struct timespec *b); /* b - a, microseconds */
void dw_add_ms(const struct timespec *base, long ms, struct timespec *out);

#endif /* DW_H */
