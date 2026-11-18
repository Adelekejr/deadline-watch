/*
 * DeadlineWatch — black-box log (CSV).
 *
 * Single writer: the supervisor thread only. Worker threads never touch
 * the file; they report by pulse and the supervisor writes the row.
 *
 * Format (one row per line, fields comma-separated):
 *   t_ms,thread,event,cycle,lateness_us,worst_us,detail
 *
 * - t_ms: milliseconds since the run started (CLOCK_MONOTONIC).
 * - lateness_us is SIGNED: negative = finished before the deadline,
 *   positive = late. A cycle is a miss exactly when lateness_us > 0.
 * - CYCLE rows are written for every reported control/sensor cycle.
 * - MISS rows are written when the watchdog declares a cycle missed
 *   because no completion pulse arrived (starvation).
 * - THROTTLE / ESCALATE / RECOVER / ROGUE / START / END / SUMMARY are
 *   supervisor events; their `detail` field is free text (no commas).
 *
 * The host-side script host/render.py parses exactly this format.
 */
#include <errno.h>
#include <string.h>

#include "dw.h"

int dw_log_open(dw_ctx_t *ctx)
{
    ctx->log = fopen(ctx->cfg.log_path, "w");
    if (!ctx->log)
        return -1;

    fprintf(ctx->log,
        "# DeadlineWatch black-box log\n"
        "# format: t_ms,thread,event,cycle,lateness_us,worst_us,detail\n"
        "# lateness_us is signed: negative = before deadline, positive = late\n"
        "# a cycle is a miss exactly when lateness_us > 0\n"
        "# events: START, CYCLE, MISS, THROTTLE, ESCALATE, RECOVER, ROGUE, END, SUMMARY\n"
        "# guard=%s rogue=%s duration=%ds\n",
        ctx->cfg.guard_on ? "on" : "off",
        ctx->cfg.rogue_on ? "on" : "off",
        ctx->cfg.duration_s);
    fflush(ctx->log);
    return 0;
}

void dw_log_close(dw_ctx_t *ctx)
{
    if (ctx->log) {
        fflush(ctx->log);
        fclose(ctx->log);
        ctx->log = NULL;
    }
}

void dw_log_row(dw_ctx_t *ctx, const struct timespec *now,
                const char *thread, const char *event,
                long cycle, long lateness_us, long worst_us,
                const char *detail)
{
    if (!ctx->log)
        return;
    double t_ms = (double)dw_diff_us(&ctx->start, now) / 1000.0;
    if (fprintf(ctx->log, "%.3f,%s,%s,%ld,%ld,%ld,%s\n",
                t_ms, thread, event, cycle, lateness_us, worst_us,
                detail ? detail : "") < 0 && !ctx->log_failed) {
        /* A black-box log that silently drops rows is worse than no
           log: say so once instead of losing rows quietly (e.g. a
           full disk). Rows after this point are still attempted. */
        ctx->log_failed = 1;
        fprintf(stderr, "deadline-watch: failed to write to log '%s': %s\n",
                ctx->cfg.log_path, strerror(errno));
    }
    /* Flush on rare, important rows so a crash cannot lose them.
       CYCLE rows stay buffered (120/s); they flush on the next event
       row, the per-second ROGUE row, or at close. */
    if (strcmp(event, "CYCLE") != 0)
        fflush(ctx->log);
}

void dw_log_summary(dw_ctx_t *ctx)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < DW_NTRACKED; i++) {
        dw_thread_state_t *st = &ctx->track[i];
        char detail[128];
        double miss_pct = st->cycles > 0
            ? 100.0 * (double)st->misses / (double)st->cycles : 0.0;
        long mean = st->cycles > 0 ? st->total_lateness_us / st->cycles : 0;
        snprintf(detail, sizeof(detail),
                 "cycles=%ld misses=%ld miss_pct=%.1f worst_us=%ld mean_us=%ld",
                 st->cycles, st->misses, miss_pct, st->worst_lateness_us, mean);
        dw_log_row(ctx, &now, st->name, "SUMMARY", st->cycles, 0,
                   st->worst_lateness_us, detail);
    }
    char detail[128];
    snprintf(detail, sizeof(detail),
             "throttle_events=%ld rogue_final_prio=%d",
             ctx->throttle_events, ctx->rogue_prio_current);
    dw_log_row(ctx, &now, "supervisor", "SUMMARY", 0, 0, 0, detail);
}
