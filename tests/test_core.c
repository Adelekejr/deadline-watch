/*
 * Host unit tests for the deadline policy (src/dw_core.c).
 * Runs on any POSIX host: dw_core.c has no QNX calls. Time is simulated
 * (we pass `now` explicitly), so the results are deterministic.
 *
 *   make host-check      (or: cc -Isrc tests/test_core.c src/dw_core.c)
 */
#include <stdio.h>
#include <string.h>

#include "dw_core.h"

static int fails, checks;
static char last_event[32];
static int n_rows, n_miss_rows, n_recover_rows, n_lost_rows;

#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

static void log_cb(void *u, const struct timespec *now, const char *who,
                   const char *ev, long cyc, long late, long worst,
                   const char *detail)
{
    (void)u; (void)now; (void)who; (void)cyc; (void)late; (void)worst; (void)detail;
    snprintf(last_event, sizeof(last_event), "%s", ev);
    n_rows++;
    if (!strcmp(ev, "MISS")) n_miss_rows++;
    if (!strcmp(ev, "RECOVER")) n_recover_rows++;
    if (!strcmp(ev, "LOST")) n_lost_rows++;
}

static struct timespec at_ms(const struct timespec *base, double ms)
{
    struct timespec t;
    dw_add_ms(base, (long)ms, &t);
    long extra_ns = (long)((ms - (long)ms) * 1e6);
    t.tv_nsec += extra_ns;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return t;
}

static void reset_counts(void) { n_rows = n_miss_rows = n_recover_rows = n_lost_rows = 0; }

static void test_time_helpers(void)
{
    struct timespec a = { 10, 999000000L }, b;
    dw_add_ms(&a, 2, &b);
    CHECK(b.tv_sec == 11 && b.tv_nsec == 1000000L, "dw_add_ms carries into seconds");
    CHECK(dw_diff_us(&a, &b) == 2000, "dw_diff_us is b - a in microseconds");
}

static void test_on_time(void)
{
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 1, 300, log_cb, NULL);
    c.targets = 1;
    int ti = dw_core_add_task(&c, "ctl", DW_ROLE_CRITICAL, 10, &e);
    int acts = 0;
    for (long k = 0; k < 100; k++) {
        struct timespec now = at_ms(&e, k * 10.0 + 5.0);  /* 5 ms early */
        acts |= dw_core_report(&c, ti, k, &now);
        acts |= dw_core_tick(&c, &now);
    }
    CHECK(c.task[ti].cycles == 100, "100 on-time cycles counted");
    CHECK(c.task[ti].misses == 0, "no misses when every cycle is early");
    CHECK(acts == 0, "no enforcement when healthy");
    /* worst_lateness_us starts at 0 and only records lateness above it
       (same as the original supervisor): early cycles leave it at 0. */
    CHECK(c.task[ti].worst_lateness_us == 0, "worst lateness stays 0 when never late");
}

static void test_late_and_stale(void)
{
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 1, 0, log_cb, NULL);
    int ti = dw_core_add_task(&c, "ctl", DW_ROLE_CRITICAL, 10, &e);
    struct timespec now = at_ms(&e, 11.0);           /* cycle 0 deadline is 10 ms */
    dw_core_report(&c, ti, 0, &now);
    CHECK(c.task[ti].misses == 1, "late completion is one miss");
    CHECK(c.task[ti].worst_lateness_us == 1000, "lateness 1 ms");
    /* Starve cycle 1 (deadline 20 ms) past grace (2 ms). */
    now = at_ms(&e, 23.0);
    dw_core_tick(&c, &now);
    CHECK(c.task[ti].misses == 2, "starved cycle counted by the watchdog");
    CHECK(n_miss_rows == 1, "one MISS row for the starved cycle");
    /* The stale report for cycle 1 must not double count or raise worst. */
    long worst = c.task[ti].worst_lateness_us;
    now = at_ms(&e, 40.0);
    dw_core_report(&c, ti, 1, &now);
    CHECK(c.task[ti].misses == 2, "stale report not counted again");
    CHECK(c.task[ti].worst_lateness_us == worst, "stale report does not raise worst");
    /* Skipped cycles: report cycle 5 directly -> cycles 2,3,4 are misses. */
    now = at_ms(&e, 55.0);
    dw_core_report(&c, ti, 5, &now);
    CHECK(c.task[ti].misses == 2 + 3, "skipped cycles 2..4 counted as misses");
    CHECK(c.task[ti].expected_cycle == 6, "expected_cycle moves past the report");
}

