/*
 * dw-pump — SIMULATED syringe infusion pump guarded by DeadlineWatch.
 *
 * NOT a medical device and not medical software: a teaching model whose
 * only purpose is to make "a missed deadline" concrete. There is no real
 * motor; the "plunger" is a variable.
 *
 * Threads (all SCHED_FIFO, need root):
 *   pump-dose     10 ms CRITICAL loop. Each cycle it compares the volume
 *                 that should have been delivered by now with what was
 *                 delivered, and commands the stepper motor (simulated)
 *                 with a per-cycle speed limit, like a real pump. A
 *                 starved cycle moves no fluid, and the speed limit
 *                 means the lag is only recovered slowly: missed
 *                 deadlines become UNDER-DOSING you can read off the
 *                 screen.
 *   pump-pressure 50 ms TRACKED loop: simulated line-pressure sensor.
 *   main          normal priority: prints one status line per second.
 *
 * Safe state (what real infusion pumps do on an internal fault): stop
 * the motor, latch an alarm, keep reporting. Entered when the
 * supervisor's BEAT reply says DW_CMD_SAFE_STATE, or when the
 * supervisor itself is unreachable (fail-safe).
 *
 * Usage (in the QNX guest, as root, after `deadline-watch --mode=system &`):
 *   ./dw-pump [--rate=ML_PER_H] [--duration=S] [--prio=N] [--work=ITERS]
 */
#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE          /* QNX extensions: SIGEV_PULSE_INIT, _SC_NPROCESSORS_ONLN */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/dw_client.h"

#define STEP_ML         0.0000333   /* fluid per motor step (simulated) */
#define DOSE_PERIOD_MS  10
#define PRESS_PERIOD_MS 50

static double g_rate_ml_h = 120.0;
static int g_prio = 20, g_press_prio = 15, g_duration = 30;
static int g_work = 200000;

static volatile double g_delivered_ml, g_target_ml, g_pressure_kpa;
static volatile int g_alarm;          /* 0 none, 1 supervisor safe state, 2 supervisor lost */
static volatile int g_stop;

static void burn(int iters)            /* stand-in for real control maths */
{
    volatile long x = 0;
    for (int i = 0; i < iters; i++)
        x += (long)(i * 2654435761L) >> 8;
    (void)x;
}

static void enter_safe_state(int why)
{
    if (!g_alarm) {
        g_alarm = why;
        /* Real pump: de-energise the motor driver, close the line clamp,
           sound the alarm. Here: the dose loop stops commanding steps. */
    }
}

static void *dose_thread(void *arg)
{
    (void)arg;
    dw_client_t c;
    if (dw_client_register(&c, "pump-dose", DW_KIND_CRITICAL,
                           DOSE_PERIOD_MS, 5000) != 0) {
        fprintf(stderr, "dw-pump: cannot register with deadline-watch: %s\n"
                "  start `deadline-watch --mode=system` first\n",
                strerror(errno));
        g_stop = 1;
        return NULL;
    }
    double rate_ml_ms = g_rate_ml_h / 3600000.0;
    /* Speed limit: 120% of the nominal steps per cycle. */
    double nominal = rate_ml_ms * DOSE_PERIOD_MS / STEP_ML;
    long max_steps = (long)(nominal * 1.2) + 1;

    for (long k = 0; !g_stop; ) {
        burn(g_work);
        double t_ms = (double)(k + 1) * DOSE_PERIOD_MS;
        double target = rate_ml_ms * t_ms;
        g_target_ml = target;
        if (!g_alarm) {
            long steps = (long)((target - g_delivered_ml) / STEP_ML);
            if (steps > max_steps) steps = max_steps;
            if (steps < 0) steps = 0;
            g_delivered_ml += steps * STEP_ML;
        }
        int cmd = dw_client_beat(&c, k);
        if (cmd == DW_CMD_SAFE_STATE)
            enter_safe_state(1);
        else if (cmd < 0)
            enter_safe_state(2);      /* fail-safe: the guard is gone */
        k = dw_client_wait_next(&c, k);
    }
    dw_client_close(&c);
    return NULL;
}

