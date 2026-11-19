/*
 * DeadlineWatch — supervisor (monitor + enforcement mechanism) and main().
 *
 * The supervisor is the main thread, at the highest priority. It owns a
 * QNX channel created with _NTO_CHF_FIXED_PRIORITY (so receiving a
 * message from a low-priority client never lowers the supervisor's own
 * priority) and receives:
 *   - a 1 ms watchdog timer pulse (timer_create + SIGEV_PULSE_INIT),
 *   - local mode: completion pulses from its own control and sensor
 *     threads and progress pulses from its rogue threads,
 *   - system mode: REGISTER / BEAT / PROGRESS messages (MsgSend) from
 *     OTHER processes that found it with name_open("deadline-watch"),
 *     and the kernel's _PULSE_CODE_DISCONNECT when one of them dies.
 *
 * WHEN to act is decided by the pure policy in dw_core.c (deadline
 * accounting, THROTTLE -> ESCALATE -> SAFE_STATE ladder). HOW to act is
 * here:
 *   - demote a local thread:    pthread_setschedparam()
 *   - demote another process's thread: SchedSet(pid, tid, ...) with the
 *     pid/tid the kernel reported in struct _msg_info at registration,
 *     or, for --demote-pid, every thread id that exists in that process
 *   - safe state: the next BEAT reply to each critical client carries
 *     DW_CMD_SAFE_STATE (the client stops its actuator); local threads
 *     read ctx->safe_state.
 *
 * Adaptive Partitioning does not exist in QNX 8.0, so there is no kernel
 * CPU budget to shrink; enforcement is strict-priority scheduling. See
 * docs/design-8.0.md.
 */
#include "dw.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/iomsg.h>

#ifndef SCHED_NOCHANGE
#define SCHED_NOCHANGE SCHED_FIFO
#endif

static dw_ctx_t *g_ctx;   /* for the signal handler only */

static void dw_on_signal(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->stop = 1;  /* async-signal-safe: the loop sees it within 1 ms */
}

/* ---- enforcement mechanism ------------------------------------------ */

static int dw_target_set_prio(dw_target_t *t, int prio)
{
    struct sched_param p;
    memset(&p, 0, sizeof(p));
    p.sched_priority = prio;
    if (!t->remote)
        return pthread_setschedparam(t->ptid, SCHED_FIFO, &p); /* returns an error code */
    if (SchedSet(t->pid, t->tid, SCHED_NOCHANGE, &p) == -1)
        return errno;
    return 0;
}

/* --demote-pid: walk thread ids 1..DW_MAX_TID_WALK of a process that did
 * not register. QNX thread ids are small integers starting at 1; ESRCH
 * means "no such thread". Returns the number of threads demoted. */
static int dw_demote_pid(int pid, int prio)
{
    int ok = 0, gap = 0;
    struct sched_param p;
    memset(&p, 0, sizeof(p));
    p.sched_priority = prio;
    for (int tid = 1; tid <= DW_MAX_TID_WALK && gap < 32; tid++) {
        if (SchedSet(pid, tid, SCHED_NOCHANGE, &p) == 0) {
            ok++;
            gap = 0;
        } else {
            gap++;
        }
    }
    return ok;
}

static void dw_demote_all(dw_ctx_t *ctx, int prio, int *ok_out, int *total_out,
                          int *err_out)
{
    int ok = 0, total = 0, err = 0;
    for (int i = 0; i < ctx->n_target; i++) {
        dw_target_t *t = &ctx->target[i];
        if (!t->active)
            continue;
        total++;
        int rc = dw_target_set_prio(t, prio);
        if (rc == 0)
            ok++;
        else if (!err)
            err = rc;
    }
    for (int i = 0; i < ctx->cfg.n_demote_pids; i++) {
        int n = dw_demote_pid(ctx->cfg.demote_pids[i], prio);
        ctx->pid_threads[i] = n;
        ok += n;
        total += n > 0 ? n : 1;
        if (n == 0 && !err)
            err = ESRCH;
    }
    ctx->rogue_prio_current = prio;
    *ok_out = ok;
    *total_out = total;
    *err_out = err;
}

