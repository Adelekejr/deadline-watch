/*
 * DeadlineWatch — supervisor (monitor + enforcement) and main().
 *
 * The supervisor is the main thread. It owns a QNX channel and receives:
 *   - completion pulses from the control loop and sensor (cycle index in
 *     the pulse value),
 *   - progress pulses from the rogue threads (100 windows each),
 *   - a 1 ms watchdog timer pulse.
 *
 * Deadline accounting: the deadline of cycle k of a thread with period P
 * is start + (k+1)*P. A cycle is a miss when its completion pulse arrives
 * after its deadline, or when the watchdog finds it overdue with no pulse
 * at all (starvation). Each cycle is counted at most once: the
 * per-thread `expected_cycle` only moves forward, and a cycle is judged
 * the moment it is passed.
 *
 * Enforcement (guard on): when the control loop misses its deadline
 * DW_MISS_THRESHOLD (2) times in a row, the supervisor demotes every
 * rogue thread to rogue_demoted_prio (below the control loop), so the
 * loop preempts the rogue and recovers. If it still misses, the rogue is
 * demoted to the minimum priority (escalation, once). The rogue stays
 * demoted for the rest of the run — no auto-restore in the MVP.
 * The "safe state" is: rogue demoted + a THROTTLE record in the log.
 *
 * NOTE: this is the QNX 8.0 design. Adaptive Partitioning does not exist
 * in QNX 8.0, so there is no kernel CPU budget to shrink — enforcement is
 * by strict-priority scheduling. See docs/design-8.0.md.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dw.h"

int64_t dw_diff_us(const struct timespec *a, const struct timespec *b)
{
    return (int64_t)(b->tv_sec - a->tv_sec) * 1000000L
         + (b->tv_nsec - a->tv_nsec) / 1000L;
}

void dw_add_ms(const struct timespec *base, long ms, struct timespec *out)
{
    out->tv_sec = base->tv_sec + ms / 1000;
    out->tv_nsec = base->tv_nsec + (ms % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += 1;
        out->tv_nsec -= 1000000000L;
    }
}

/* Deadline of cycle k of tracked thread ti: start + (k+1) * period. */
static void dw_deadline(const dw_ctx_t *ctx, int ti, long cycle,
                        struct timespec *out)
{
    dw_add_ms(&ctx->start, (cycle + 1) * ctx->track[ti].period_ms, out);
}

static void dw_demoted_rogues(dw_ctx_t *ctx, int prio, int *ok_out)
{
    int ok = 0;
    for (int i = 0; i < ctx->n_rogue; i++) {
        struct sched_param p;
        p.sched_priority = prio;
        if (pthread_setschedparam(ctx->rogue_tids[i], SCHED_FIFO, &p) == 0)
            ok++;
    }
    if (ok_out)
        *ok_out = ok;
}

static void dw_check_throttle(dw_ctx_t *ctx, const struct timespec *now)
{
    dw_thread_state_t *ctl = &ctx->track[DW_THREAD_CONTROL];
    if (!ctx->cfg.guard_on)
        return;
    if (ctx->n_rogue == 0)
        return;   /* no rogue threads: nothing to demote, no enforcement event */
    if (ctl->consecutive_misses < DW_MISS_THRESHOLD)
        return;

    if (!ctx->throttled) {
        int ok = 0;
        dw_demoted_rogues(ctx, ctx->cfg.rogue_demoted_prio, &ok);
        ctx->throttled = 1;
        ctx->throttle_events++;
        ctx->rogue_prio_current = ctx->cfg.rogue_demoted_prio;
        ctl->consecutive_misses = 0;   /* fresh count for the escalation check */
        ctl->clean_cycles = 0;
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "rogue demoted to prio %d (%d of %d threads)",
                 ctx->cfg.rogue_demoted_prio, ok, ctx->n_rogue);
        dw_log_row(ctx, now, "supervisor", "THROTTLE", ctl->expected_cycle,
                   0, ctl->worst_lateness_us, detail);
        return;
    }

    if (!ctx->escalated &&
        ctl->consecutive_misses >= DW_MISS_THRESHOLD * 2) {
        int minp = sched_get_priority_min(SCHED_FIFO);
        int ok = 0;
        dw_demoted_rogues(ctx, minp, &ok);
        ctx->escalated = 1;
        ctx->rogue_prio_current = minp;
        dw_log_row(ctx, now, "supervisor", "ESCALATE", ctl->expected_cycle,
                   0, ctl->worst_lateness_us,
                   "control loop still missing after demotion; "
                   "rogue dropped to minimum priority");
    }
}

