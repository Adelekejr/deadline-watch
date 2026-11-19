/*
 * DeadlineWatch — local-mode worker threads: control loop, sensor, rogue.
 *
 * The control loop and sensor are periodic threads. Each cycle they do a
 * fixed amount of stand-in work, then report completion to the supervisor
 * with a pulse carrying the cycle index. The supervisor — not the thread —
 * decides whether the cycle met its deadline (arrival vs. deadline), so
 * the measurement is independent of the thread being measured.
 *
 * If a thread is starved so long that the next period start is already
 * past, it skips ahead instead of running catch-up cycles: a skipped
 * cycle is a missed cycle, and the supervisor counts it as one.
 *
 * The rogue thread is a deliberately greedy CPU hog. It burns
 * rogue_duty_pct of each 10 ms window and sleeps the rest, at a priority
 * ABOVE the control loop — so with the guard off it starves the loop.
 * It reports progress every 100 windows so the supervisor (and the demo)
 * can show its throughput dropping when it is throttled.
 */
#include "dw.h"

#include <time.h>

/* Stand-in workload: a deterministic compute loop the compiler cannot
 * remove (volatile accumulator). This stands in for a real control
 * computation; the iteration count is a config knob (control_work_iters /
 * sensor_work_iters) to calibrate on the target so the control loop's
 * work takes ~1 ms. */
void dw_burn(int iters)
{
    volatile long x = 0;
    for (int i = 0; i < iters; i++)
        x += (long)(i * 2654435761L) >> 8;
    (void)x;
}

static void *dw_periodic_thread(dw_ctx_t *ctx, int period_ms, int work_iters,
                                int pulse_code, int prio)
{
    long cycle = 0;
    struct timespec next;
    int safe_reported = 0;

    while (!ctx->stop) {
        /* Safe state: in local mode there is no real actuator, so the
           stand-in "output" is simply held at zero and said once. The
           loop keeps reporting so the log shows whether it is healthy. */
        if (ctx->safe_state && !safe_reported &&
            pulse_code == DW_PULSE_CONTROL_DONE) {
            safe_reported = 1;
            printf("control: SAFE STATE commanded by supervisor; "
                   "output held at zero (in-process demo, no actuator)\n");
        }
        dw_burn(work_iters);
        MsgSendPulse(ctx->coid, prio, (int8_t)pulse_code, (int)cycle);
        cycle++;

        /* Sleep until the next cycle's start; skip cycles already past. */
        for (;;) {
            dw_add_ms(&ctx->start, cycle * period_ms, &next);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (dw_diff_us(&now, &next) > 0)
                break;  /* next start is in the future */
            cycle++;    /* this cycle already passed: skip it */
            if (ctx->stop)
                return NULL;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

void *dw_control_thread(void *arg)
{
    dw_ctx_t *ctx = arg;
    return dw_periodic_thread(ctx, ctx->cfg.control_period_ms,
                              ctx->cfg.control_work_iters,
                              DW_PULSE_CONTROL_DONE, ctx->cfg.control_prio);
}

void *dw_sensor_thread(void *arg)
{
    dw_ctx_t *ctx = arg;
    return dw_periodic_thread(ctx, ctx->cfg.sensor_period_ms,
                              ctx->cfg.sensor_work_iters,
                              DW_PULSE_SENSOR_DONE, ctx->cfg.sensor_prio);
}

void *dw_rogue_thread(void *arg)
{
    dw_ctx_t *ctx = arg;
    long windows = 0;
    long since_report = 0;

    while (!ctx->stop) {
        struct timespec wstart, now;
        clock_gettime(CLOCK_MONOTONIC, &wstart);

        /* Work in microseconds: with a 10 ms window, integer
           milliseconds would turn 85% into 8 ms (80%). */
        long window_us = (long)ctx->cfg.rogue_work_window_ms * 1000L;
        long burn_us = window_us * ctx->cfg.rogue_duty_pct / 100;

        /* Burn CPU for the duty share of the window. */
        do {
            dw_burn(1000);
            if (ctx->stop)
                return NULL;
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (dw_diff_us(&wstart, &now) < burn_us);

        /* Sleep the rest of the window. */
        long sleep_us = window_us - burn_us;
        if (sleep_us > 0) {
            struct timespec rem;
            rem.tv_sec = sleep_us / 1000000L;
            rem.tv_nsec = (sleep_us % 1000000L) * 1000L;
            nanosleep(&rem, NULL);
        }

        windows++;
        if (++since_report >= 100) {
            MsgSendPulse(ctx->coid, ctx->cfg.rogue_prio,
                         DW_PULSE_ROGUE_PROGRESS, 100);
            since_report = 0;
        }
    }
    (void)windows;
    return NULL;
}