static void dw_redemote_pids(dw_ctx_t *ctx, const struct timespec *now)
{
    for (int i = 0; i < ctx->cfg.n_demote_pids; i++) {
        int n = dw_demote_pid(ctx->cfg.demote_pids[i], ctx->rogue_prio_current);
        if (n > ctx->pid_threads[i]) {
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "pid %d has %d new thread(s): demoted to prio %d",
                     ctx->cfg.demote_pids[i], n - ctx->pid_threads[i],
                     ctx->rogue_prio_current);
            dw_log_row(ctx, now, "supervisor", "THROTTLE", 0, 0, 0, detail);
        }
        ctx->pid_threads[i] = n;
    }
}

static void dw_refresh_targets(dw_ctx_t *ctx)
{
    int n = ctx->cfg.n_demote_pids;
    for (int i = 0; i < ctx->n_target; i++)
        if (ctx->target[i].active)
            n++;
    ctx->core.targets = n;
}

/* Rescue boost: when the safe state is commanded, the critical threads
 * may be the very ones being starved (e.g. by an offender that could not
 * be demoted). Raise them just below the supervisor so they get the CPU
 * to execute their safe-state code. Returns threads boosted. */
static int dw_rescue_boost(dw_ctx_t *ctx)
{
    struct sched_param p;
    int n = 0;
    memset(&p, 0, sizeof(p));
    p.sched_priority = ctx->cfg.supervisor_prio - 1;
    for (int ti = 0; ti < DW_MAX_TASKS; ti++) {
        dw_task_t *t = &ctx->core.task[ti];
        if (!t->active || t->role != DW_ROLE_CRITICAL)
            continue;
        if (t->pid != 0) {
            if (SchedSet(t->pid, t->tid, SCHED_NOCHANGE, &p) == 0)
                n++;
        } else if (ti == ctx->control_ti) {
            if (pthread_setschedparam(ctx->control_tid, SCHED_FIFO, &p) == 0)
                n++;
        }
    }
    return n;
}

static const char *dw_task_name(const dw_ctx_t *ctx, int ti)
{
    return (ti >= 0 && ti < DW_MAX_TASKS) ? ctx->core.task[ti].name : "?";
}

/* Carry out what the policy decided. */
static void dw_apply(dw_ctx_t *ctx, int acts, const struct timespec *now)
{
    char detail[192];
    int ok, total, err;
    int ti = ctx->core.trigger_task;
    long worst = (ti >= 0) ? ctx->core.task[ti].worst_lateness_us : 0;

    if (acts & DW_ACT_THROTTLE) {
        dw_demote_all(ctx, ctx->cfg.rogue_demoted_prio, &ok, &total, &err);
        snprintf(detail, sizeof(detail),
                 "%s missed %d in a row: demoted %d of %d offender threads "
                 "to prio %d%s%s",
                 dw_task_name(ctx, ti), ctx->core.miss_threshold, ok, total,
                 ctx->cfg.rogue_demoted_prio,
                 err ? " (first error: " : "", err ? strerror(err) : "");
        if (err)
            strncat(detail, ")", sizeof(detail) - strlen(detail) - 1);
        dw_log_row(ctx, now, "supervisor", "THROTTLE",
                   ctx->core.trigger_cycle, 0, worst, detail);
    }
    if (acts & DW_ACT_ESCALATE) {
        int minp = sched_get_priority_min(SCHED_FIFO);
        dw_demote_all(ctx, minp, &ok, &total, &err);
        snprintf(detail, sizeof(detail),
                 "%s still missing after demotion: %d of %d offender "
                 "threads dropped to minimum prio %d",
                 dw_task_name(ctx, ti), ok, total, minp);
        dw_log_row(ctx, now, "supervisor", "ESCALATE",
                   ctx->core.trigger_cycle, 0, worst, detail);
    }
    if (acts & DW_ACT_SAFE_STATE) {
        ctx->safe_state = 1;
        dw_task_t *t = (ti >= 0) ? &ctx->core.task[ti] : NULL;
        const char *why = ctx->core.safe_reason ? ctx->core.safe_reason : "?";
        int boosted = 0;
        if (ctx->cfg.rescue_boost)
            boosted = dw_rescue_boost(ctx);
        if (t && !t->active)
            snprintf(detail, sizeof(detail),
                     "critical task %s lost (pid %d): safe state latched; "
                     "operator alarm", t->name, t->pid);
        else
            snprintf(detail, sizeof(detail),
                     "%s: %s still failing with enforcement exhausted; safe "
                     "state commanded; %d critical thread(s) boosted to prio %d",
                     why, t ? t->name : "?", boosted,
                     ctx->cfg.supervisor_prio - 1);
        dw_log_row(ctx, now, "supervisor", "SAFE_STATE",
                   ctx->core.trigger_cycle, 0, worst, detail);
        if (ctx->cfg.live) {
            printf("*** SAFE STATE: %s ***\n", detail);
            fflush(stdout);
        }
    }
}

