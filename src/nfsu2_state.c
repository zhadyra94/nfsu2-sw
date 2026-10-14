/* nfsu2_state.c -- race state from guest memory. See nfsu2_state.h. */
#include <stddef.h>
#include <stdint.h>
#include "nfsu2_state.h"

extern ptrdiff_t g_xbox_mem_offset;

#define VA_GAMEFLOW 0x00428E34u   /* GAMEFLOW_STATE_* (0..9, 6 = RACING) */
#define VA_MODE     0x003E5A64u   /* 0 menus, 3 flyby, 4 driving, 6 paused */
#define VA_FLAG     0x003FBCFCu   /* 1 menus / paused, 0 driving */

static uint32_t word(uint32_t va)
{
    return *(volatile const uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
}

int nfsu2_state_words(unsigned *gameflow, unsigned *mode, unsigned *flag)
{
    unsigned g = 0, m = 0, f = 0;
    int ok = g_xbox_mem_offset != 0;

    if (ok) {
        g = word(VA_GAMEFLOW);
        m = word(VA_MODE);
        f = word(VA_FLAG);
    }
    if (gameflow) *gameflow = g;
    if (mode) *mode = m;
    if (flag) *flag = f;
    return ok;
}

int nfsu2_in_race(void)
{
    uint32_t gf, mode, flag;

    if (!g_xbox_mem_offset)
        return -1;
    gf = word(VA_GAMEFLOW);
    if (gf != 6)                      /* not RACING */
        return gf <= 9 ? 0 : -1;
    mode = word(VA_MODE);
    flag = word(VA_FLAG);
    if (mode == 4 && flag == 0)
        return 1;                     /* driving */
    if (mode == 6 && flag == 1)
        return 0;                     /* paused */
    return -1;                        /* flyby, transitions, unknown */
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
