/*
 * cpu_report.c -- host thread names and the periodic "[cpu]" log line.
 * See cpu_report.h.
 *
 * The reporter is a detached thread. It samples the CPU time of every thread
 * of the process, keeps the last sample, and prints the difference. It never
 * calls guest code and never takes the guest lock.
 *
 *   [cpu] 10.0 s: 143% of a core; game 62%, nv2a-exec 41%, ..., others 8% (17 threads); wakeups 2300/s; thermal fair, ...
 *
 * Linux: /proc/self/task/<tid>/{stat,status}. Apple: task_threads() and
 * thread_info(). Both need only public APIs.
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "cpu_report.h"

#ifndef _WIN32

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

/* iOS detection, as in src/main.c. */
#if defined(__APPLE__) && (defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) || \
                           defined(__IPHONE_OS_VERSION_MIN_REQUIRED__))
#  define CPU_REPORT_IOS 1
#endif

/* ---- device state, written by the iOS overlay, read by the report ------ */

static volatile int s_thermal = -1;
static volatile int s_low_power = -1;
static volatile int s_battery = -1;

/* ---- thermal test source ----------------------------------------------- */

/* RECOMP_THERMAL_TEST="2@40,0@70" = thermal 2 from 40 s, thermal 0 from 70 s.
 * The seconds count from the first xbox_power_thermal() call (on Linux this
 * is the first swap: cpu_report_start does not run without RECOMP_CPU_LOG).
 * Before the first entry the value is -1. While it is set, the updates from
 * the overlay do not change the thermal value. */
#define TT_MAX 16
static struct { int val; double at_s; } s_tt[TT_MAX];
static int s_tt_n, s_tt_parsed;
static double s_tt_t0 = -1.0;

static double hc_now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void tt_parse(void)
{
    const char *e;
    char *end;

    if (s_tt_parsed)
        return;
    s_tt_parsed = 1;
    e = getenv("RECOMP_THERMAL_TEST");
    while (e && *e && s_tt_n < TT_MAX) {
        long v = strtol(e, &end, 10);
        double t;

        if (end == e || *end != '@')
            break;
        e = end + 1;
        t = strtod(e, &end);
        if (end == e)
            break;
        s_tt[s_tt_n].val = (int)v;
        s_tt[s_tt_n].at_s = t;
        s_tt_n++;
        e = end;
        if (*e != ',')
            break;
        e++;
    }
    if (s_tt_n > 0)
        fprintf(stderr, "[power] RECOMP_THERMAL_TEST: %d step(s) from %s, seconds since the first thermal read\n",
                s_tt_n, getenv("RECOMP_THERMAL_TEST"));
}

void xbox_power_update(int thermal, int low_power, int battery_pct)
{
    tt_parse();
    if (s_tt_n == 0)
        s_thermal = thermal;
    s_low_power = low_power;
    s_battery = battery_pct;
}

int xbox_power_thermal(void)
{
    tt_parse();
    if (s_tt_n > 0) {
        double now = hc_now_s();
        int i, v = -1;

        if (s_tt_t0 < 0.0)
            s_tt_t0 = now;
        for (i = 0; i < s_tt_n; i++)
            if (now - s_tt_t0 >= s_tt[i].at_s)
                v = s_tt[i].val;
        return v;
    }
    return s_thermal;
}

int xbox_power_low_power(void) { return s_low_power; }

/* ---- 30 fps when hot --------------------------------------------------- */

/* NFSU2_HOT_30: off | lpm | serious (default) | fair. Engage at once. Release
 * after the condition was false for RECOMP_HOT_HOLD_S seconds (default 60)
 * without a break. An unknown value (-1) counts as "not hot". */
enum { HC_OFF, HC_LPM, HC_SERIOUS, HC_FAIR };
static const char *const hc_names[] = { "off", "lpm", "serious", "fair" };

static const char *hc_thermal_name(int t)
{
    switch (t) {
    case 0:  return "nominal";
    case 1:  return "fair";
    case 2:  return "serious";
    case 3:  return "critical";
    default: return "unknown";
    }
}

static const char *hc_lpm_name(int l)
{
    return l < 0 ? "unknown" : l ? "on" : "off";
}