/* ---- system mode: messages from other processes --------------------- */

static int dw_find_target_slot(dw_ctx_t *ctx)
{
    if (ctx->n_target < DW_MAX_TARGETS)
        return ctx->n_target++;
    return -1;
}

static void dw_handle_register(dw_ctx_t *ctx, long rcvid,
                               const struct _msg_info *info,
                               const dw_msg_t *m, const struct timespec *now)
{
    dw_reply_t r;
    char name[DW_NAME_LEN];
    char detail[160];
    memset(&r, 0, sizeof(r));
    snprintf(name, sizeof(name), "%.*s", DW_NAME_LEN - 1, m->name);
    /* Commas would break the CSV; spaces are kept. */
    for (char *p = name; *p; p++)
        if (*p == ',' || *p == '\n' || *p == '\r')
            *p = '_';
    if (!name[0])
        snprintf(name, sizeof(name), "pid%d", info->pid);

    if (m->kind == DW_KIND_CRITICAL || m->kind == DW_KIND_TRACKED) {
        int role = m->kind == DW_KIND_CRITICAL ? DW_ROLE_CRITICAL
                                               : DW_ROLE_TRACKED;
        int ti = dw_core_add_task(&ctx->core, name, role, m->period_ms, now);
        if (ti < 0) {
            r.status = m->period_ms < 1 ? EINVAL : ENOSPC;
        } else {
            dw_task_t *t = &ctx->core.task[ti];
            t->pid = info->pid;
            t->tid = info->tid;
            t->scoid = info->scoid;
            ctx->last_misses[ti] = 0;
            r.slot = ti;
            r.epoch_sec = (int64_t)now->tv_sec;
            r.epoch_nsec = (int64_t)now->tv_nsec;
            snprintf(detail, sizeof(detail), "%s pid=%d tid=%d period=%dms",
                     role == DW_ROLE_CRITICAL ? "critical" : "tracked",
                     info->pid, info->tid, m->period_ms);
            dw_log_row(ctx, now, name, "REGISTER", 0, 0, 0, detail);
        }
    } else if (m->kind == DW_KIND_DEMOTABLE) {
        int si = dw_find_target_slot(ctx);
        if (si < 0) {
            r.status = ENOSPC;
        } else {
            dw_target_t *t = &ctx->target[si];
            memset(t, 0, sizeof(*t));
            t->active = 1;
            t->remote = 1;
            t->pid = info->pid;
            t->tid = info->tid;
            t->scoid = info->scoid;
            snprintf(t->name, sizeof(t->name), "%s", name);
            dw_refresh_targets(ctx);
            r.slot = DW_MAX_TASKS + si;
            r.epoch_sec = (int64_t)now->tv_sec;
            r.epoch_nsec = (int64_t)now->tv_nsec;
            int rc = 0;
            /* Arrived after enforcement started: demote it straight away. */
            if (ctx->core.throttled)
                rc = dw_target_set_prio(t, ctx->rogue_prio_current);
            snprintf(detail, sizeof(detail),
                     "demotable pid=%d tid=%d%s%s", info->pid, info->tid,
                     ctx->core.throttled ? " (demoted on arrival)" : "",
                     rc ? " (demotion failed)" : "");
            dw_log_row(ctx, now, name, "REGISTER", 0, 0, 0, detail);
        }
    } else {
        r.status = EINVAL;
    }
    MsgReply(rcvid, 0, &r, sizeof(r));
}