static void test_ladder(void)
{
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 1, 300, log_cb, NULL);
    c.targets = 4;
    int ti = dw_core_add_task(&c, "ctl", DW_ROLE_CRITICAL, 10, &e);
    CHECK(ti == 0, "first task takes slot 0");
    int seen_throttle = -1, seen_escalate = -1, seen_safe = -1;
    /* Starve the task completely; tick every 1 ms for 600 ms. */
    for (int ms = 1; ms <= 600; ms++) {
        struct timespec now = at_ms(&e, ms);
        int a = dw_core_tick(&c, &now);
        if ((a & DW_ACT_THROTTLE) && seen_throttle < 0) seen_throttle = ms;
        if ((a & DW_ACT_ESCALATE) && seen_escalate < 0) seen_escalate = ms;
        if ((a & DW_ACT_SAFE_STATE) && seen_safe < 0) seen_safe = ms;
    }
    /* A starved cycle k is judged on the first tick strictly after its
       deadline + grace, i.e. at 10(k+1) + 2 + 1 ms.
       2nd miss (cycle 1) at 23 ms -> THROTTLE (count resets to 0).
       4 more consecutive (cycles 2..5) -> ESCALATE at 63 ms.
       30 consecutive misses since the throttle (= 300 ms at 10 ms,
       cycles 2..31) -> SAFE_STATE at 323 ms. */
    CHECK(seen_throttle == 23, "THROTTLE on the 2nd consecutive miss (23 ms)");
    CHECK(seen_escalate == 63, "ESCALATE after 4 more misses (63 ms)");
    CHECK(seen_safe == 323, "SAFE_STATE after 300 ms of failure post-escalation");
    CHECK(c.throttle_events == 1, "one throttle event");
    CHECK(c.safe_state == 1, "safe state latched");
    /* Latched: nothing more happens. */
    struct timespec now = at_ms(&e, 700);
    CHECK(dw_core_tick(&c, &now) == 0, "no further actions after safe state");
}

static void test_no_targets(void)
{
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 1, 100, log_cb, NULL);
    c.targets = 0;   /* nothing to demote */
    int ti = dw_core_add_task(&c, "ctl", DW_ROLE_CRITICAL, 10, &e);
    int acts = 0, safe_ms = -1;
    for (int ms = 1; ms <= 200; ms++) {
        struct timespec now = at_ms(&e, ms);
        int a = dw_core_tick(&c, &now);
        acts |= a;
        if ((a & DW_ACT_SAFE_STATE) && safe_ms < 0) safe_ms = ms;
    }
    CHECK(!(acts & (DW_ACT_THROTTLE | DW_ACT_ESCALATE)),
          "no THROTTLE/ESCALATE when there is nothing to demote");
    /* 10 consecutive misses = 100 ms; cycle 9 judged at 103 ms. */
    CHECK(safe_ms == 103, "with nothing to demote, safe state after safe_after_ms (100 ms)");
    (void)ti;
}

static void test_guard_off_and_tracked(void)
{
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 0, 300, log_cb, NULL);       /* guard off */
    c.targets = 4;
    int ti = dw_core_add_task(&c, "ctl", DW_ROLE_CRITICAL, 10, &e);
    int acts = 0;
    for (int ms = 1; ms <= 1000; ms++) {
        struct timespec now = at_ms(&e, ms);
        acts |= dw_core_tick(&c, &now);
    }
    CHECK(acts == 0, "guard off: measure only, never act");
    CHECK(c.task[ti].misses > 90, "guard off: misses still counted");

    dw_core_init(&c, 1, 300, log_cb, NULL);       /* guard on, tracked only */
    c.targets = 4;
    dw_core_add_task(&c, "sensor", DW_ROLE_TRACKED, 50, &e);
    acts = 0;
    for (int ms = 1; ms <= 1000; ms++) {
        struct timespec now = at_ms(&e, ms);
        acts |= dw_core_tick(&c, &now);
    }
    CHECK(acts == 0, "tracked (non-critical) task never triggers enforcement");
}