static void *pressure_thread(void *arg)
{
    (void)arg;
    dw_client_t c;
    if (dw_client_register(&c, "pump-pressure", DW_KIND_TRACKED,
                           PRESS_PERIOD_MS, 5000) != 0)
        return NULL;
    unsigned seed = 7;
    for (long k = 0; !g_stop; ) {
        burn(g_work / 4);
        seed = seed * 1103515245u + 12345u;
        g_pressure_kpa = 20.0 + (double)(seed >> 16 & 0xff) / 64.0;
        if (dw_client_beat(&c, k) < 0)
            break;
        k = dw_client_wait_next(&c, k);
    }
    dw_client_close(&c);
    return NULL;
}

static int spawn(pthread_t *t, void *(*fn)(void *), int prio)
{
    pthread_attr_t a;
    struct sched_param p;
    int rc;
    pthread_attr_init(&a);
    memset(&p, 0, sizeof(p));
    p.sched_priority = prio;
    if ((rc = pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED)) ||
        (rc = pthread_attr_setschedpolicy(&a, SCHED_FIFO)) ||
        (rc = pthread_attr_setschedparam(&a, &p)) ||
        (rc = pthread_create(t, &a, fn, NULL))) {
        fprintf(stderr, "dw-pump: cannot start thread at prio %d: %s "
                "(run as root)\n", prio, strerror(rc));
        pthread_attr_destroy(&a);
        return -1;
    }
    pthread_attr_destroy(&a);
    return 0;
}

static int arg_int(const char *a, const char *key, int *out)
{
    size_t n = strlen(key);
    if (strncmp(a, key, n) != 0)
        return 0;
    *out = atoi(a + n);
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        int r;
        if (strncmp(argv[i], "--rate=", 7) == 0) g_rate_ml_h = atof(argv[i] + 7);
        else if (arg_int(argv[i], "--duration=", &g_duration)) ;
        else if (arg_int(argv[i], "--prio=", &g_prio)) ;
        else if (arg_int(argv[i], "--work=", &g_work)) ;
        else if (arg_int(argv[i], "--pressure-prio=", &r)) g_press_prio = r;
        else {
            fprintf(stderr, "usage: %s [--rate=ML_PER_H] [--duration=S] "
                    "[--prio=N] [--pressure-prio=N] [--work=ITERS]\n", argv[0]);
            return 2;
        }
    }
    if (g_rate_ml_h <= 0) g_rate_ml_h = 120.0;
    printf("dw-pump (SIMULATION, not a medical device): pid %d, rate %.1f mL/h, "
           "dose loop %d ms @prio %d\n", (int)getpid(), g_rate_ml_h,
           DOSE_PERIOD_MS, g_prio);

    pthread_t td, tp;
    if (spawn(&td, dose_thread, g_prio) != 0)
        return 1;
    if (spawn(&tp, pressure_thread, g_press_prio) != 0) {
        g_stop = 1;
        pthread_join(td, NULL);
        return 1;
    }

    /* Status printer: normal priority on purpose (like a UI). When the
       CPU is hogged above it, lines go missing; the timestamp is real
       elapsed time, so gaps are visible. */
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        sleep(1);
        clock_gettime(CLOCK_MONOTONIC, &tn);
        double el = (double)(tn.tv_sec - t0.tv_sec) +
                    (double)(tn.tv_nsec - t0.tv_nsec) / 1e9;
        if (g_stop || (g_duration > 0 && el >= g_duration + 0.5))
            break;
        double tgt = g_target_ml, got = g_delivered_ml;
        double err = tgt > 0 ? 100.0 * (got - tgt) / tgt : 0.0;
        printf("[pump %5.1fs] prescribed %7.4f mL  delivered %7.4f mL  "
               "dose error %+6.2f%%  line %4.1f kPa  %s\n",
               el, tgt, got, err, g_pressure_kpa,
               g_alarm == 1 ? "ALARM: SAFE STATE (motor stopped by guard)"
               : g_alarm == 2 ? "ALARM: guard unreachable, motor stopped"
               : "infusing");
        fflush(stdout);
    }
    g_stop = 1;
    pthread_join(td, NULL);
    pthread_join(tp, NULL);
    return g_alarm ? 3 : 0;
}