static void dw_handle_msg(dw_ctx_t *ctx, long rcvid,
                          const struct _msg_info *info, const dw_msg_t *m,
                          const struct timespec *now)
{
    dw_reply_t r;
    memset(&r, 0, sizeof(r));

    if (m->req == DW_REQ_REGISTER) {
        dw_handle_register(ctx, rcvid, info, m, now);
        return;
    }
    if (m->req == DW_REQ_BEAT && m->slot >= 0 && m->slot < DW_MAX_TASKS &&
        ctx->core.task[m->slot].active &&
        ctx->core.task[m->slot].scoid == info->scoid) {
        int acts = dw_core_report(&ctx->core, m->slot, (long)m->value, now);
        if (acts)
            dw_apply(ctx, acts, now);
        r.slot = m->slot;
        r.command = (ctx->safe_state &&
                     ctx->core.task[m->slot].role == DW_ROLE_CRITICAL)
                    ? DW_CMD_SAFE_STATE : DW_CMD_RUN;
    } else if (m->req == DW_REQ_PROGRESS && m->slot >= DW_MAX_TASKS &&
               m->slot < DW_MAX_TASKS + ctx->n_target &&
               ctx->target[m->slot - DW_MAX_TASKS].active &&
               ctx->target[m->slot - DW_MAX_TASKS].scoid == info->scoid) {
        if (m->value > 0 && m->value < 1000000)
            ctx->rogue_progress += (long)m->value;
        r.slot = m->slot;
    } else if (m->req == DW_REQ_UNREGISTER && m->slot >= 0 &&
               m->slot < DW_MAX_TASKS && ctx->core.task[m->slot].active &&
               ctx->core.task[m->slot].scoid == info->scoid) {
        dw_core_unregister(&ctx->core, m->slot, now);
        r.slot = m->slot;
    } else if (m->req == DW_REQ_UNREGISTER && m->slot >= DW_MAX_TASKS &&
               m->slot < DW_MAX_TASKS + ctx->n_target &&
               ctx->target[m->slot - DW_MAX_TASKS].active &&
               ctx->target[m->slot - DW_MAX_TASKS].scoid == info->scoid) {
        dw_target_t *t = &ctx->target[m->slot - DW_MAX_TASKS];
        t->active = 0;
        dw_refresh_targets(ctx);
        dw_log_row(ctx, now, t->name, "UNREGISTER", 0, 0, 0, "clean exit");
        r.slot = m->slot;
    } else {
        r.status = EPERM;   /* unknown slot, or not the connection that owns it */
    }
    MsgReply(rcvid, 0, &r, sizeof(r));
}

static void dw_handle_disconnect(dw_ctx_t *ctx, int scoid,
                                 const struct timespec *now)
{
    int acts = 0;
    for (int ti = 0; ti < DW_MAX_TASKS; ti++) {
        dw_task_t *t = &ctx->core.task[ti];
        if (t->active && t->scoid == scoid && t->pid != 0)
            acts |= dw_core_lost(&ctx->core, ti, now);
    }
    for (int i = 0; i < ctx->n_target; i++) {
        dw_target_t *t = &ctx->target[i];
        if (t->active && t->remote && t->scoid == scoid) {
            t->active = 0;
            dw_log_row(ctx, now, t->name, "LOST", 0, 0, 0,
                       "demotable process disconnected");
        }
    }
    dw_refresh_targets(ctx);
    if (acts)
        dw_apply(ctx, acts, now);
    ConnectDetach(scoid);
}

/* ---- once a second: rogue row, live view, flush --------------------- */

/* Built into one buffer and written with a single write(), so the
 * supervisor blocks for one short I/O per second, not one per line. */
static void dw_live_line(dw_ctx_t *ctx, const struct timespec *now)
{
    char buf[2048];
    size_t n = 0;
    const char *state = !ctx->cfg.guard_on ? "MEASURE-ONLY"
                      : ctx->core.safe_state ? "SAFE STATE"
                      : ctx->core.escalated ? "ESCALATED"
                      : ctx->core.throttled ? "THROTTLED"
                      : "ARMED";
    long secs = (long)(dw_diff_us(&ctx->start, now) / 1000000L);
    n += (size_t)snprintf(buf + n, sizeof(buf) - n,
                          "[%4lds] guard %-12s offenders@prio %-3d "
                          "offender work/s %ld\n",
                          secs, state, ctx->rogue_prio_current,
                          ctx->rogue_progress);
    for (int ti = 0; ti < DW_MAX_TASKS && n < sizeof(buf) - 160; ti++) {
        dw_task_t *t = &ctx->core.task[ti];
        if (!t->name[0])
            continue;
        long per_s = 1000L / t->period_ms;
        long new_miss = t->misses - ctx->last_misses[ti];
        ctx->last_misses[ti] = t->misses;
        double pct = t->expected_cycle > 0
            ? 100.0 * (double)t->misses / (double)t->expected_cycle : 0.0;
        int bar = per_s > 0 ? (int)(20 * new_miss / per_s) : 0;
        if (bar > 20) bar = 20;
        if (bar < 0) bar = 0;
        char b[21];
        for (int i = 0; i < 20; i++)
            b[i] = i < bar ? '#' : '.';
        b[20] = '\0';
        n += (size_t)snprintf(buf + n, sizeof(buf) - n,
                              "        %-14s %s misses/s |%s| %3ld  "
                              "total %5.1f%%  worst %6.1f ms\n",
                              t->name,
                              t->gone == 2 ? "LOST" : t->gone == 1 ? "EXIT" : "    ",
                              b,
                              new_miss, pct,
                              (double)t->worst_lateness_us / 1000.0);
    }
    if (n > sizeof(buf))
        n = sizeof(buf) - 1;
    fflush(stdout);
    if (write(STDOUT_FILENO, buf, n) < 0) {
        /* stdout gone (e.g. closed pipe): stop the live view, keep guarding */
        ctx->cfg.live = 0;
    }
}