/* Record one missed cycle (late pulse, skipped cycle, or starvation). */
static void dw_record_miss(dw_ctx_t *ctx, int ti, long cycle,
                           const struct timespec *deadline,
                           const struct timespec *now, const char *why)
{
    dw_thread_state_t *st = &ctx->track[ti];
    long late = dw_diff_us(deadline, now);
    st->misses++;
    st->consecutive_misses++;
    st->clean_cycles = 0;   /* a miss breaks the clean streak (same as a
                               late pulse in dw_report_cycle); without this
                               a starvation miss would not stop a RECOVER */
    if (late > st->worst_lateness_us)
        st->worst_lateness_us = late;
    dw_log_row(ctx, now, st->name, "MISS", cycle, late,
               st->worst_lateness_us, why);
    if (ti == DW_THREAD_CONTROL)
        dw_check_throttle(ctx, now);
}

static void dw_report_cycle(dw_ctx_t *ctx, int ti, long cycle,
                            const struct timespec *now)
{
    dw_thread_state_t *st = &ctx->track[ti];

    /* Count any cycles skipped before this one (starved or skipped by the
       thread). The watchdog usually catches them first; this is the net. */
    while (st->expected_cycle < cycle) {
        struct timespec d;
        dw_deadline(ctx, ti, st->expected_cycle, &d);
        dw_record_miss(ctx, ti, st->expected_cycle, &d, now, "skipped");
        st->expected_cycle++;
    }
    if (cycle < st->expected_cycle) {
        /* Stale pulse for a cycle already counted as a miss: do not
           count it again, and do not raise worst_lateness_us either.
           The summary's worst must be derivable from the logged rows;
           this pulse has no row, so its lateness must not leak into
           the summary (it used to, which made SUMMARY worst_us larger
           than the worst lateness in any log row). */
        return;
    }

    /* cycle == expected_cycle: the normal case. */
    struct timespec d;
    dw_deadline(ctx, ti, cycle, &d);
    long late = dw_diff_us(&d, now);
    st->cycles++;
    st->total_lateness_us += late;
    if (late > st->worst_lateness_us)
        st->worst_lateness_us = late;
    dw_log_row(ctx, now, st->name, "CYCLE", cycle, late,
               st->worst_lateness_us, "");

    if (late > 0) {
        st->misses++;
        st->consecutive_misses++;
        st->clean_cycles = 0;   /* a miss breaks the clean streak */
        if (ti == DW_THREAD_CONTROL)
            dw_check_throttle(ctx, now);
    } else {
        st->consecutive_misses = 0;
        if (ctx->throttled && ti == DW_THREAD_CONTROL) {
            st->clean_cycles++;
            if (st->clean_cycles >= DW_RECOVER_CYCLES && !ctx->recovered) {
                ctx->recovered = 1;
                dw_log_row(ctx, now, "supervisor", "RECOVER", cycle, 0,
                           st->worst_lateness_us,
                           "control loop clean for 50 cycles; "
                           "rogue stays demoted");
            }
        }
    }
    st->expected_cycle = cycle + 1;
}

