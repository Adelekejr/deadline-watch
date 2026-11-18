/*
 * DeadlineWatch — configuration.
 *
 * Precedence: compiled-in defaults, then the config file
 * (config/deadline-watch.conf by default, or --config=PATH), then
 * command-line flags. A missing default config file is not an error.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dw.h"

void dw_config_defaults(dw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->control_period_ms    = 10;
    cfg->sensor_period_ms     = 50;
    cfg->supervisor_prio      = 30;
    cfg->control_prio         = 20;
    cfg->sensor_prio          = 15;
    cfg->rogue_prio           = 25;
    cfg->rogue_demoted_prio   = 5;
    cfg->rogue_duty_pct       = 85;
    cfg->rogue_work_window_ms = 10;
    cfg->rogue_threads        = 0;   /* 0 = one per online CPU */
    cfg->guard_on             = 1;
    cfg->rogue_on             = 1;
    cfg->duration_s           = 30;
    cfg->control_work_iters   = 200000;
    cfg->sensor_work_iters    = 50000;
    snprintf(cfg->log_path, sizeof(cfg->log_path), "deadline-watch.csv");
    snprintf(cfg->config_path, sizeof(cfg->config_path),
             "config/deadline-watch.conf");
}

static int dw_set_bool(const char *s, int *out)
{
    if (strcmp(s, "1") == 0 || strcmp(s, "on") == 0 ||
        strcmp(s, "true") == 0 || strcmp(s, "yes") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "off") == 0 ||
        strcmp(s, "false") == 0 || strcmp(s, "no") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int dw_set_int(dw_config_t *cfg, const char *key, const char *val)
{
    /* Non-numeric keys first: strtol() below would reject their values. */
    if (strcmp(key, "guard_on") == 0) return dw_set_bool(val, &cfg->guard_on);
    if (strcmp(key, "rogue_on") == 0) return dw_set_bool(val, &cfg->rogue_on);
    if (strcmp(key, "log_path") == 0) {
        snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", val);
        return 0;
    }

    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0')
        return -1;

    if (strcmp(key, "control_period_ms") == 0)    cfg->control_period_ms = (int)v;
    else if (strcmp(key, "sensor_period_ms") == 0) cfg->sensor_period_ms = (int)v;
    else if (strcmp(key, "supervisor_prio") == 0)  cfg->supervisor_prio = (int)v;
    else if (strcmp(key, "control_prio") == 0)     cfg->control_prio = (int)v;
    else if (strcmp(key, "sensor_prio") == 0)      cfg->sensor_prio = (int)v;
    else if (strcmp(key, "rogue_prio") == 0)       cfg->rogue_prio = (int)v;
    else if (strcmp(key, "rogue_demoted_prio") == 0) cfg->rogue_demoted_prio = (int)v;
    else if (strcmp(key, "rogue_duty_pct") == 0)   cfg->rogue_duty_pct = (int)v;
    else if (strcmp(key, "rogue_work_window_ms") == 0) cfg->rogue_work_window_ms = (int)v;
    else if (strcmp(key, "rogue_threads") == 0)    cfg->rogue_threads = (int)v;
    else if (strcmp(key, "duration_s") == 0)       cfg->duration_s = (int)v;
    else if (strcmp(key, "control_work_iters") == 0) cfg->control_work_iters = (int)v;
    else if (strcmp(key, "sensor_work_iters") == 0)  cfg->sensor_work_iters = (int)v;
    else
        return -1;
    return 0;
}

static char *dw_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e - 1))) *(--e) = '\0';
    return s;
}

int dw_config_load_file(dw_config_t *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *s = dw_trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "deadline-watch: %s:%d: ignoring line (no '='): %s\n",
                    path, lineno, s);
            continue;
        }
        *eq = '\0';
        char *key = dw_trim(s);
        char *val = dw_trim(eq + 1);
        if (dw_set_int(cfg, key, val) != 0)
            fprintf(stderr, "deadline-watch: %s:%d: unknown key '%s', ignoring\n",
                    path, lineno, key);
    }
    fclose(f);
    return 0;
}