static void dw_second(dw_ctx_t *ctx, const struct timespec *now)
{
    char detail[96];
    snprintf(detail, sizeof(detail), "rogue windows/s=%ld prio=%d",
             ctx->rogue_progress, ctx->rogue_prio_current);
    ctx->rogue_windows_total += ctx->rogue_progress;
    dw_log_row(ctx, now, "rogue", "ROGUE", ctx->rogue_windows_total, 0, 0,
               detail);   /* non-CYCLE row: also flushes the log */
    if (ctx->cfg.live)
        dw_live_line(ctx, now);
    ctx->rogue_progress = 0;
    ctx->last_second = *now;
}

/* ---- main loop ------------------------------------------------------- */

typedef union {
    uint16_t type;
    struct _pulse pulse;
    dw_msg_t dw;
    char raw[256];
} dw_rx_t;

int dw_supervisor_run(dw_ctx_t *ctx)
{
    for (;;) {
        dw_rx_t rx;
        struct _msg_info info;
        memset(&info, 0, sizeof(info));
        long rcvid = MsgReceive(ctx->chid, &rx, sizeof(rx), &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                if (ctx->stop)
                    return 0;   /* SIGINT/SIGTERM: clean shutdown */
                continue;
            }
            fprintf(stderr, "deadline-watch: MsgReceive failed: %s\n",
                    strerror(errno));
            return -1;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if (rcvid > 0) {
            /* A message from another process (system mode). */
            if (rx.type == _IO_CONNECT) {
                MsgReply(rcvid, EOK, NULL, 0);          /* name_open() handshake */
            } else if (rx.type == DW_MSG_TYPE &&
                       (size_t)info.msglen >= sizeof(dw_msg_t)) {
                dw_handle_msg(ctx, rcvid, &info, &rx.dw, &now);
            } else {
                MsgError(rcvid, ENOSYS);
            }
        } else {
            int acts = 0;
            switch (rx.pulse.code) {
            case DW_PULSE_CONTROL_DONE:
                acts = dw_core_report(&ctx->core, ctx->control_ti,
                                      rx.pulse.value.sival_int, &now);
                break;
            case DW_PULSE_SENSOR_DONE:
                acts = dw_core_report(&ctx->core, ctx->sensor_ti,
                                      rx.pulse.value.sival_int, &now);
                break;
            case DW_PULSE_ROGUE_PROGRESS:
                ctx->rogue_progress += rx.pulse.value.sival_int;
                break;
            case DW_PULSE_TIMER:
                acts = dw_core_tick(&ctx->core, &now);
                /* A --demote-pid process may create threads after it was
                   demoted (seen on QNX: a rogue that starts its burners
                   late escaped a THROTTLE that came first). Every 50 ms
                   while throttled, re-apply the demotion to the whole pid. */
                if (++ctx->tick_count % 50 == 0 && ctx->core.throttled &&
                    !ctx->core.safe_state)
                    dw_redemote_pids(ctx, &now);
                if (dw_diff_us(&ctx->last_second, &now) >= 1000000L)
                    dw_second(ctx, &now);
                if (ctx->cfg.duration_s > 0 &&
                    dw_diff_us(&ctx->start, &now) >=
                        (int64_t)ctx->cfg.duration_s * 1000000L)
                    ctx->stop = 1;
                break;
            case _PULSE_CODE_DISCONNECT:
                dw_handle_disconnect(ctx, rx.pulse.scoid, &now);
                break;
            default:
                break;  /* unknown pulse: ignore */
            }
            if (acts)
                dw_apply(ctx, acts, &now);
        }

        if (ctx->stop)
            return 0;
    }
}