int xbox_power_hot_cap(void)
{
    static int policy = -1, engaged;
    static double hold_s, clear_since = -1.0;
    int th, lp, thermal_hot, lpm_hot;

    if (policy < 0) {
        const char *e = getenv("NFSU2_HOT_30");
        const char *h = getenv("RECOMP_HOT_HOLD_S");
        int i;

        policy = HC_SERIOUS;
        if (e && e[0]) {
            for (i = 0; i < 4; i++)
                if (strcmp(e, hc_names[i]) == 0)
                    policy = i;
        }
        hold_s = 60.0;
        if (h && h[0]) {
            double v = strtod(h, NULL);
            if (v >= 0.0 && v <= 3600.0)
                hold_s = v;
        }
        fprintf(stderr, "[power] 30 fps when hot: %s (NFSU2_HOT_30)\n", hc_names[policy]);
    }
    if (policy == HC_OFF)
        return 0;

    th = xbox_power_thermal();
    lp = xbox_power_low_power();
    lpm_hot = lp == 1;
    thermal_hot = (policy == HC_SERIOUS && th >= 2) || (policy == HC_FAIR && th >= 1);

    if (lpm_hot || thermal_hot) {
        clear_since = -1.0;
        if (!engaged) {
            engaged = 1;
            fprintf(stderr, "[power] hot (%s%s%s): 60 fps races -> 30 fps (NFSU2_HOT_30=%s)\n",
                    thermal_hot ? "thermal " : "", thermal_hot ? hc_thermal_name(th) : "",
                    lpm_hot ? (thermal_hot ? ", low power on" : "low power on") : "",
                    hc_names[policy]);
        }
    } else if (engaged) {
        double now = hc_now_s();

        if (clear_since < 0.0)
            clear_since = now;
        if (now - clear_since >= hold_s) {
            engaged = 0;
            clear_since = -1.0;
            fprintf(stderr, "[power] cool for %g s (thermal %s, low power %s): back to 60 fps\n",
                    hold_s, hc_thermal_name(th), hc_lpm_name(lp));
        }
    }
    return engaged;
}

#if defined(__APPLE__) || defined(__linux__)

/* ---- thread names ------------------------------------------------------ */