static void dw_usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --config=PATH        config file (default: config/deadline-watch.conf)\n"
        "  --guard=on|off       enforce (demote rogue on 2 misses) or measure only\n"
        "  --rogue=on|off       start the rogue threads or run control-only\n"
        "  --duration=N         run length in seconds (default 30)\n"
        "  --rogue-duty=N       rogue burn %% of each 10 ms window (default 85)\n"
        "  --rogue-threads=N    rogue thread count, 0 = one per CPU (default 0)\n"
        "  --log=PATH           black-box log file (default deadline-watch.csv)\n"
        "  --help               this text\n",
        prog);
}

int dw_config_parse_args(dw_config_t *cfg, int argc, char **argv)
{
    const char *config_arg = NULL;

    /* First pass: --help and --config (the file loads before other flags). */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            dw_usage(argv[0]);
            exit(0);
        }
        if (strncmp(argv[i], "--config=", 9) == 0)
            config_arg = argv[i] + 9;
    }

    if (config_arg) {
        snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", config_arg);
        if (dw_config_load_file(cfg, cfg->config_path) != 0) {
            fprintf(stderr, "deadline-watch: cannot read config file '%s'\n",
                    cfg->config_path);
            return -1;
        }
    } else {
        /* Default file is optional. */
        dw_config_load_file(cfg, cfg->config_path);
    }

    /* Second pass: flags override the file. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--guard=", 8) == 0) {
            if (dw_set_bool(a + 8, &cfg->guard_on) != 0) { dw_usage(argv[0]); return -1; }
        } else if (strncmp(a, "--rogue=", 8) == 0) {
            if (dw_set_bool(a + 8, &cfg->rogue_on) != 0) { dw_usage(argv[0]); return -1; }
        } else if (strncmp(a, "--duration=", 11) == 0) {
            cfg->duration_s = (int)strtol(a + 11, NULL, 10);
        } else if (strncmp(a, "--rogue-duty=", 13) == 0) {
            cfg->rogue_duty_pct = (int)strtol(a + 13, NULL, 10);
        } else if (strncmp(a, "--rogue-threads=", 16) == 0) {
            cfg->rogue_threads = (int)strtol(a + 16, NULL, 10);
        } else if (strncmp(a, "--log=", 6) == 0) {
            snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", a + 6);
        } else if (strncmp(a, "--config=", 9) == 0) {
            /* handled in the first pass */
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            /* handled in the first pass */
        } else {
            fprintf(stderr, "deadline-watch: unknown option '%s'\n", a);
            dw_usage(argv[0]);
            return -1;
        }
    }

    /* Sanity clamps (tuned on the target; see README.md). */
    if (cfg->control_period_ms < 1) cfg->control_period_ms = 1;
    if (cfg->sensor_period_ms < 1)  cfg->sensor_period_ms = 1;
    if (cfg->rogue_duty_pct < 0)    cfg->rogue_duty_pct = 0;
    if (cfg->rogue_duty_pct > 100)  cfg->rogue_duty_pct = 100;
    if (cfg->duration_s < 1)        cfg->duration_s = 1;
    return 0;
}

void dw_config_print(const dw_config_t *cfg)
{
    printf("config: control %d ms @prio %d, sensor %d ms @prio %d, "
           "rogue %d%% duty @prio %d (demote to %d), supervisor @prio %d\n",
           cfg->control_period_ms, cfg->control_prio,
           cfg->sensor_period_ms, cfg->sensor_prio,
           cfg->rogue_duty_pct, cfg->rogue_prio, cfg->rogue_demoted_prio,
           cfg->supervisor_prio);
    printf("config: guard=%s rogue=%s duration=%ds log=%s\n",
           cfg->guard_on ? "on" : "off",
           cfg->rogue_on ? "on" : "off",
           cfg->duration_s, cfg->log_path);
}