/* ---- setup ----------------------------------------------------------- */

static int dw_spawn(dw_ctx_t *ctx, pthread_t *tid,
                    void *(*fn)(void *), int prio, const char *name)
{
    pthread_attr_t attr;
    int rc;
    pthread_attr_init(&attr);
    /* pthread_* calls return an error number; they do not set errno. */
    if ((rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED)) != 0 ||
        (rc = pthread_attr_setschedpolicy(&attr, SCHED_FIFO)) != 0) {
        fprintf(stderr, "deadline-watch: cannot set %s thread scheduling: %s\n",
                name, strerror(rc));
        pthread_attr_destroy(&attr);
        return -1;
    }
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = prio;
    if ((rc = pthread_attr_setschedparam(&attr, &param)) != 0) {
        /* A bad priority must fail loudly: silently running at the
           default priority would make the demo meaningless. */
        fprintf(stderr, "deadline-watch: cannot set %s thread priority %d: %s\n",
                name, prio, strerror(rc));
        pthread_attr_destroy(&attr);
        return -1;
    }
    rc = pthread_create(tid, &attr, fn, ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "deadline-watch: cannot start %s thread: %s\n",
                name, strerror(rc));
        return -1;
    }
    return 0;
}

static void dw_clamp_prio(int *prio, int pmin, int pmax, const char *name)
{
    if (*prio < pmin) {
        fprintf(stderr, "deadline-watch: %s priority %d below minimum %d; "
                "clamped\n", name, *prio, pmin);
        *prio = pmin;
    } else if (*prio > pmax) {
        fprintf(stderr, "deadline-watch: %s priority %d above maximum %d; "
                "clamped\n", name, *prio, pmax);
        *prio = pmax;
    }
}

static int dw_start_local(dw_ctx_t *ctx)
{
    ctx->control_ti = dw_core_add_task(&ctx->core, "control", DW_ROLE_CRITICAL,
                                       ctx->cfg.control_period_ms, &ctx->start);
    ctx->sensor_ti = dw_core_add_task(&ctx->core, "sensor", DW_ROLE_TRACKED,
                                      ctx->cfg.sensor_period_ms, &ctx->start);

    /* Rogue threads: one per online CPU unless configured otherwise, so
       the rogue can saturate every CPU the guest has. */
    int n_rogue = 0;
    if (ctx->cfg.rogue_on) {
        n_rogue = ctx->cfg.rogue_threads;
        if (n_rogue <= 0) {
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            n_rogue = (ncpu > 0 && ncpu <= 32) ? (int)ncpu : 1;
        }
        if (n_rogue > 32)
            n_rogue = 32;
    }
    ctx->n_rogue = n_rogue;
    if (n_rogue > 0) {
        ctx->rogue_tids = calloc((size_t)n_rogue, sizeof(pthread_t));
        if (!ctx->rogue_tids) {
            fprintf(stderr, "deadline-watch: out of memory\n");
            return -1;
        }
    }

    printf("deadline-watch: local mode (%d rogue thread%s, guard %s)\n",
           n_rogue, n_rogue == 1 ? "" : "s", ctx->cfg.guard_on ? "on" : "off");

    if (dw_spawn(ctx, &ctx->control_tid, dw_control_thread,
                 ctx->cfg.control_prio, "control") != 0)
        return -1;
    if (dw_spawn(ctx, &ctx->sensor_tid, dw_sensor_thread,
                 ctx->cfg.sensor_prio, "sensor") != 0)
        return -1;
    for (int i = 0; i < n_rogue; i++) {
        if (dw_spawn(ctx, &ctx->rogue_tids[i], dw_rogue_thread,
                     ctx->cfg.rogue_prio, "rogue") != 0)
            return -1;
        dw_target_t *t = &ctx->target[ctx->n_target++];
        t->active = 1;
        t->ptid = ctx->rogue_tids[i];
        snprintf(t->name, sizeof(t->name), "rogue%d", i);
    }
    dw_refresh_targets(ctx);
    return 0;
}