void xbox_thread_name(const char *name)
{
    char buf[16];

    if (!name)
        return;
#if defined(__APPLE__)
    (void)buf;
    pthread_setname_np(name);
#else
    /* Linux limit: 15 characters and the NUL. */
    strncpy(buf, name, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    pthread_setname_np(pthread_self(), buf);
#endif
}

void xbox_thread_namef(const char *fmt, ...)
{
    char buf[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    xbox_thread_name(buf);
}

/* ---- per-thread samples ------------------------------------------------ */

#define CR_MAX_THREADS 256

struct cr_thread {
    uint64_t id;        /* stable: Linux tid, Apple thread_id */
    uint64_t cpu_us;    /* user + system time so far */
    uint64_t vcs;       /* voluntary context switches so far (Linux) */
    char     name[32];  /* "" = unnamed */
};

struct cr_sample {
    struct cr_thread t[CR_MAX_THREADS];
    int      n;             /* entries stored (up to CR_MAX_THREADS) */
    int      total;         /* live threads, also those past the table */
    uint64_t task_wakeups;  /* Apple: interrupt + idle wakeups so far */
    int      have_task_wakeups;
};

static uint64_t cr_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

#if defined(__APPLE__)
#include <mach/mach.h>

static uint64_t cr_tv_us(time_value_t tv)
{
    return (uint64_t)(unsigned)tv.seconds * 1000000u + (uint64_t)(unsigned)tv.microseconds;
}

static void cr_sample_take(struct cr_sample *s)
{
    thread_act_array_t list = NULL;
    mach_msg_type_number_t n = 0, i;

    s->n = 0;
    s->total = 0;
    s->task_wakeups = 0;
    s->have_task_wakeups = 0;

#if defined(TASK_POWER_INFO) && defined(TASK_POWER_INFO_COUNT)
    {
        task_power_info_data_t pi;
        mach_msg_type_number_t cnt = TASK_POWER_INFO_COUNT;

        if (task_info(mach_task_self(), TASK_POWER_INFO, (task_info_t)&pi, &cnt) == KERN_SUCCESS) {
            s->task_wakeups = pi.task_interrupt_wakeups + pi.task_platform_idle_wakeups;
            s->have_task_wakeups = 1;
        }
    }
#endif

    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
        return;
    for (i = 0; i < n; i++) {
        thread_basic_info_data_t bi;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        struct cr_thread *t;
        pthread_t pt;

        if (thread_info(list[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS)
            continue;
#ifdef TH_FLAGS_IDLE
        if (bi.flags & TH_FLAGS_IDLE)
            continue;
#endif
        s->total++;
        if (s->n >= CR_MAX_THREADS)
            continue;
        t = &s->t[s->n++];
        t->cpu_us = cr_tv_us(bi.user_time) + cr_tv_us(bi.system_time);
        t->vcs = 0;
        t->name[0] = 0;
        t->id = list[i];     /* fallback: the mach port number */
#ifdef THREAD_IDENTIFIER_INFO
        {
            thread_identifier_info_data_t ii;
            mach_msg_type_number_t icnt = THREAD_IDENTIFIER_INFO_COUNT;

            if (thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &icnt) == KERN_SUCCESS)
                t->id = ii.thread_id;
        }
#endif
        pt = pthread_from_mach_thread_np(list[i]);
        if (pt)
            pthread_getname_np(pt, t->name, sizeof t->name);
    }
    for (i = 0; i < n; i++)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_t));
}

#else /* __linux__ */
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

/* Reads a small /proc file into buf (NUL-terminated). Returns the length or -1. */
static int cr_read_file(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    int len;

    if (fd < 0)
        return -1;
    len = (int)read(fd, buf, (size_t)cap - 1);
    close(fd);
    if (len < 0)
        return -1;
    buf[len] = 0;
    return len;
}

static void cr_sample_take(struct cr_sample *s)
{
    static long hz;
    static char main_comm[32];
    DIR *d;
    struct dirent *e;
    char path[64], buf[4096];

    s->n = 0;
    s->total = 0;
    s->task_wakeups = 0;
    s->have_task_wakeups = 0;
    if (!hz) {
        hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0)
            hz = 100;
    }
    /* A new thread inherits its creator's name until it sets its own, and the
     * first thread has the process name: a thread still called that is
     * unnamed. */
    main_comm[0] = 0;
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)getpid());
    if (cr_read_file(path, main_comm, sizeof main_comm) > 0)
        main_comm[strcspn(main_comm, "\n")] = 0;

    d = opendir("/proc/self/task");
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        struct cr_thread *t;
        char *p, *q;
        unsigned long long ut, st;
        int tid = atoi(e->d_name), field;

        if (tid <= 0)
            continue;
        snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
        if (cr_read_file(path, buf, sizeof buf) <= 0)
            continue;
        s->total++;
        if (s->n >= CR_MAX_THREADS)
            continue;
        t = &s->t[s->n];
        t->id = (uint64_t)tid;
        t->name[0] = 0;
        t->vcs = 0;
        /* "tid (comm) S ppid ...": comm may hold spaces and ')': it ends at
         * the LAST ')'. After it, field 3 (state) comes first; utime and
         * stime are fields 14 and 15. */
        p = strchr(buf, '(');
        q = strrchr(buf, ')');
        if (!p || !q || q < p)
            continue;
        {
            size_t nl = (size_t)(q - p - 1);

            if (nl < sizeof t->name &&
                !(nl == strlen(main_comm) && memcmp(p + 1, main_comm, nl) == 0)) {
                memcpy(t->name, p + 1, nl);
                t->name[nl] = 0;
            }
        }
        p = q + 1;
        ut = st = 0;
        for (field = 3; field <= 15; field++) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            if (field == 14)
                ut = strtoull(p, NULL, 10);
            else if (field == 15)
                st = strtoull(p, NULL, 10);
            while (*p && *p != ' ')
                p++;
        }
        t->cpu_us = (uint64_t)((ut + st) * 1000000ull / (unsigned long long)hz);

        snprintf(path, sizeof path, "/proc/self/task/%d/status", tid);
        if (cr_read_file(path, buf, sizeof buf) > 0) {
            p = strstr(buf, "\nvoluntary_ctxt_switches:");
            if (p)
                t->vcs = strtoull(p + 25, NULL, 10);
        }
        s->n++;
    }
    closedir(d);
}
#endif /* platform sampler */

/* ---- the report -------------------------------------------------------- */

struct cr_row {
    char     name[32];
    uint64_t cpu_us;
    uint64_t wakeups;
};

static struct cr_sample s_prev, s_cur;
static struct cr_row s_rows[CR_MAX_THREADS];

static int cr_row_cmp(const void *a, const void *b)
{
    const struct cr_row *x = a, *y = b;

    if (x->cpu_us != y->cpu_us)
        return x->cpu_us < y->cpu_us ? 1 : -1;
    if (x->wakeups != y->wakeups)
        return x->wakeups < y->wakeups ? 1 : -1;
    return 0;
}

