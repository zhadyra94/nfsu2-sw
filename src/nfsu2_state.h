/* nfsu2_state.h -- reads the game's race state from guest memory.
 * NFSU1 build (the file keeps the NFSU2 names). Found by reverse engineering
 * default.xbe; validated on Linux: menus, loading, countdown, driving, pause,
 * resume, Quick Race and Career. Single racy 32-bit reads, by design. */
#ifndef NFSU2_STATE_H
#define NFSU2_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = driving, 0 = menus / loading / paused, -1 = unknown (no guest memory,
 * race countdown, replay, a state not understood). */
int nfsu2_in_race(void);

/* The words behind the last answer. Any pointer may be NULL.
 * NFSU1: gameflow = GAMEFLOW_STATE word; mode = RCSTATE read through the race
 * controller pointer (0xFFFFFFFF when there is no valid object); flag = 1 when
 * that pointer is valid, else 0. Returns 0 and writes zeros when there is no
 * guest memory. */
int nfsu2_state_words(unsigned *gameflow, unsigned *mode, unsigned *flag);

/* Debounce for the automatic Pad/Drive layout. *state starts at 0 (zero
 * initialised is valid). Feed one nfsu2_in_race() reading per poll.
 * Returns the layout to use (0 Pad, 1 Drive), or -1 for "no change":
 *  - the same 0/1 reading on two consecutive polls returns that layout, and
 *    keeps returning it on every poll while the reading stays steady, so a
 *    caller that was blocked (editor open, touch off) can apply it later;
 *  - an unknown reading (-1 or junk) resets that streak. The 6th consecutive
 *    unknown reading (about 3 s at 0.5 s) returns 0 (Pad) and so does every
 *    further unknown one: a results screen that keeps gameflow 6 must not
 *    leave Drive up. Any 0/1 reading clears the unknown count and the
 *    normal 2-poll debounce applies again. */
int nfsu2_auto_layout_step(int *state, int reading);

#ifdef __cplusplus
}
#endif
#endif