static int dw_start_system(dw_ctx_t *ctx)
{
    /* Publish our channel under a name other processes can name_open().
       The channel is our own (fixed priority + disconnect notification);
       dispatch_create_channel() wraps it so name_attach() can use it. */
    dispatch_t *dpp = dispatch_create_channel(ctx->chid, DISPATCH_FLAG_NOLOCK);
    if (!dpp) {
        fprintf(stderr, "deadline-watch: dispatch_create_channel failed: %s\n",
                strerror(errno));
        return -1;
    }
    ctx->attach = name_attach(dpp, DW_SERVICE_NAME, 0);
    if (!ctx->attach) {
        fprintf(stderr, "deadline-watch: name_attach(\"%s\") failed: %s\n"
                "  Is another supervisor already running? (ls /dev/name/local)\n",
                DW_SERVICE_NAME, strerror(errno));
        return -1;
    }
    dw_refresh_targets(ctx);
    printf("deadline-watch: system mode, listening as /dev/name/local/%s "
           "(guard %s)\n", DW_SERVICE_NAME, ctx->cfg.guard_on ? "on" : "off");
    printf("deadline-watch: start clients now, e.g. ./dw-pump & ./dw-rogue\n");
    fflush(stdout);
    return 0;
}

static void dw_print_summary(const dw_ctx_t *ctx)
{
    printf("\n=== DeadlineWatch summary ===\n");
    printf("mode: %s, guard: %s, duration: %ds\n",
           ctx->cfg.mode == DW_MODE_SYSTEM ? "system" : "local",
           ctx->cfg.guard_on ? "on" : "off", ctx->cfg.duration_s);
    for (int i = 0; i < DW_MAX_TASKS; i++) {
        const dw_task_t *st = &ctx->core.task[i];
        if (!st->name[0])
            continue;
        /* Miss % is over cycles JUDGED (completed + starved/skipped),
           i.e. every period that elapsed, not just completed cycles. */
        long judged = st->expected_cycle;
        double miss_pct = judged > 0
            ? 100.0 * (double)st->misses / (double)judged : 0.0;
        long mean = st->cycles > 0 ? st->total_lateness_us / st->cycles : 0;
        printf("%s%s: %ld cycles judged (%ld completed), %ld misses (%.1f%%), "
               "longest miss streak %ld (%ld ms), worst lateness %ld us, "
               "mean %ld us\n",
               st->name, st->role == DW_ROLE_CRITICAL ? " [critical]" : "",
               judged, st->cycles, st->misses, miss_pct,
               st->max_consecutive, st->max_consecutive * st->period_ms,
               st->worst_lateness_us, mean);
    }
    printf("throttle events: %ld, offender final priority: %d, safe state: %s\n",
           ctx->core.throttle_events, ctx->rogue_prio_current,
           ctx->core.safe_state ? "YES" : "no");
    printf("log: %s\n", ctx->cfg.log_path);
}

