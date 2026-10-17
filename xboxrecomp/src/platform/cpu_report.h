/*
 * cpu_report.h -- host thread names and a periodic "[cpu]" log line.
 *
 * Every RECOMP_CPU_LOG seconds one line on stderr shows where CPU time goes:
 * total % of a core, the busiest threads by name, wakeups per second, and the
 * thermal / low power / battery state the iOS overlay hands in. Plain C.
 * Apple: mach APIs. Linux: /proc. Switch and Windows: no-ops.
 */
#ifndef XBOX_CPU_REPORT_H
#define XBOX_CPU_REPORT_H

#ifdef _WIN32
/* The platform library is header-only on Windows: no-op stubs. */
static inline void xbox_thread_name(const char *name) { (void)name; }
static inline void xbox_thread_namef(const char *fmt, ...) { (void)fmt; }
static inline void cpu_report_start(void) {}
static inline void xbox_power_update(int thermal, int low_power, int battery_pct)
{ (void)thermal; (void)low_power; (void)battery_pct; }
static inline int xbox_power_thermal(void) { return -1; }
static inline int xbox_power_low_power(void) { return -1; }
static inline int xbox_power_hot_cap(void) { return 0; }
#else
#ifdef __cplusplus
extern "C" {
#endif

/* Names the CALLING thread (Linux: cut to 15 chars). Call it first in the
 * thread function. Do not call it on the Linux main thread: it would rename
 * the whole process in ps and top. */
void xbox_thread_name(const char *name);
void xbox_thread_namef(const char *fmt, ...);

/* Starts the reporter thread when RECOMP_CPU_LOG=<seconds> is above 0.
 * Default: 10 on iOS, 0 (off) elsewhere. Call once. */
void cpu_report_start(void);

/* The iOS overlay reports the device state. -1 = unknown. thermal: 0 nominal,
 * 1 fair, 2 serious, 3 critical. */
void xbox_power_update(int thermal, int low_power, int battery_pct);
int  xbox_power_thermal(void);      /* -1 unknown, 0..3 */
int  xbox_power_low_power(void);    /* -1 unknown, 0/1 */

/* 1 = run the 60 fps races at 30 fps now. Call once per swap from the game
 * thread. Key NFSU2_HOT_30: off | lpm | serious (default) | fair. It engages
 * at once and releases after RECOMP_HOT_HOLD_S seconds (default 60) without
 * the condition. Logs each change. Linux test source:
 * RECOMP_THERMAL_TEST="2@40,0@70" (thermal value @ seconds after the first
 * xbox_power_thermal() call). */
int  xbox_power_hot_cap(void);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */

#endif