/* Appends to buf; never writes past cap. */
static void cr_add(char *buf, size_t cap, const char *fmt, ...)
{
    size_t len = strlen(buf);
    va_list ap;

    if (len + 1 >= cap)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

static void cr_print(uint64_t window_us)
{
    static const char *const thermal_name[] = { "nominal", "fair", "serious", "critical" };
    char line[1024];
    uint64_t total_us = 0, others_us = 0, vcs_total = 0;
    int nrows = 0, i, j, shown, have_vcs = 0, th, lp, bat, any;
    double w = (double)window_us;

#ifdef __linux__
    have_vcs = 1;
#endif
    for (i = 0; i < s_cur.n; i++) {
        const struct cr_thread *c = &s_cur.t[i];
        uint64_t pcpu = 0, pvcs = 0;
        struct cr_row *r = &s_rows[nrows++];

        /* A thread not in the last sample is new: all its time is this
         * window's. */
        for (j = 0; j < s_prev.n; j++)
            if (s_prev.t[j].id == c->id) {
                pcpu = s_prev.t[j].cpu_us;
                pvcs = s_prev.t[j].vcs;
                break;
            }
        r->cpu_us = c->cpu_us >= pcpu ? c->cpu_us - pcpu : 0;
        r->wakeups = c->vcs >= pvcs ? c->vcs - pvcs : 0;
        if (c->name[0])
            snprintf(r->name, sizeof r->name, "%s", c->name);
        else
            snprintf(r->name, sizeof r->name, "t%llu",
#if defined(__APPLE__)
                     (unsigned long long)(c->id & 0xFFFFu));   /* thread_id is long: low 16 bits */
#else
                     (unsigned long long)c->id);               /* the tid, as ps -L shows it */
#endif
        total_us += r->cpu_us;
        vcs_total += r->wakeups;
    }
    qsort(s_rows, (size_t)nrows, sizeof s_rows[0], cr_row_cmp);

    line[0] = 0;
    cr_add(line, sizeof line, "[cpu] %.1f s: %.0f%% of a core; ", w / 1e6,
           w > 0 ? 100.0 * (double)total_us / w : 0.0);
    shown = 0;
    for (i = 0; i < nrows && shown < 8; i++) {
        if (!s_rows[i].cpu_us && !s_rows[i].wakeups)
            break;                  /* sorted: the rest are idle */
        cr_add(line, sizeof line, "%s %.0f%%", s_rows[i].name,
               w > 0 ? 100.0 * (double)s_rows[i].cpu_us / w : 0.0);
        if (have_vcs)
            cr_add(line, sizeof line, " %.0f/s", w > 0 ? (double)s_rows[i].wakeups * 1e6 / w : 0.0);
        cr_add(line, sizeof line, ", ");
        shown++;
    }
    for (j = shown; j < nrows; j++)
        others_us += s_rows[j].cpu_us;
    cr_add(line, sizeof line, "others %.0f%% (%d threads)",
           w > 0 ? 100.0 * (double)others_us / w : 0.0, s_cur.total);

    if (have_vcs)
        cr_add(line, sizeof line, "; wakeups %.0f/s", w > 0 ? (double)vcs_total * 1e6 / w : 0.0);
    else if (s_cur.have_task_wakeups && s_prev.have_task_wakeups) {
        uint64_t dw = s_cur.task_wakeups >= s_prev.task_wakeups
                          ? s_cur.task_wakeups - s_prev.task_wakeups : 0;
        cr_add(line, sizeof line, "; wakeups %.0f/s", w > 0 ? (double)dw * 1e6 / w : 0.0);
    }

    th = s_thermal;
    lp = s_low_power;
    bat = s_battery;
    any = 0;
    if (th >= 0 && th <= 3) {
        cr_add(line, sizeof line, "; thermal %s", thermal_name[th]);
        any = 1;
    }
    if (lp >= 0) {
        cr_add(line, sizeof line, "%slow power %s", any ? ", " : "; ", lp ? "on" : "off");
        any = 1;
    }
    if (bat >= 0)
        cr_add(line, sizeof line, "%sbattery %d%%", any ? ", " : "; ", bat);
    fprintf(stderr, "%s\n", line);
}

static void *cr_thread_main(void *arg)
{
    unsigned period = (unsigned)(uintptr_t)arg;
    uint64_t t0;

    xbox_thread_name("cpu-report");
    cr_sample_take(&s_prev);        /* silent baseline */
    t0 = cr_now_us();
    for (;;) {
        struct timespec ts;
        uint64_t t1;

        ts.tv_sec = (time_t)period;
        ts.tv_nsec = 0;
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
            ;
        cr_sample_take(&s_cur);
        t1 = cr_now_us();
        cr_print(t1 - t0);
        s_prev = s_cur;
        t0 = t1;
    }
    return NULL;
}

void cpu_report_start(void)
{
    static int started;
    const char *e = getenv("RECOMP_CPU_LOG");
    long period =
#ifdef CPU_REPORT_IOS
        10;
#else
        0;
#endif
    pthread_t th;

    if (started)
        return;
    if (e && e[0])
        period = strtol(e, NULL, 10);
    if (period <= 0 || period > 3600)
        return;
    started = 1;
    if (pthread_create(&th, NULL, cr_thread_main, (void *)(uintptr_t)period) == 0)
        pthread_detach(th);
    else
        started = 0;
}

#else /* neither Apple nor Linux (Switch): no-ops */

void xbox_thread_name(const char *name) { (void)name; }
void xbox_thread_namef(const char *fmt, ...) { (void)fmt; }
void cpu_report_start(void) {}

#endif /* __APPLE__ || __linux__ */

#endif /* !_WIN32 */