int main(int argc, char **argv)
{
    static dw_ctx_t ctx;   /* static: large, and the signal handler needs it */
    memset(&ctx, 0, sizeof(ctx));
    ctx.chid = -1;
    ctx.coid = -1;
    ctx.control_ti = ctx.sensor_ti = -1;
    dw_config_defaults(&ctx.cfg);
    if (dw_config_parse_args(&ctx.cfg, argc, argv) != 0)
        return 2;

    /* Clamp priorities into the range the scheduler actually allows. */
    int pmin = sched_get_priority_min(SCHED_FIFO);
    int pmax = sched_get_priority_max(SCHED_FIFO);
    if (pmin == -1 || pmax == -1) {
        fprintf(stderr, "deadline-watch: cannot query priority range: %s\n",
                strerror(errno));
        return 1;
    }
    dw_clamp_prio(&ctx.cfg.supervisor_prio, pmin, pmax, "supervisor");
    dw_clamp_prio(&ctx.cfg.control_prio, pmin, pmax, "control");
    dw_clamp_prio(&ctx.cfg.sensor_prio, pmin, pmax, "sensor");
    dw_clamp_prio(&ctx.cfg.rogue_prio, pmin, pmax, "rogue");
    dw_clamp_prio(&ctx.cfg.rogue_demoted_prio, pmin, pmax, "rogue-demoted");
    dw_config_print(&ctx.cfg);

    /* SIGINT/SIGTERM end the run cleanly (summary + flushed log). The
       signals are blocked in every worker thread (they inherit this
       mask) and unblocked again in the supervisor below. */
    g_ctx = &ctx;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dw_on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, NULL);

    clock_gettime(CLOCK_MONOTONIC, &ctx.start);
    ctx.last_second = ctx.start;
    ctx.rogue_prio_current = ctx.cfg.rogue_prio;
    dw_core_init(&ctx.core, ctx.cfg.guard_on, ctx.cfg.safe_after_ms,
                 dw_log_core_cb, &ctx);
    dw_core_set_safe_miss_pct(&ctx.core, ctx.cfg.safe_miss_pct);

    if (dw_log_open(&ctx) != 0) {
        fprintf(stderr, "deadline-watch: cannot open log '%s': %s\n",
                ctx.cfg.log_path, strerror(errno));
        return 1;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    dw_log_row(&ctx, &now, "supervisor", "START", 0, 0, 0, "run begins");

    /* Fixed priority: a message from a priority-10 client must not drop
       the supervisor to 10 (default QNX behaviour is to inherit the
       sender's priority). System mode also asks the kernel to tell us
       when a client process goes away (_PULSE_CODE_DISCONNECT). */
    unsigned chflags = _NTO_CHF_FIXED_PRIORITY;
    chflags |= (ctx.cfg.mode == DW_MODE_SYSTEM) ? _NTO_CHF_DISCONNECT
                                                : _NTO_CHF_PRIVATE;
    ctx.chid = ChannelCreate(chflags);
    if (ctx.chid == -1) {
        fprintf(stderr, "deadline-watch: ChannelCreate failed: %s\n",
                strerror(errno));
        return 1;
    }
    ctx.coid = ConnectAttach(0, 0, ctx.chid, _NTO_SIDE_CHANNEL, 0);
    if (ctx.coid == -1) {
        fprintf(stderr, "deadline-watch: ConnectAttach failed: %s\n",
                strerror(errno));
        return 1;
    }

    /* The supervisor must be the highest-priority thread or an offender
       can starve the monitor itself. Real-time priorities need root. */
    struct sched_param mp;
    memset(&mp, 0, sizeof(mp));
    mp.sched_priority = ctx.cfg.supervisor_prio;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &mp);
    if (rc != 0) {
        fprintf(stderr,
            "deadline-watch: cannot set supervisor priority %d: %s\n"
            "  Run as root in the guest; real-time priorities are required.\n",
            ctx.cfg.supervisor_prio, strerror(rc));
        return 1;
    }

    /* 1 ms watchdog timer, delivered as a pulse on our own channel. */
    struct sigevent sev;
    SIGEV_PULSE_INIT(&sev, ctx.coid, ctx.cfg.supervisor_prio,
                     DW_PULSE_TIMER, 0);
    if (timer_create(CLOCK_MONOTONIC, &sev, &ctx.watchdog) != 0) {
        fprintf(stderr, "deadline-watch: timer_create failed: %s\n",
                strerror(errno));
        return 1;
    }
    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = DW_TIMER_PERIOD_MS * 1000000L;
    its.it_interval = its.it_value;
    if (timer_settime(ctx.watchdog, 0, &its, NULL) != 0) {
        fprintf(stderr, "deadline-watch: timer_settime failed: %s\n",
                strerror(errno));
        return 1;
    }

    int started = (ctx.cfg.mode == DW_MODE_SYSTEM) ? dw_start_system(&ctx)
                                                    : dw_start_local(&ctx);
    pthread_sigmask(SIG_UNBLOCK, &block, NULL);   /* supervisor takes the signals */
    rc = 1;
    if (started == 0)
        rc = dw_supervisor_run(&ctx);
    ctx.stop = 1;

    timer_delete(ctx.watchdog);
    if (ctx.cfg.mode == DW_MODE_LOCAL) {
        if (ctx.control_ti >= 0 && started == 0) {
            pthread_join(ctx.control_tid, NULL);
            pthread_join(ctx.sensor_tid, NULL);
            for (int i = 0; i < ctx.n_target; i++)
                pthread_join(ctx.target[i].ptid, NULL);
        }
        free(ctx.rogue_tids);
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    dw_log_row(&ctx, &now, "supervisor", "END", 0, 0, 0,
               rc == 0 ? "run ends" : "run aborted");
    dw_log_summary(&ctx);
    dw_log_close(&ctx);
    dw_print_summary(&ctx);

    if (ctx.attach)
        name_detach(ctx.attach, 0);
    ChannelDestroy(ctx.chid);
    return rc < 0 ? 1 : rc;
}