static void dw_watchdog_tick(dw_ctx_t *ctx, const struct timespec *now)
{
    /* Starvation check: any cycle whose deadline passed (plus grace)
       with no completion pulse is a miss. */
    for (int ti = 0; ti < DW_NTRACKED; ti++) {
        dw_thread_state_t *st = &ctx->track[ti];
        for (;;) {
            struct timespec d, grace;
            dw_deadline(ctx, ti, st->expected_cycle, &d);
            dw_add_ms(&d, DW_WATCHDOG_GRACE_MS, &grace);
            if (dw_diff_us(&grace, now) <= 0)
                break;  /* not overdue yet */
            dw_record_miss(ctx, ti, st->expected_cycle, &d, now, "starved");
            st->expected_cycle++;
            if (st->expected_cycle > 1000000)
                break;  /* thread is dead; stop counting */
        }
    }

    /* Rogue throughput, once a second. */
    if (dw_diff_us(&ctx->last_rogue_log, now) >= 1000000L) {
        char detail[96];
        snprintf(detail, sizeof(detail), "rogue windows/s=%ld prio=%d",
                 ctx->rogue_progress, ctx->rogue_prio_current);
        ctx->rogue_windows_total += ctx->rogue_progress;
        dw_log_row(ctx, now, "rogue", "ROGUE", ctx->rogue_windows_total,
                   0, 0, detail);
        ctx->rogue_progress = 0;
        ctx->last_rogue_log = *now;
    }

    /* Run length. */
    if (dw_diff_us(&ctx->start, now) >=
        (int64_t)ctx->cfg.duration_s * 1000000L)
        ctx->stop = 1;
}

int dw_supervisor_run(dw_ctx_t *ctx)
{
    for (;;) {
        struct _pulse pulse;
        int rcvid = MsgReceive(ctx->chid, &pulse, sizeof(pulse), NULL);
        if (rcvid == -1) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "deadline-watch: MsgReceive failed: %s\n",
                    strerror(errno));
            return -1;
        }
        if (rcvid != 0) {
            /* No one sends messages; reply so the sender never blocks. */
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        switch (pulse.code) {
        case DW_PULSE_CONTROL_DONE:
            dw_report_cycle(ctx, DW_THREAD_CONTROL,
                            pulse.value.sival_int, &now);
            break;
        case DW_PULSE_SENSOR_DONE:
            dw_report_cycle(ctx, DW_THREAD_SENSOR,
                            pulse.value.sival_int, &now);
            break;
        case DW_PULSE_ROGUE_PROGRESS:
            ctx->rogue_progress += pulse.value.sival_int;
            break;
        case DW_PULSE_TIMER:
            dw_watchdog_tick(ctx, &now);
            break;
        default:
            break;  /* unknown pulse: ignore */
        }

        if (ctx->stop)
            return 0;
    }
}

static int dw_spawn(dw_ctx_t *ctx, pthread_t *tid,
                    void *(*fn)(void *), int prio, const char *name)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0 ||
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        fprintf(stderr, "deadline-watch: cannot set %s thread scheduling: %s\n",
                name, strerror(errno));
        pthread_attr_destroy(&attr);
        return -1;
    }
    struct sched_param param;
    param.sched_priority = prio;
    if (pthread_attr_setschedparam(&attr, &param) != 0) {
        /* A bad priority must fail loudly: silently running at the
           default priority would make the demo meaningless. */
        fprintf(stderr, "deadline-watch: cannot set %s thread priority %d: %s\n",
                name, prio, strerror(errno));
        pthread_attr_destroy(&attr);
        return -1;
    }
    int rc = pthread_create(tid, &attr, fn, ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "deadline-watch: cannot start %s thread: %s\n",
                name, strerror(rc));
        return -1;
    }
    return 0;
}

static int dw_clamp_prio(int *prio, int pmin, int pmax, const char *name)
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
    return 0;
}

