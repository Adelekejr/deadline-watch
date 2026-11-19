/*
 * DeadlineWatch client library — lets ANY QNX process be guarded by a
 * running `deadline-watch --mode=system` supervisor.
 *
 * Critical control loop (e.g. an infusion-pump dose loop):
 *
 *     dw_client_t c;
 *     dw_client_register(&c, "pump-dose", DW_KIND_CRITICAL, 10, 5000);
 *     for (long k = 0; ; ) {
 *         do_one_control_cycle();
 *         if (dw_client_beat(&c, k) == DW_CMD_SAFE_STATE)
 *             enter_safe_state();          // stop motor, latch alarm
 *         k = dw_client_wait_next(&c, k);  // sleep to the next period
 *     }
 *
 * Best-effort thread that the supervisor is allowed to demote:
 *
 *     dw_client_register(&c, "telemetry", DW_KIND_DEMOTABLE, 0, 5000);
 *
 * One dw_client_t per thread (the registration records the calling
 * thread's pid/tid). Link with src/dw_client.c.
 */
#ifndef DW_CLIENT_H
#define DW_CLIENT_H

#include <time.h>

#include "dw_proto.h"

typedef struct {
    int coid;                 /* connection to the supervisor (name_open) */
    int slot;
    int kind;
    int period_ms;
    struct timespec epoch;    /* cycle 0 start, from the supervisor */
    int safe_state;           /* latched once DW_CMD_SAFE_STATE is seen */
} dw_client_t;

/* Connect and register the CALLING thread. Retries name_open for up to
 * wait_ms (the supervisor may still be starting). Returns 0 or -1 with
 * errno set. */
int dw_client_register(dw_client_t *c, const char *name, int kind,
                       int period_ms, int wait_ms);

/* Report that cycle `cycle` is done. Returns DW_CMD_RUN,
 * DW_CMD_SAFE_STATE, or -1 if the supervisor is gone (treat that as a
 * reason to go to the safe state too). */
int dw_client_beat(dw_client_t *c, long cycle);

/* Report best-effort work units (demotable threads). Non-fatal. */
int dw_client_progress(dw_client_t *c, long units);

/* Sleep until the start of the next cycle after `cycle`, skipping any
 * cycles whose start already passed (a skipped cycle is a miss, and the
 * supervisor counts it). Returns the next cycle index to run. */
long dw_client_wait_next(const dw_client_t *c, long cycle);

/* Unregister (a clean exit, logged as UNREGISTER rather than LOST) and
 * disconnect. */
void dw_client_close(dw_client_t *c);

#endif /* DW_CLIENT_H */
