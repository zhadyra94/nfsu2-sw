/**
 * Xbox Input Compatibility Layer - iOS Touchscreen Gamepad
 *
 * Declares the touch virtual-pad layer that backs xbox_InputGetState on
 * iOS when no real (GameController) pad is attached. iOS has no keyboard
 * and no mouse, so without this layer a device with no Bluetooth pad
 * connected cannot even pass a press-start screen.
 *
 * The layer is event-driven: SDL's UIKit backend delivers SDL_FINGERDOWN /
 * FINGERMOTION / FINGERUP events whenever the runtime pumps events (the
 * renderers do that once per present), and an SDL event watch accumulates
 * them into a virtual controller. Reading is lock-protected because the
 * pump runs on the render thread while the game polls input from its own.
 *
 * Layout constants and the hit-test table live in xinput_ios.c; the zones
 * are exposed here so an on-screen overlay can draw them later.
 */

#ifndef NFSU2_XINPUT_IOS_H
#define NFSU2_XINPUT_IOS_H

#include <stddef.h>

#include "xinput_xbox.h"

/* What a zone drives. ZONE_FLAG sets a wButtons bit (ZONE's `value` is the
 * XBOX_GAMEPAD_* flag), ZONE_ANALOG presses an analog button (`value` is an
 * XBOX_BUTTON_* index, pressed = 255; the DRIVE pedals read lower when the
 * thumb slides down the pedal, see NFSU2_ANALOG_PEDALS), ZONE_DPAD maps the
 * touch's offset from the zone centre to D-pad flags (diagonals included),
 * ZONE_STEER (DRIVE only) is a wide strip: the finger's x offset from the
 * centre is the left stick X. A STEER zone is hit-tested as a RECTANGLE (rx /
 * ry are half-extents, not ellipse radii) and is only in the table while the
 * strip setting is on. */
#define XBOX_IOS_ZONE_FLAG   0
#define XBOX_IOS_ZONE_ANALOG 1
#define XBOX_IOS_ZONE_DPAD   2
#define XBOX_IOS_ZONE_STEER  3

/* One touchable zone of the on-screen pad, in coordinates normalized to
 * the window (0..1, origin top left). The radii describe an ellipse in
 * normalized space, which is round on screen only when rx / ry matches the
 * window aspect -- the table in xinput_ios.c is laid out for a landscape
 * phone. */
typedef struct XBOX_IOS_TOUCH_ZONE {
    int   id;      /* stable id, for a future overlay to key on */
    float cx, cy;  /* centre */
    float rx, ry;  /* radii */
    int   kind;    /* XBOX_IOS_ZONE_* */
    int   value;   /* wButtons flag or XBOX_BUTTON_* index (dpad: unused) */
} XBOX_IOS_TOUCH_ZONE;

/* Install the event hooks. Idempotent and safe to call from any thread;
 * returns 1 when the touch layer is live, 0 when disabled (RECOMP_TOUCH=0)
 * or its mutex could not be created -- callers simply retry on their next
 * poll. */
int xbox_ios_touch_init(void);

/* Build the state the touches currently imply. Fills *pState with zeros
 * where nothing is touched (a connected pad that presses nothing), bumps
 * the packet number, and returns 1. Returns 0 only when the layer is not
 * initialized; the caller then reports no device. */
int xbox_ios_touch_state(XBOX_INPUT_STATE *pState);

/* The active hit-test zones (the layout preset times the size setting), for
 * the on-screen overlay. Never NULL; *count receives the number of zones.
 * The table changes on set_layout / set_scale: re-read it after those. */
const XBOX_IOS_TOUCH_ZONE *xbox_ios_touch_zones(int *count);

/* Touch on/off (RECOMP_TOUCH). Off drops every finger, ignores new ones and
 * makes xbox_ios_touch_state() return 0. */
void xbox_ios_touch_set_enabled(int on);
int  xbox_ios_touch_enabled(void);

/* Layout preset (0 = PAD, 1 = DRIVE) and zone size factor (0.7..1.5). Both
 * clear all fingers. Main thread. */
void xbox_ios_touch_set_layout(int drive);
void xbox_ios_touch_set_scale(float k);

