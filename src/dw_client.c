/*
 * DeadlineWatch client library (see dw_client.h).
 * QNX APIs used: name_open() / name_close() (<sys/dispatch.h>) and
 * MsgSend() (<sys/neutrino.h>).
 */
#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE          /* QNX extensions: SIGEV_PULSE_INIT, _SC_NPROCESSORS_ONLN */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "dw_client.h"
#include "dw_core.h"   /* dw_add_ms / dw_diff_us (pure helpers) */

int dw_client_register(dw_client_t *c, const char *name, int kind,
                       int period_ms, int wait_ms)
{
    memset(c, 0, sizeof(*c));
    c->coid = -1;
    c->slot = -1;

    /* The supervisor may still be starting: retry every 100 ms. */
    for (int waited = 0; ; waited += 100) {
        c->coid = name_open(DW_SERVICE_NAME, 0);
        if (c->coid != -1)
            break;
        if (waited >= wait_ms)
            return -1;
        struct timespec ts = { 0, 100 * 1000000L };
        nanosleep(&ts, NULL);
    }

    dw_msg_t m;
    dw_reply_t r;
    memset(&m, 0, sizeof(m));
    memset(&r, 0, sizeof(r));
    m.type = DW_MSG_TYPE;
    m.req = DW_REQ_REGISTER;
    m.kind = kind;
    m.period_ms = period_ms;
    snprintf(m.name, sizeof(m.name), "%s", name);
    if (MsgSend(c->coid, &m, sizeof(m), &r, sizeof(r)) == -1) {
        int e = errno;
        name_close(c->coid);
        c->coid = -1;
        errno = e;
        return -1;
    }
    if (r.status != 0) {
        name_close(c->coid);
        c->coid = -1;
        errno = r.status;
        return -1;
    }
    c->slot = r.slot;
    c->kind = kind;
    c->period_ms = period_ms;
    c->epoch.tv_sec = (time_t)r.epoch_sec;
    c->epoch.tv_nsec = (long)r.epoch_nsec;
    return 0;
}

static int dw_client_send(dw_client_t *c, int req, long value)
{
    if (c->coid == -1)
        return -1;
    dw_msg_t m;
    dw_reply_t r;
    memset(&m, 0, sizeof(m));
    memset(&r, 0, sizeof(r));
    m.type = DW_MSG_TYPE;
    m.req = (uint16_t)req;
    m.slot = c->slot;
    m.value = value;
    if (MsgSend(c->coid, &m, sizeof(m), &r, sizeof(r)) == -1)
        return -1;
    if (r.status != 0)
        return -1;
    if (r.command == DW_CMD_SAFE_STATE)
        c->safe_state = 1;
    return c->safe_state ? DW_CMD_SAFE_STATE : DW_CMD_RUN;
}

int dw_client_beat(dw_client_t *c, long cycle)
{
    return dw_client_send(c, DW_REQ_BEAT, cycle);
}

int dw_client_progress(dw_client_t *c, long units)
{
    return dw_client_send(c, DW_REQ_PROGRESS, units);
}

long dw_client_wait_next(const dw_client_t *c, long cycle)
{
    struct timespec next, now;
    long k = cycle + 1;
    for (;;) {
        dw_add_ms(&c->epoch, k * (long)c->period_ms, &next);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (dw_diff_us(&now, &next) > 0)
            break;      /* next start is in the future */
        k++;            /* this cycle's start already passed: skip it */
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    return k;
}

void dw_client_close(dw_client_t *c)
{
    if (c->coid != -1) {
        dw_client_send(c, DW_REQ_UNREGISTER, 0);  /* clean exit, not LOST */
        name_close(c->coid);
    }
    c->coid = -1;
}