static void test_recover(void)
{
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 1, 300, log_cb, NULL);
    c.targets = 1;
    int ti = dw_core_add_task(&c, "ctl", DW_ROLE_CRITICAL, 10, &e);
    struct timespec now = at_ms(&e, 12.0);
    dw_core_report(&c, ti, 0, &now);              /* late */
    now = at_ms(&e, 22.0);
    int a = dw_core_report(&c, ti, 1, &now);       /* late -> throttle */
    CHECK(a & DW_ACT_THROTTLE, "two late completions throttle");
    for (long k = 2; k < 2 + 49; k++) {
        now = at_ms(&e, k * 10.0 + 1.0);
        dw_core_report(&c, ti, k, &now);
    }
    CHECK(n_recover_rows == 0, "no RECOVER before 50 clean cycles");
    now = at_ms(&e, 51 * 10.0 + 1.0);
    dw_core_report(&c, ti, 51, &now);
    CHECK(n_recover_rows == 1, "RECOVER after 50 clean cycles");
    CHECK(strcmp(last_event, "RECOVER") == 0, "RECOVER is the last row");
}

static void test_lost(void)
{
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 1, 300, log_cb, NULL);
    int crit = dw_core_add_task(&c, "pump-dose", DW_ROLE_CRITICAL, 10, &e);
    int trk = dw_core_add_task(&c, "pump-pressure", DW_ROLE_TRACKED, 50, &e);
    struct timespec now = at_ms(&e, 5);
    CHECK(dw_core_lost(&c, trk, &now) == 0, "losing a tracked task is not a safe-state event");
    CHECK(dw_core_lost(&c, crit, &now) & DW_ACT_SAFE_STATE, "losing the critical loop -> safe state");
    CHECK(n_lost_rows == 2, "LOST rows logged");
    int ti3 = dw_core_add_task(&c, "new", DW_ROLE_CRITICAL, 10, &e);
    CHECK(ti3 != crit && ti3 != trk, "slots are not reused (log names stay unique)");
    CHECK(dw_core_add_task(&c, "bad", DW_ROLE_CRITICAL, 0, &e) == -1, "period 0 rejected");
}

static void test_miss_rate(void)
{
    /* Intermittent starvation (every other cycle missed) never reaches
       300 ms of CONTINUOUS failure, but a 50 % miss-rate limit catches
       it once enforcement is exhausted. */
    dw_core_t c; struct timespec e = { 100, 0 };
    reset_counts();
    dw_core_init(&c, 1, 300, log_cb, NULL);
    dw_core_set_safe_miss_pct(&c, 50);
    c.targets = 0;                      /* offender cannot be demoted */
    int ti = dw_core_add_task(&c, "pump-dose", DW_ROLE_CRITICAL, 10, &e);
    int acts = 0;
    for (long k = 0; k < 300; k++) {
        struct timespec now = at_ms(&e, k * 10.0 + (k % 2 ? 12.0 : 5.0));
        acts |= dw_core_report(&c, ti, k, &now);
    }
    CHECK(acts & DW_ACT_SAFE_STATE, "50% miss rate in a 1 s window -> safe state");
    CHECK(c.safe_reason && strcmp(c.safe_reason, "miss-rate") == 0, "reason is miss-rate");

    dw_core_init(&c, 1, 300, log_cb, NULL);   /* miss-rate criterion off */
    c.targets = 0;
    ti = dw_core_add_task(&c, "pump-dose", DW_ROLE_CRITICAL, 10, &e);
    acts = 0;
    for (long k = 0; k < 300; k++) {
        struct timespec now = at_ms(&e, k * 10.0 + (k % 2 ? 12.0 : 5.0));
        acts |= dw_core_report(&c, ti, k, &now);
    }
    CHECK(!(acts & DW_ACT_SAFE_STATE), "without the miss-rate limit, alternating misses never trip safe state");

    dw_core_init(&c, 1, 0, log_cb, NULL);     /* both criteria off */
    ti = dw_core_add_task(&c, "pump-dose", DW_ROLE_CRITICAL, 10, &e);
    struct timespec now = at_ms(&e, 5);
    CHECK(dw_core_lost(&c, ti, &now) == 0, "safe state disabled entirely -> lost is only logged");
}

int main(void)
{
    test_time_helpers();
    test_on_time();
    test_late_and_stale();
    test_ladder();
    test_no_targets();
    test_guard_off_and_tracked();
    test_recover();
    test_lost();
    test_miss_rate();
    printf("dw_core: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