/* Drive feel switches (each also an env key read at the first use; the
 * setters are for the settings panel and the harness):
 *   analog pedals   NFSU2_ANALOG_PEDALS (1)  DRIVE GAS / BRAKE value from the
 *                   finger's height: 255 in the upper 60 % of the pedal, then
 *                   linearly down to 90 at the very bottom. 0 = flat 255.
 *   steering strip  NFSU2_STEER (1)          the STEER zone exists in DRIVE;
 *                   set_steer rebuilds the table and clears all fingers.
 *   strip gain      NFSU2_STEER_SENS (1.0)   0.5..2.0; full lock needs a
 *                   finger offset of half-width / sens from the centre.
 *   old DRIVE       NFSU2_DRIVE_CLASSIC (0)  1 = the pre-strip DRIVE table and
 *                   its A / B / X / Y / LB / RB labels (no STEER zone).
 * Main thread. */
void xbox_ios_touch_set_analog_pedals(int on);
void xbox_ios_touch_set_steer(int on);
void xbox_ios_touch_set_steer_sens(float k);
void xbox_ios_touch_set_drive_classic(int on);

/* Current analog value of a held zone for the overlay (idx in the active
 * table): 0 when no finger is on it. A DRIVE pedal with analog pedals on:
 * its level 0..1 (the byte the game reads / 255). The STEER zone: the signed
 * steer -1..1 (0 when idle). Any other held zone: 1. */
float xbox_ios_touch_zone_value(int idx);

/* Text the overlay draws on zone idx ("GAS", "A", ...), NULL for none. In
 * DRIVE (unless NFSU2_DRIVE_CLASSIC): GAS, BRAKE, NOS (B), E-BRAKE (A),
 * SHIFT^ / SHIFTv (X / Y, with the U+2191 / U+2193 arrows as UTF-8), LOOK
 * (Black), CAM (White). */
const char *xbox_ios_touch_zone_label(int idx);

/* Short function text for the overlay to draw under the zone's main glyph, or
 * NULL for none. PAD and classic DRIVE (the letters name buttons there): A
 * "E-BRAKE", B "NOS", X "SHIFT UP", Y "SHIFT DN", Black "LOOK", White "CAM",
 * LT "BRAKE", RT "GAS", START "PAUSE"; BACK, D-pad and STEER NULL. The new
 * DRIVE layout returns NULL for every zone (its labels already are the
 * function). The overlay skips captions on pedal-style controls. ASCII. */
const char *xbox_ios_touch_zone_caption(int idx);

/* SF Symbol name the overlay draws on zone idx instead of the text label, or
 * NULL (keep the text). Black "eye.fill", White "camera.fill", START
 * "pause.fill", BACK "arrow.uturn.backward" in every layout; in the new DRIVE
 * layout also A "exclamationmark.octagon.fill", B "flame.fill", X "arrow.up",
 * Y "arrow.down". PAD / classic faces, LT / RT, pedals, D-pad and STEER: NULL.
 * The overlay falls back to the text when a symbol does not load. */
const char *xbox_ios_touch_zone_icon(int idx);

/* Look of a zone, for the overlay (derived from the zone's kind / value and
 * the active layout: in DRIVE the LT / RT zones are pedals). Values of
 * XBOX_IOS_STYLE_*. ios_touch_overlay.m cannot include this header and
 * mirrors the numbers as plain ints: keep the two in step. */
enum {
    XBOX_IOS_STYLE_FACE_A = 0,
    XBOX_IOS_STYLE_FACE_B,
    XBOX_IOS_STYLE_FACE_X,
    XBOX_IOS_STYLE_FACE_Y,
    XBOX_IOS_STYLE_SHOULDER_BLACK,
    XBOX_IOS_STYLE_SHOULDER_WHITE,
    XBOX_IOS_STYLE_TRIGGER_L,
    XBOX_IOS_STYLE_TRIGGER_R,
    XBOX_IOS_STYLE_PEDAL_GAS,
    XBOX_IOS_STYLE_PEDAL_BRAKE,
    XBOX_IOS_STYLE_DPAD,
    XBOX_IOS_STYLE_MENU_BACK,
    XBOX_IOS_STYLE_MENU_START,
    XBOX_IOS_STYLE_STEER,
    XBOX_IOS_STYLE_OTHER
};
int xbox_ios_touch_zone_style(int idx);   /* OTHER when idx is out of range */

/* D-pad arms held through the D-pad zone (not the keyboard): OR of
 * XBOX_IOS_DPAD_BIT_*. */