int main(int argc, char **argv)
{
    dw_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
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

    clock_gettime(CLOCK_MONOTONIC, &ctx.start);
    ctx.last_rogue_log = ctx.start;
    ctx.rogue_prio_current = ctx.cfg.rogue_prio;

    if (dw_log_open(&ctx) != 0) {
        fprintf(stderr, "deadline-watch: cannot open log '%s': %s\n",
                ctx.cfg.log_path, strerror(errno));
        return 1;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    dw_log_row(&ctx, &now, "supervisor", "START", 0, 0, 0, "run begins");

    ctx.chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_PRIVATE);
    if (ctx.chid == -1) {
        fprintf(stderr, "deadline-watch: ChannelCreate failed: %s\n",
                strerror(errno));
        return 1;
    }
    ctx.coid = ConnectAttach(0, 0, ctx.chid,
                             _NTO_SIDE_CHANNEL, 0);
    if (ctx.coid == -1) {
        fprintf(stderr, "deadline-watch: ConnectAttach failed: %s\n",
                strerror(errno));
        return 1;
    }

    /* The supervisor must be the highest-priority thread or the rogue
       can starve the monitor itself. Real-time priorities need root in
       the guest; fail loudly instead of running a misleading demo. */
    struct sched_param mp;
    mp.sched_priority = ctx.cfg.supervisor_prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &mp) != 0) {
        fprintf(stderr,
            "deadline-watch: cannot set supervisor priority %d: %s\n"
            "  Run as root in the guest; real-time priorities are required.\n",
            ctx.cfg.supervisor_prio, strerror(errno));
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

    /* Tracked threads. */
    ctx.track[DW_THREAD_CONTROL].name = "control";
    ctx.track[DW_THREAD_CONTROL].period_ms = ctx.cfg.control_period_ms;
    ctx.track[DW_THREAD_SENSOR].name = "sensor";
    ctx.track[DW_THREAD_SENSOR].period_ms = ctx.cfg.sensor_period_ms;

    /* Rogue threads: one per online CPU unless configured otherwise, so
       the rogue can saturate every CPU the guest has (the QEMU default
       is 8). A single rogue thread could only hog one CPU. */
    int n_rogue = 0;
    if (ctx.cfg.rogue_on) {
        n_rogue = ctx.cfg.rogue_threads;
        if (n_rogue <= 0) {
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            n_rogue = (ncpu > 0 && ncpu <= 32) ? (int)ncpu : 1;
        }
        if (n_rogue > 32)
            n_rogue = 32;
    }
    ctx.n_rogue = n_rogue;
    if (n_rogue > 0) {
        ctx.rogue_tids = calloc((size_t)n_rogue, sizeof(pthread_t));
        if (!ctx.rogue_tids) {
            fprintf(stderr, "deadline-watch: out of memory\n");
            return 1;
        }
    }

    printf("deadline-watch: starting (%d rogue thread%s, guard %s)\n",
           n_rogue, n_rogue == 1 ? "" : "s",
           ctx.cfg.guard_on ? "on" : "off");

    if (dw_spawn(&ctx, &ctx.control_tid, dw_control_thread,
                 ctx.cfg.control_prio, "control") != 0)
        return 1;
    if (dw_spawn(&ctx, &ctx.sensor_tid, dw_sensor_thread,
                 ctx.cfg.sensor_prio, "sensor") != 0)
        return 1;
    for (int i = 0; i < n_rogue; i++) {
        if (dw_spawn(&ctx, &ctx.rogue_tids[i], dw_rogue_thread,
                     ctx.cfg.rogue_prio, "rogue") != 0)
            return 1;
    }

    int rc = dw_supervisor_run(&ctx);

    timer_delete(ctx.watchdog);
    pthread_join(ctx.control_tid, NULL);
    pthread_join(ctx.sensor_tid, NULL);
    for (int i = 0; i < n_rogue; i++)
        pthread_join(ctx.rogue_tids[i], NULL);
    free(ctx.rogue_tids);

    clock_gettime(CLOCK_MONOTONIC, &now);
    dw_log_row(&ctx, &now, "supervisor", "END", 0, 0, 0, "run ends");
    dw_log_summary(&ctx);
    dw_log_close(&ctx);

    /* Summary on stdout. */
    printf("\n=== DeadlineWatch summary ===\n");
    printf("guard: %s, rogue: %s (%d threads), duration: %ds\n",
           ctx.cfg.guard_on ? "on" : "off",
           ctx.cfg.rogue_on ? "on" : "off", n_rogue, ctx.cfg.duration_s);
    for (int i = 0; i < DW_NTRACKED; i++) {
        dw_thread_state_t *st = &ctx.track[i];
        double miss_pct = st->cycles > 0
            ? 100.0 * (double)st->misses / (double)st->cycles : 0.0;
        long mean = st->cycles > 0 ? st->total_lateness_us / st->cycles : 0;
        printf("%s: %ld cycles, %ld misses (%.1f%%), worst lateness %ld us, "
               "mean %ld us\n",
               st->name, st->cycles, st->misses, miss_pct,
               st->worst_lateness_us, mean);
    }
    printf("throttle events: %ld, rogue final priority: %d\n",
           ctx.throttle_events, ctx.rogue_prio_current);
    printf("log: %s\n", ctx.cfg.log_path);

    ChannelDestroy(ctx.chid);
    return rc;
}
