/* nfsu2_state.c -- race state from guest memory. See nfsu2_state.h. */
#include <stddef.h>
#include <stdint.h>
#include "nfsu2_state.h"

extern ptrdiff_t g_xbox_mem_offset;

/* NFSU1 (not NFSU2's addresses or numbering). Found in default.xbe:
 *  - GameFlowManager at 0x283418; its state word is 0x283434 (set by
 *    GameFlowBeginRacing, sub_0004F130, with 6). Enum order = the name table at
 *    0x242354: 0 NONE, 1 LOADING_FRONTEND, 2 UNLOADING_FRONTEND, 3 IN_FRONTEND,
 *    4 LOADING_TRACK_COMMON, 5 LOADING_TRACK_SPECIFIC, 6 RACING, 7/8 UNLOADING
 *    track, 9 EXIT_DEMO_DISC.
 *  - The race controller (RC) is an object allocated in the contiguous window;
 *    its pointer is the word 0x265B18 (0 outside races, set when gameflow
 *    becomes 6, cleared at unload). Its first word is the RCSTATE (state
 *    machine of sub_00031880; handler table 0x241A70, names 0x241AA0):
 *    0 NONE, 1 LOADING, 2 CAMERA_FLYBY, 3 PRERACE (countdown), 4
 *    RACE_IN_PROGRESS, 5 INSTANT_REPLAY, 6 AUTO_REPLAY, 7 PAUSE, 8 POSTRACE,
 *    9 AI_DEMO, 10 REPLAY_DEMO, 11 TUTORIAL.
 * Measured on Linux: 1 -> 3 -> 4 (drive) -> 7 (pause) -> 4 (resume) -> 8
 * (results after a finish or a blown engine, and the Restart/Statistics/Leave
 * menu) -> 1 (Restart). In Drag the RC is already 4 while the car waits. */
#define VA_GAMEFLOW   0x00283434u   /* GAMEFLOW_STATE_* (0..9, 6 = RACING) */
#define VA_RC_PTR     0x00265B18u   /* race controller object, 0 outside races */
#define RC_STATE_OFF  0u            /* RCSTATE_* at [object + 0] */

#define RCSTATE_DRIVING 4u          /* RACE_IN_PROGRESS */
#define RCSTATE_PAUSE   7u
#define RCSTATE_POSTRACE 8u         /* results screen and its Restart / Leave menu */

static uint32_t word(uint32_t va)
{
    return *(volatile const uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
}

/* The object pointer is a contiguous-window address (0x80000000..0x83FFFFFF)
 * or, on a build that allocates it from the heap, low RAM. The reader reads
 * it as the lifted code does (host base + address), but only inside those
 * ranges. */
static int rc_ptr_ok(uint32_t p)
{
    if (p >= 0x80001000u && p < 0x83FFFF00u)
        return 1;
    return p >= 0x00001000u && p < 0x03FFFF00u;
}

static int rc_state(uint32_t *state)
{
    uint32_t p = word(VA_RC_PTR);

    if (!rc_ptr_ok(p))
        return 0;
    *state = word(p + RC_STATE_OFF);
    return 1;
}

/* gameflow = GAMEFLOW_STATE; mode = RCSTATE (0xFFFFFFFF when no valid RC
 * object); flag = 1 when the RC object pointer is valid, else 0. */
int nfsu2_state_words(unsigned *gameflow, unsigned *mode, unsigned *flag)
{
    unsigned g = 0, m = 0, f = 0;
    int ok = g_xbox_mem_offset != 0;

    if (ok) {
        uint32_t st;

        g = word(VA_GAMEFLOW);
        f = rc_state(&st) ? 1u : 0u;
        m = f ? st : 0xFFFFFFFFu;
    }
    if (gameflow) *gameflow = g;
    if (mode) *mode = m;
    if (flag) *flag = f;
    return ok;
}

int nfsu2_in_race(void)
{
    uint32_t gf, st;

    if (!g_xbox_mem_offset)
        return -1;
    gf = word(VA_GAMEFLOW);
    if (gf != 6)                      /* not RACING */
        return gf <= 9 ? 0 : -1;
    if (!rc_state(&st))
        return -1;                    /* object not up yet / being freed */
    if (st == RCSTATE_DRIVING)
        return 1;                     /* driving */
    if (st == RCSTATE_PAUSE || st == RCSTATE_POSTRACE)
        return 0;                     /* paused / results: menus */
    return -1;                        /* loading, countdown, replay, unknown */
}

/* *state: bits 0-1 = streak of the last valid reading (0 none, 1 = 0, 2 = 1);
 * bits 2.. = consecutive unknown readings, saturating at AUTO_UNKNOWN_PAD. */
#define AUTO_UNKNOWN_PAD 6        /* ~3 s at the 0.5 s tick */

int nfsu2_auto_layout_step(int *state, int reading)
{
    int streak = *state & 3;
    int unknown = *state >> 2;

    if (reading == 0 || reading == 1) {
        int now = reading + 1;
        int steady = streak == now;

        *state = now;                 /* unknown counter back to 0 */
        return steady ? reading : -1;
    }
    if (unknown < AUTO_UNKNOWN_PAD)
        unknown++;
    *state = unknown << 2;            /* streak reset */
    return unknown >= AUTO_UNKNOWN_PAD ? 0 : -1;
}