#define XBOX_IOS_DPAD_BIT_UP    1u
#define XBOX_IOS_DPAD_BIT_DOWN  2u
#define XBOX_IOS_DPAD_BIT_LEFT  4u
#define XBOX_IOS_DPAD_BIT_RIGHT 8u
unsigned xbox_ios_touch_dpad_bits(void);

/* Changes every time the zone table is rebuilt (layout, size, geometry edits,
 * ...). */
unsigned xbox_ios_touch_layout_gen(void);

/* Active layout: 0 = PAD, 1 = DRIVE. */
int xbox_ios_touch_get_layout(void);

/* Per-zone geometry overrides (the layout editor). An override replaces one
 * zone's centre and radii in the preset, keyed by (layout PAD / DRIVE, zone
 * id); the size setting still scales it after that (a STEER zone keeps its
 * width, as in the preset). Overrides are stored UNSCALED ("base"). The
 * DRIVE overrides apply to the classic DRIVE table too (same ids 1..11).
 * Main thread; every change rebuilds the table and bumps layout_gen.
 *
 *   zone_geom       base geometry of zone idx of the ACTIVE table (the
 *                   override if one exists, else the preset), before the size
 *                   scale. Returns 0 for an index out of range. Any out
 *                   pointer may be NULL. Edit from these values, never from
 *                   xbox_ios_touch_zones(), which already includes the scale.
 *   zone_set_geom   sets the override of zone idx. Clamps: radii rx 0.02..0.30,
 *                   ry 0.04..0.45 (a STEER strip: rx up to 0.45, ry up to 0.19,
 *                   the height the table caps it at), then the centre so the
 *                   zone lies fully inside 0..1. Returns 0 for a bad index. A
 *                   call that changes nothing creates no override and does not
 *                   bump the generation.
 *   layout_reset    drops every override of that layout (0 PAD, 1 DRIVE).
 *   layout_format   writes the overrides as text (see below). Returns the
 *                   length the text needs, snprintf style: buf (n bytes, may be
 *                   NULL / 0) is always NUL-terminated and truncated if the
 *                   return value is >= n.
 *   layout_parse    REPLACES all overrides (both layouts) with the ones in
 *                   text; returns how many lines were applied. Lines that are
 *                   not "v1 ..." comments, garbage, unknown layouts or zone
 *                   ids and non-finite numbers are skipped; values are clamped
 *                   like zone_set_geom. NULL changes nothing and returns 0.
 *
 * Text: one header comment line, then one line per override, PAD first, ids
 * ascending, normalized base geometry with 4 decimals:
 *     # nfsu2 touch layout v1 ...
 *     v1 <pad|drive> <zone id> <cx> <cy> <rx> <ry>
 */
int  xbox_ios_touch_zone_geom(int idx, float *cx, float *cy, float *rx, float *ry);
int  xbox_ios_touch_zone_set_geom(int idx, float cx, float cy, float rx, float ry);
void xbox_ios_touch_layout_reset(int drive);
int  xbox_ios_touch_layout_format(char *buf, size_t n);
int  xbox_ios_touch_layout_parse(const char *text);

/* Called with the zone index when a finger lands on a zone, on whichever
 * thread pumps SDL events. NULL clears it. */
void xbox_ios_touch_set_press_hook(void (*hook)(int zone_idx));

/* Called when ANY finger goes down and is tracked (zone, steering strip or
 * dynamic stick), on the thread that pumps SDL events, outside the layer's
 * lock, after the finger is recorded and before the press hook. Not called
 * for a repeated down of a held finger, for a finger past the tracking limit
 * or while touch is off. The overlay wakes its display link with it. NULL
 * clears. */
void xbox_ios_touch_set_activity_hook(void (*hook)(void));

/* Tilt steering: x in -1..1 drives the left stick X while no finger holds
 * that stick; active 0 turns it off. */
void xbox_ios_touch_set_tilt(float x, int active);

/* Hardware keyboard: the action mask from SDL's key state (game thread), and
 * folding such a mask into *s. */
unsigned long long xbox_ios_keyboard_mask_from_sdl(void);
void xbox_ios_keyboard_fold(XBOX_INPUT_STATE *s, unsigned long long mask);

#endif /* NFSU2_XINPUT_IOS_H */
