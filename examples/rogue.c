/*
 * dw-rogue — a SEPARATE misbehaving process for the cross-process demo.
 *
 * Plays a runaway best-effort job (think: a telemetry compressor or a UI
 * renderer stuck in a loop) that was mistakenly given a priority ABOVE
 * the pump's control loop. Each thread burns DUTY% of every 10 ms window.
 *
 * With --register=on (default) each thread registers with the supervisor
 * as DW_KIND_DEMOTABLE: in a real system that is the integrator's
 * contract ("best-effort processes may be demoted"). With --register=off
 * the process does not cooperate at all; the supervisor can still demote
 * it with `deadline-watch --demote-pid=<pid printed below>`.
 *
 * Usage (as root): ./dw-rogue [--threads=N] [--prio=25] [--duty=85]
 *                             [--register=on|off] [--duration=S]
 *                             [--start-delay=S]  (print pid, wait, then burn)
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
#include "../src/dw_core.h"

static int g_threads = 0, g_prio = 25, g_duty = 85, g_register = 1;
static int g_duration = 0, g_delay = 0;
static volatile int g_stop;

static void *rogue_thread(void *arg)
{
    long idx = (long)arg;
    dw_client_t c;
    int registered = 0;
    if (g_register) {
        char name[DW_NAME_LEN];
        snprintf(name, sizeof(name), "rogue-t%ld", idx);
        registered = dw_client_register(&c, name, DW_KIND_DEMOTABLE, 0, 5000) == 0;
        if (!registered)
            fprintf(stderr, "dw-rogue: thread %ld could not register: %s\n",
                    idx, strerror(errno));
    }
    const long window_us = 10000;
    const long burn_us = window_us * g_duty / 100;
    long since = 0;
    while (!g_stop) {
        struct timespec w, now;
        clock_gettime(CLOCK_MONOTONIC, &w);
        do {
            volatile long x = 0;
            for (int i = 0; i < 1000; i++) x += i;
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (dw_diff_us(&w, &now) < burn_us && !g_stop);
        long rest = window_us - burn_us;
        if (rest > 0) {
            struct timespec r = { rest / 1000000L, (rest % 1000000L) * 1000L };
            nanosleep(&r, NULL);
        }
        if (registered && ++since >= 100) {
            dw_client_progress(&c, since);
            since = 0;
        }
    }
    if (registered)
        dw_client_close(&c);
    return NULL;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--threads=", 10) == 0) g_threads = atoi(a + 10);
        else if (strncmp(a, "--prio=", 7) == 0) g_prio = atoi(a + 7);
        else if (strncmp(a, "--duty=", 7) == 0) g_duty = atoi(a + 7);
        else if (strncmp(a, "--duration=", 11) == 0) g_duration = atoi(a + 11);
        else if (strncmp(a, "--start-delay=", 14) == 0) g_delay = atoi(a + 14);
        else if (strcmp(a, "--register=on") == 0) g_register = 1;
        else if (strcmp(a, "--register=off") == 0) g_register = 0;
        else {
            fprintf(stderr, "usage: %s [--threads=N] [--prio=N] [--duty=PCT] "
                    "[--register=on|off] [--duration=S] [--start-delay=S]\n",
                    argv[0]);
            return 2;
        }
    }
    if (g_duty < 0) g_duty = 0;
    if (g_duty > 100) g_duty = 100;
    if (g_threads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g_threads = (n > 0 && n <= 32) ? (int)n : 1;
    }
    if (g_threads > 32) g_threads = 32;

    printf("dw-rogue: pid %d, %d threads @prio %d, %d%% duty, register=%s\n",
           (int)getpid(), g_threads, g_prio, g_duty, g_register ? "on" : "off");
    fflush(stdout);

    /* The main thread only sleeps and then stops the burners. Run it ABOVE
       them, or at 100 % duty it would never get the CPU to stop them. */
    {
        struct sched_param mp;
        memset(&mp, 0, sizeof(mp));
        mp.sched_priority = g_prio + 1;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &mp);
        if (rc != 0)
            fprintf(stderr, "dw-rogue: cannot raise main thread to prio %d: %s\n",
                    g_prio + 1, strerror(rc));
    }

    if (g_delay > 0)
        sleep((unsigned)g_delay);   /* lets a script pass our pid to --demote-pid */

    pthread_t t[32];
    int started = 0;
    for (long i = 0; i < g_threads; i++) {
        pthread_attr_t a;
        struct sched_param p;
        int rc;
        pthread_attr_init(&a);
        memset(&p, 0, sizeof(p));
        p.sched_priority = g_prio;
        if ((rc = pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED)) ||
            (rc = pthread_attr_setschedpolicy(&a, SCHED_FIFO)) ||
            (rc = pthread_attr_setschedparam(&a, &p)) ||
            (rc = pthread_create(&t[i], &a, rogue_thread, (void *)i))) {
            fprintf(stderr, "dw-rogue: cannot start thread at prio %d: %s "
                    "(run as root)\n", g_prio, strerror(rc));
            pthread_attr_destroy(&a);
            break;
        }
        pthread_attr_destroy(&a);
        started++;
    }
    if (started == 0)
        return 1;
    if (g_duration > 0) {
        sleep((unsigned)g_duration);
        g_stop = 1;
    }
    for (int i = 0; i < started; i++)
        pthread_join(t[i], NULL);
    return 0;
}
