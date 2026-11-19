/*
 * DeadlineWatch — runtime deadline supervisor for QNX 8.0.
 * Shared types and declarations for the supervisor binary.
 *
 * QNX-only program: it uses QNX message passing (channels, pulses,
 * MsgReceive/MsgReply, name_attach), kernel scheduling calls
 * (SchedSet) and POSIX real-time scheduling. The deadline POLICY lives
 * in dw_core.[ch], which has no QNX calls and is unit-tested on the host.
 *
 * Adaptive Partitioning is NOT used: QNX OS 8.0 does not support it
 * (see docs/design-8.0.md). Enforcement is by strict-priority
 * scheduling: the supervisor demotes the offending threads.
 *
 * Two modes:
 *   local  (default) one process: supervisor + control + sensor + rogue
 *          threads. This is the mode the recorded QEMU runs used.
 *   system the supervisor registers the name "deadline-watch"; other
 *          processes (examples/pump.c, examples/rogue.c, or anything
 *          linking dw_client.c) register with it and are guarded across
 *          process boundaries.
 */
#ifndef DW_H
#define DW_H

#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE          /* QNX extensions: SIGEV_PULSE_INIT, _SC_NPROCESSORS_ONLN */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <pthread.h>
#include <sched.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "dw_core.h"
#include "dw_proto.h"

/* Pulse codes sent to the supervisor channel (user codes must be 0..127). */
#define DW_PULSE_CONTROL_DONE    1
#define DW_PULSE_SENSOR_DONE     2
#define DW_PULSE_ROGUE_PROGRESS  3
#define DW_PULSE_TIMER           4

#define DW_MODE_LOCAL   0
#define DW_MODE_SYSTEM  1

#define DW_TIMER_PERIOD_MS   1    /* supervisor watchdog tick */
#define DW_MAX_TARGETS       64   /* demotable threads */
#define DW_MAX_DEMOTE_PIDS   8
#define DW_MAX_TID_WALK      256  /* --demote-pid: thread ids probed per process */

typedef struct {
    int mode;                 /* DW_MODE_LOCAL / DW_MODE_SYSTEM */
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
    int rogue_on;             /* local mode: 1 = start rogue threads */
    int duration_s;           /* 30; 0 = run until SIGINT/SIGTERM */
    int safe_after_ms;        /* 300; continuous-failure limit (0 = off) */
    int safe_miss_pct;        /* 50; miss-rate limit per 1 s window (0 = off) */
    int rescue_boost;         /* 1 = on SAFE_STATE, boost critical threads so
                                 they can run their safe-state code */
    int live;                 /* 1 = print a live status line every second */
    int control_work_iters;   /* stand-in workload size; calibrate on target */
    int sensor_work_iters;
    int demote_pids[DW_MAX_DEMOTE_PIDS]; /* system mode: unregistered processes to demote */
    int n_demote_pids;
    char log_path[256];       /* deadline-watch.csv */
    char config_path[256];    /* config/deadline-watch.conf */
} dw_config_t;

/* A thread the supervisor may demote. */
typedef struct {
    int active;
    int remote;               /* 0 = local pthread, 1 = other process */
    pthread_t ptid;           /* local */
    int pid, tid, scoid;      /* remote (from the kernel, not the client) */
    char name[DW_NAME_LEN];
} dw_target_t;

typedef struct dw_ctx {
    dw_config_t cfg;
    dw_core_t core;           /* policy state: tasks, ladder, counters */
    int chid;                 /* supervisor channel */
    int coid;                 /* side-channel connection (local pulses + timer) */
    name_attach_t *attach;    /* system mode: the registered name */
    timer_t watchdog;         /* 1 ms timer */
    FILE *log;
    int log_failed;           /* a log write has failed (warned once) */
    struct timespec start;    /* monotonic start time */
    int control_ti, sensor_ti;/* local mode: core task indices */
    pthread_t control_tid;
    pthread_t sensor_tid;
    pthread_t *rogue_tids;
    int n_rogue;
    dw_target_t target[DW_MAX_TARGETS];
    int n_target;
    int rogue_prio_current;   /* priority the targets are at right now */
    long rogue_progress;      /* progress units in the current second */
    long rogue_windows_total; /* cumulative */
    long last_misses[DW_MAX_TASKS]; /* for the live per-second view */
    struct timespec last_second;
    int pid_threads[DW_MAX_DEMOTE_PIDS]; /* --demote-pid threads at the demoted prio */
    unsigned tick_count;
    volatile sig_atomic_t stop;
    volatile sig_atomic_t safe_state; /* read by local worker threads */
} dw_ctx_t;

/* config.c */
void dw_config_defaults(dw_config_t *cfg);
int dw_config_load_file(dw_config_t *cfg, const char *path);
int dw_config_parse_args(dw_config_t *cfg, int argc, char **argv);
void dw_config_print(const dw_config_t *cfg);

/* log.c — single writer: the supervisor thread only. */
int dw_log_open(dw_ctx_t *ctx);
void dw_log_close(dw_ctx_t *ctx);
void dw_log_flush(dw_ctx_t *ctx);
void dw_log_row(dw_ctx_t *ctx, const struct timespec *now,
                const char *thread, const char *event,
                long cycle, long lateness_us, long worst_us,
                const char *detail);
void dw_log_core_cb(void *user, const struct timespec *now,
                    const char *who, const char *event, long cycle,
                    long lateness_us, long worst_us, const char *detail);
void dw_log_summary(dw_ctx_t *ctx);

/* threads.c (local mode) */
void *dw_control_thread(void *arg);
void *dw_sensor_thread(void *arg);
void *dw_rogue_thread(void *arg);
void dw_burn(int iters);

/* supervisor.c */
int dw_supervisor_run(dw_ctx_t *ctx);

#endif /* DW_H */
