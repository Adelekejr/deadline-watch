/*
 * DeadlineWatch — deadline accounting and enforcement POLICY.
 *
 * This file and dw_core.c contain no QNX calls at all. They decide WHEN a
 * cycle is a miss and WHEN to throttle, escalate or go to the safe state;
 * supervisor.c carries out those decisions with QNX kernel calls
 * (pthread_setschedparam / SchedSet, pulses, replies). The split keeps
 * the policy unit-testable on any host (tests/test_core.c) while the
 * mechanism stays QNX-native.
 */
#ifndef DW_CORE_H
#define DW_CORE_H

#include <stdint.h>
#include <time.h>

#define DW_MAX_TASKS      16
#define DW_TASK_NAME_LEN  24

/* Task roles. CRITICAL tasks drive enforcement; TRACKED tasks are only
 * measured. (Demotable best-effort threads are not tasks: they are
 * enforcement targets, kept by the supervisor.) */
#define DW_ROLE_CRITICAL  1
#define DW_ROLE_TRACKED   2

/* Actions the policy asks the mechanism to carry out (bit mask). */
#define DW_ACT_NONE       0
#define DW_ACT_THROTTLE   1   /* demote every enforcement target */
#define DW_ACT_ESCALATE   2   /* demote targets to the minimum priority */
#define DW_ACT_SAFE_STATE 4   /* command critical tasks into their safe state */

typedef void (*dw_log_fn)(void *user, const struct timespec *now,
                          const char *who, const char *event, long cycle,
                          long lateness_us, long worst_us, const char *detail);

typedef struct {
    char name[DW_TASK_NAME_LEN];
    int role;                 /* DW_ROLE_CRITICAL / DW_ROLE_TRACKED */
    int period_ms;
    int active;               /* 0 = free slot or lost task */
    struct timespec epoch;    /* cycle k runs in [epoch+k*P, epoch+(k+1)*P) */
    long cycles;              /* reported (completed) cycles */
    long misses;              /* late completions + skipped + starved */
    long worst_lateness_us;
    long total_lateness_us;   /* sum of signed lateness of reported cycles */
    long consecutive_misses;
    long max_consecutive;     /* longest run of consecutive misses (outage) */
    int gone;                 /* 0 active/never, 1 unregistered, 2 lost */
    long expected_cycle;      /* next cycle index not yet accounted for */
    long clean_cycles;        /* consecutive on-time cycles */
    long win_judged;          /* cycles judged in the current 1 s window */
    long win_missed;          /* ...of which missed */
    /* Identity, filled by the mechanism (not used by the policy). */
    int pid, tid, scoid;
} dw_task_t;

typedef struct {
    dw_task_t task[DW_MAX_TASKS];
    int guard_on;             /* 0 = measure only */
    int targets;              /* enforcement targets available right now */
    int miss_threshold;       /* consecutive critical misses before THROTTLE */
    int recover_cycles;       /* clean cycles before RECOVER is logged */
    int grace_ms;             /* watchdog grace past a deadline */
    int safe_after_ms;        /* 0 = off; continuous-failure limit, see dw_core.c */
    int safe_miss_pct;        /* 0 = off; miss-rate limit per 1 s window */
    int throttled, escalated, recovered, safe_state;
    const char *safe_reason;  /* "continuous-failure", "miss-rate", "lost" */
    long throttle_events;
    int trigger_task;         /* task that caused the last action */
    long trigger_cycle;
    dw_log_fn log;
    void *log_user;
} dw_core_t;

/* time helpers */
int64_t dw_diff_us(const struct timespec *a, const struct timespec *b); /* b - a */
void dw_add_ms(const struct timespec *base, long ms, struct timespec *out);

void dw_core_init(dw_core_t *c, int guard_on, int safe_after_ms,
                  dw_log_fn log, void *log_user);
/* Optional second safe-state criterion (default 0 = off). */
void dw_core_set_safe_miss_pct(dw_core_t *c, int pct);
/* Returns the task index, or -1 if the table is full / arguments bad. */
int dw_core_add_task(dw_core_t *c, const char *name, int role, int period_ms,
                     const struct timespec *epoch);
/* A completion report for cycle `cycle` of task ti, received at `now`. */
int dw_core_report(dw_core_t *c, int ti, long cycle, const struct timespec *now);
/* Periodic watchdog: counts starved cycles (deadline + grace passed with
 * no report). */
int dw_core_tick(dw_core_t *c, const struct timespec *now);
/* The task's process went away. A lost CRITICAL task is a safe-state
 * event when the guard is on (nobody is running that control loop). */
int dw_core_lost(dw_core_t *c, int ti, const struct timespec *now);
/* The task said goodbye (clean exit): stop tracking it, no action. */
void dw_core_unregister(dw_core_t *c, int ti, const struct timespec *now);

#endif /* DW_CORE_H */
