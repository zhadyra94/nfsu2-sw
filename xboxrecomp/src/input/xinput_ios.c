/*
 * xinput_ios.c -- the touchscreen as the Xbox gamepad (iOS only).
 *
 * A phone has no keys and no mouse; a device without a Bluetooth pad must
 * still get past "Press START". This layer turns touches into the virtual
 * controller that answers for port 0 whenever no real one is attached (see
 * xbox_InputGetState: a pad that is attached never reaches it, so a real
 * pad always outranks the screen).
 *
 * The sticks are dynamic, the way most mobile ports do it: nothing is on
 * screen for them, a touch that lands on the bare half of the screen
 * becomes that stick's centre, and the deflection is the offset from there,
 * so the thumb never has to find a fixed circle first. Everything else is
 * a fixed zone (xinput_ios.h's XBOX_IOS_TOUCH_ZONE): a touch that lands in
 * one presses it and holds it until the finger lifts, wherever the finger
 * drifts meanwhile -- re-testing every motion would drop a button the
 * moment a thumb slides half a zone over, which reads as a missed press.
 *
 * Events, not polling: SDL's UIKit backend queues SDL_FINGER* events as
 * the touches happen, and the renderers pump SDL's event queue once per
 * present (d3d8_gl.c's dev_Present, nv2a_vk.c's vk_flip). An SDL event
 * watch sees everything the pump drains, discarded or not, so no renderer
 * had to change. The watch also follows window resizes -- touch positions
 * are normalized to the window, and stick deflection needs real pixels,
 * so the height/width ratio is kept from SDL_WINDOWEVENT_SIZE_CHANGED
 * (16:9 until the first one arrives; iOS reports the window as it is
 * created, well before the title's first poll in practice).
 *
 * DRIVE adds two things on top of the zones: pedals (GAS / BRAKE are a full
 * press wherever the thumb is; NFSU2_ANALOG_PEDALS=1 opts into analog ones that
 * read lower as the thumb slides down the pedal) and a
 * steering strip (a zone whose finger x is the left stick X, relative to the
 * touch-down point by default; NFSU2_STEER / NFSU2_STEER_SENS /
 * NFSU2_STEER_MODE / NFSU2_STEER_CURVE). Both, the old DRIVE table
 * (NFSU2_DRIVE_CLASSIC) and the activity hook for the overlay are described in
 * xinput_ios.h.
 *
 * Screen adaptation (overlay-fed): a safe-area band the zones are mapped into
 * (Dynamic Island / notch), and, on a window whose aspect is not the phone's,
 * round controls kept round plus a tablet size factor. See xinput_ios.h.
 *
 * RECOMP_TOUCH=0 turns the layer off, e.g. to prove a Bluetooth pad works
 * with the touch code out of the way. The settings panel flips it live
 * (xbox_ios_touch_set_enabled), and also picks the zone layout (PAD, or
 * DRIVE for racing), the zone size, haptics and tilt steering.
 *
 * Kept out of xinput_device.c the same way xinput_nx.c is kept out: this
 * file is compiled only for iOS, and everything here is meaningless
 * elsewhere. Apple clang rejects nested functions, so every callback is a
 * file-scope static.
 */
#if defined(__APPLE__) && (defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) || \
                           defined(__IPHONE_OS_VERSION_MIN_REQUIRED__))
#define NFSU2_IOS 1
#endif

#ifdef NFSU2_IOS

#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xinput_xbox.h"
#include "xinput_ios.h"

/* ------------------------------------------------------------------ */
/* The on-screen pad                                                   */
/* ------------------------------------------------------------------ */

/* Coordinates normalized to the window, laid out on an 852x393 landscape
 * phone screen (radii are a circle of n pixels: rx = n/852, ry = n/393).
 * On a portrait window the same numbers still hit-test, just squashed --
 * zones are relative to the window, not the device orientation.
 *
 *   BACK                          START
 *
 *        LB                  RB
 *
 *                    Y
 *  (left half:   X       B    (right half: right stick,
 *   left stick)      A     drag = deflection)
 *     dpad
 */
static const XBOX_IOS_TOUCH_ZONE s_zones_pad[] = {
    /* id   cx      cy      rx      ry      kind               value            */
    /* Shoulders ABOVE triggers, both near the top edge; clear of the
     * Dynamic Island (x 11-48 pt, y 133-260 pt in landscape) and of the
     * d-pad/stick areas. Numerically checked on 852x393. */
    {  1,  0.056f, 0.102f, 0.035f, 0.076f, XBOX_IOS_ZONE_FLAG,   XBOX_GAMEPAD_BACK  },
    {  2,  0.944f, 0.102f, 0.035f, 0.076f, XBOX_IOS_ZONE_FLAG,   XBOX_GAMEPAD_START },
    {  4,  0.165f, 0.130f, 0.042f, 0.091f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_BLACK    },  /* LB */
    {  6,  0.835f, 0.130f, 0.042f, 0.091f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_WHITE    },  /* RB */
    {  5,  0.165f, 0.370f, 0.048f, 0.104f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_LTRIGGER },  /* LT */
    {  7,  0.835f, 0.370f, 0.048f, 0.104f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_RTRIGGER },  /* RT */
    /* ABXY diamond, right-bottom; centres >=0.065 apart, rx 0.038. */
    {  8,  0.840f, 0.860f, 0.038f, 0.082f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_A      },
    {  9,  0.905f, 0.770f, 0.038f, 0.082f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_B      },
    { 10,  0.775f, 0.770f, 0.038f, 0.082f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_X      },
    { 11,  0.840f, 0.680f, 0.038f, 0.082f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_Y      },
    {  3,  0.080f, 0.810f, 0.058f, 0.126f, XBOX_IOS_ZONE_DPAD,   0                  },
};
#define PAD_COUNT ((int)(sizeof s_zones_pad / sizeof s_zones_pad[0]))

/* DRIVE preset (racing), laid out by function: the right thumb works the
 * pedals and the boost, the left thumb steers.
 *
 *   BACK  dpad           LOOK  CAM        START
 *                                          SHIFT up
 *                                          SHIFT down
 *                  E-BRAKE    NOS
 *   steering       BRAKE            GAS
 *   strip          BRAKE            GAS
 *
 * GAS (RT) is a tall pedal in the bottom-right corner, BRAKE (LT) left of it,
 * NOS (B) above GAS, E-BRAKE (A) above BRAKE, SHIFT up / down (X / Y) a small
 * pair on the right edge, LOOK (Black) and CAM (White) small near the top
 * right, BACK / START in the top corners, a small d-pad (menus, free roam)
 * top left. The steering strip (XBOX_IOS_ZONE_STEER, a RECTANGLE: rx / ry are
 * half-extents, see zone_at) sits in the lower left, x 0.06-0.28 (51-239 pt,
 * 187 pt wide on 852 pt): the touch-down point inside it is "straight ahead"
 * (relative mode), so it only has to be big enough to land a thumb on. The
 * rest of the left half is still the dynamic left stick, so menus keep working
 * by stick.
 *
 * Checked numerically on 852x393 (harness case "DRIVE geometry"): no two
 * zones overlap, the strip starts at x = 51 pt so it clears the Dynamic Island
 * strip (x 11-48 pt, y 133-260 pt), every main control is at least 60 pt
 * across. */
static const XBOX_IOS_TOUCH_ZONE s_zones_drive[] = {
    /* id   cx      cy      rx      ry      kind               value            */
    {  1,  0.056f, 0.102f, 0.035f, 0.076f, XBOX_IOS_ZONE_FLAG,   XBOX_GAMEPAD_BACK  },
    {  2,  0.944f, 0.102f, 0.035f, 0.076f, XBOX_IOS_ZONE_FLAG,   XBOX_GAMEPAD_START },
    {  4,  0.765f, 0.100f, 0.034f, 0.073f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_BLACK    },  /* LOOK */
    {  6,  0.855f, 0.100f, 0.034f, 0.073f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_WHITE    },  /* CAM */
    {  5,  0.725f, 0.740f, 0.055f, 0.185f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_LTRIGGER },  /* BRAKE */
    {  7,  0.905f, 0.715f, 0.068f, 0.215f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_RTRIGGER },  /* GAS */
    {  8,  0.725f, 0.370f, 0.046f, 0.088f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_A      },  /* E-BRAKE */
    {  9,  0.860f, 0.375f, 0.036f, 0.078f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_B      },  /* NOS */
    { 10,  0.955f, 0.272f, 0.032f, 0.066f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_X      },  /* SHIFT up */
    { 11,  0.955f, 0.425f, 0.032f, 0.066f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_Y      },  /* SHIFT down */
    {  3,  0.190f, 0.300f, 0.040f, 0.087f, XBOX_IOS_ZONE_DPAD,   0                  },
    { 12,  0.170f, 0.790f, 0.110f, 0.170f, XBOX_IOS_ZONE_STEER,  0                  },
};
#define DRIVE_NEW_COUNT ((int)(sizeof s_zones_drive / sizeof s_zones_drive[0]))

/* NFSU2_DRIVE_CLASSIC=1: the DRIVE preset as it was before the strip (pedals
 * bottom right, face diamond above them, LB / RB small, left half = dynamic
 * stick, no STEER zone, game-agnostic A / B / X / Y / LB / RB labels). Checked
 * on 852x393: no overlaps, clear of the Dynamic Island strip. */
static const XBOX_IOS_TOUCH_ZONE s_zones_drive_classic[] = {
    /* id   cx      cy      rx      ry      kind               value            */
    {  1,  0.056f, 0.102f, 0.035f, 0.076f, XBOX_IOS_ZONE_FLAG,   XBOX_GAMEPAD_BACK  },
    {  2,  0.944f, 0.102f, 0.035f, 0.076f, XBOX_IOS_ZONE_FLAG,   XBOX_GAMEPAD_START },
    {  4,  0.150f, 0.100f, 0.032f, 0.070f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_BLACK    },  /* LB */
    {  6,  0.850f, 0.100f, 0.032f, 0.070f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_WHITE    },  /* RB */
    {  5,  0.785f, 0.780f, 0.055f, 0.150f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_LTRIGGER },  /* BRAKE */
    {  7,  0.915f, 0.700f, 0.065f, 0.220f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_RTRIGGER },  /* GAS */
    {  8,  0.830f, 0.500f, 0.030f, 0.066f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_A      },
    {  9,  0.890f, 0.380f, 0.030f, 0.066f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_B      },
    { 10,  0.770f, 0.380f, 0.030f, 0.066f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_X      },
    { 11,  0.830f, 0.260f, 0.030f, 0.066f, XBOX_IOS_ZONE_ANALOG, XBOX_BUTTON_Y      },
    {  3,  0.080f, 0.810f, 0.045f, 0.098f, XBOX_IOS_ZONE_DPAD,   0                  },
};
#define DRIVE_CLASSIC_COUNT ((int)(sizeof s_zones_drive_classic / sizeof s_zones_drive_classic[0]))

#define ZONE_MAX 16
typedef char zone_max_ok[(PAD_COUNT <= ZONE_MAX && DRIVE_NEW_COUNT <= ZONE_MAX &&
                         DRIVE_CLASSIC_COUNT <= ZONE_MAX) ? 1 : -1];

/* The active table: the chosen preset with rx/ry times the size setting.
 * Rebuilt under s_lock; the overlay reads it on the main thread, the same
 * thread that changes it. */
static XBOX_IOS_TOUCH_ZONE s_zones[ZONE_MAX];
static int   s_zone_n;
static int   s_drive;                 /* 0 PAD, 1 DRIVE */
static float s_scale = 1.0f;          /* NFSU2_TOUCH_SCALE, 0.7..1.5 */
static unsigned s_layout_gen;         /* bumped by every rebuild (__atomic) */

/* Screen adaptation (all under s_lock; every default is the identity, so a
 * layer that is never told anything builds today's table). Fed by the overlay,
 * never by SDL window events.
 *   safe area   xbox_ios_touch_set_safe_area: the Dynamic Island / notch bands
 *               as fractions of the window width; every zone's x is mapped into
 *               the band between them (zones_rebuild_locked).
 *   aspect      xbox_ios_touch_set_aspect: window width / height. The tables
 *               are laid out for PHONE_ASPECT; when the window differs by more
 *               than ASPECT_TOL, round controls are re-shaped and the tablet
 *               size factor applies. */
#define PHONE_ASPECT (852.0f / 393.0f)
#define ASPECT_TOL   0.10f
#define SAFE_MAX     0.25f            /* per side; keeps the band >= half the screen */
#define TABLET_MIN   0.6f
#define TABLET_MAX   1.2f
static float s_safe_l, s_safe_r;      /* requested insets, 0..SAFE_MAX */
static int   s_safe_on = 1;           /* NFSU2_TOUCH_SAFE (the overlay applies it) */
static float s_zone_aspect;           /* window w / h; 0 = not known */
static int   s_round_on = 1;          /* NFSU2_TOUCH_ROUND: re-shape round controls */
static float s_tablet = 1.0f;         /* NFSU2_TOUCH_TABLET_SCALE (overlay: 1.0 on phones) */
static float s_zone_shift[ZONE_MAX];  /* safe-area x shift per active zone */

/* Panel / env switches of the drive feel. s_steer_on / s_steer_sens /
 * s_analog_pedals are read by the poll under s_lock and written under it. */
static int   s_drive_classic;         /* NFSU2_DRIVE_CLASSIC (0 default) */
static int   s_steer_on = 1;          /* NFSU2_STEER: the strip exists in DRIVE */
static float s_steer_sens = 1.0f;     /* NFSU2_STEER_SENS, 0.5..2.0 */
static int   s_steer_rel = 1;         /* NFSU2_STEER_MODE: 1 relative, 0 absolute */
static float s_steer_curve = 1.35f;   /* NFSU2_STEER_CURVE, 1.0..2.0 (relative mode) */
static int   s_analog_pedals;         /* NFSU2_ANALOG_PEDALS (0 default: full press) */

/* The strip keeps its width at every size setting (in absolute mode the
 * steering gain follows its half-width, so the size slider must not change the
 * feel); its height
 * follows the slider but never leaves the screen: half-height <= 0.19, which
 * keeps the bottom edge at or above y = 0.98 from its centre 0.79. */
#define STEER_RY_MAX 0.19f

/* ---- Per-zone geometry overrides (the layout editor) ----
 * An override replaces one zone's centre and radii of the preset, before the
 * size scale. Keyed by (layout, zone id); PAD and DRIVE are separate, and the
 * DRIVE ones also apply when the classic DRIVE table is on (same ids). All
 * state is under s_lock. */
#define GEOM_IDS 32
typedef struct {
    int   used;
    float cx, cy, rx, ry;   /* base (unscaled), rect half-extents for STEER */
} GeomOv;
static GeomOv s_geom_ov[2][GEOM_IDS];

#define GEOM_RX_MIN 0.02f
#define GEOM_RX_MAX 0.30f
#define GEOM_RY_MIN 0.04f
#define GEOM_RY_MAX 0.45f
#define GEOM_STEER_RX_MAX 0.45f

static const GeomOv *geom_ov_get(int drive, int id)
{
    if (id < 0 || id >= GEOM_IDS || !s_geom_ov[drive ? 1 : 0][id].used)
        return NULL;
    return &s_geom_ov[drive ? 1 : 0][id];
}

/* The preset (before overrides and scale) of zone `id` in the table the
 * layout is currently using; NULL when the id is not in it. */
static const XBOX_IOS_TOUCH_ZONE *preset_find(const XBOX_IOS_TOUCH_ZONE *tab, int n, int id)
{
    int i;
    for (i = 0; i < n; i++)
        if (tab[i].id == id)
            return &tab[i];
    return NULL;
}

static float clampf_nan_lo(float v, float lo, float hi)
{
    return !(v >= lo) ? lo : v > hi ? hi : v;   /* NaN gives lo */
}

/* Radii first, then the centre so the whole zone lies inside 0..1. */
static void geom_clamp(int steer, float *cx, float *cy, float *rx, float *ry)
{
    *rx = clampf_nan_lo(*rx, GEOM_RX_MIN, steer ? GEOM_STEER_RX_MAX : GEOM_RX_MAX);
    *ry = clampf_nan_lo(*ry, GEOM_RY_MIN, steer ? STEER_RY_MAX : GEOM_RY_MAX);
    *cx = clampf_nan_lo(*cx, *rx, 1.0f - *rx);
    *cy = clampf_nan_lo(*cy, *ry, 1.0f - *ry);
}

/* An overridden zone stays fully on screen after the size scale too (a
 * preset zone at 1.5 may poke out, as it always did). */
static void zone_keep_on_screen(XBOX_IOS_TOUCH_ZONE *z)
{
    if (z->rx > 0.5f) z->rx = 0.5f;
    if (z->ry > 0.5f) z->ry = 0.5f;
    if (z->cx < z->rx) z->cx = z->rx;
    else if (z->cx > 1.0f - z->rx) z->cx = 1.0f - z->rx;
    if (z->cy < z->ry) z->cy = z->ry;
    else if (z->cy > 1.0f - z->ry) z->cy = 1.0f - z->ry;
}

/* The aspect the window is treated as: its w / h when that is a landscape
 * window more than ASPECT_TOL away from the phone tables' aspect, else 0. */
static float aspect_effective_locked(void)
{
    float a = s_zone_aspect;
    if (!(a >= 1.0f && a <= 3.0f))
        return 0.0f;                  /* unknown, NaN or portrait */
    if (fabsf(a / PHONE_ASPECT - 1.0f) <= ASPECT_TOL)
        return 0.0f;
    return a;
}

/* Insets in force: the requested ones while the setting is on. */
static void safe_effective_locked(float *l, float *r)
{
    *l = s_safe_on ? s_safe_l : 0.0f;
    *r = s_safe_on ? s_safe_r : 0.0f;
}

/* Controls that are round (or round-ish pills) on the phone: they keep their
 * shape in points on any aspect. Pedals, triggers and the strip do not. */
static int zone_is_round_style(const XBOX_IOS_TOUCH_ZONE *z)
{
    switch (z->kind) {
    case XBOX_IOS_ZONE_FLAG:
    case XBOX_IOS_ZONE_DPAD:
        return 1;
    case XBOX_IOS_ZONE_ANALOG:
        return z->value != XBOX_BUTTON_LTRIGGER && z->value != XBOX_BUTTON_RTRIGGER;
    default:
        return 0;
    }
}

/* Safe-area shift, a rigid move per half of the screen (spacing and sizes are
 * kept): zones with cx < 0.5 form the left group, the rest the right group (the
 * strip goes with its centre). The left group moves right by the amount its
 * outermost left edge sits inside the left inset; the right group moves left by
 * the amount its outermost right edge sits inside the right inset. A zone
 * wider than the band (a strip: its half-width is cut to the band first) is
 * centred in the band instead. s_zone_shift[i] is the x shift of zone i. */
static void zones_safe_shift_locked(float sl, float sr)
{
    float sk = 1.0f - sl - sr, minl = 2.0f, maxr = -1.0f, shl, shr;
    int wide[ZONE_MAX];
    int i;

    for (i = 0; i < ZONE_MAX; i++)
        s_zone_shift[i] = 0.0f;
    if (!(sl > 0.0f || sr > 0.0f))
        return;
    for (i = 0; i < s_zone_n; i++) {
        XBOX_IOS_TOUCH_ZONE *z = &s_zones[i];
        wide[i] = (2.0f * z->rx > sk + 1e-6f);
        if (z->kind == XBOX_IOS_ZONE_STEER && z->rx > sk * 0.5f)
            z->rx = sk * 0.5f;              /* cut to the band, centred below */
        if (wide[i])
            continue;
        if (z->cx < 0.5f) {
            if (z->cx - z->rx < minl)
                minl = z->cx - z->rx;
        } else if (z->cx + z->rx > maxr) {
            maxr = z->cx + z->rx;
        }
    }
    shl = (minl < 2.0f && minl < sl) ? sl - minl : 0.0f;
    shr = (maxr > -1.0f && maxr > 1.0f - sr) ? maxr - (1.0f - sr) : 0.0f;
    for (i = 0; i < s_zone_n; i++) {
        XBOX_IOS_TOUCH_ZONE *z = &s_zones[i];
        float sh = wide[i] ? sl + sk * 0.5f - z->cx : z->cx < 0.5f ? shl : -shr;
        z->cx += sh;
        s_zone_shift[i] = sh;
    }
}

/* Active table = preset -> override -> size -> aspect shape -> tablet factor
 * -> keep on screen (overrides) -> safe-area shift (rigid per half). The result is the one table
 * hit-testing and the overlay both use. */
static void zones_rebuild_locked(void)
{
    const XBOX_IOS_TOUCH_ZONE *src = !s_drive ? s_zones_pad
                                   : s_drive_classic ? s_zones_drive_classic
                                                     : s_zones_drive;
    int n = !s_drive ? PAD_COUNT
                     : s_drive_classic ? DRIVE_CLASSIC_COUNT : DRIVE_NEW_COUNT;
    float ea = aspect_effective_locked();
    float tab = ea > 0.0f ? s_tablet : 1.0f;
    float shape = (ea > 0.0f && s_round_on) ? ea / PHONE_ASPECT : 1.0f;
    float sl, sr;
    int i, m = 0;

    safe_effective_locked(&sl, &sr);
    for (i = 0; i < n; i++) {
        XBOX_IOS_TOUCH_ZONE z = src[i];
        const GeomOv *o = geom_ov_get(s_drive, z.id);
        if (o) {
            z.cx = o->cx;
            z.cy = o->cy;
            z.rx = o->rx;
            z.ry = o->ry;
        }
        if (z.kind == XBOX_IOS_ZONE_STEER) {
            if (!s_steer_on)
                continue;
            z.ry *= s_scale;
            z.ry *= tab;              /* the width keeps the steering feel */
            if (z.ry > STEER_RY_MAX)
                z.ry = STEER_RY_MAX;
        } else {
            z.rx *= s_scale;
            z.ry *= s_scale;
            if (zone_is_round_style(&z))
                z.ry *= shape;        /* same shape in points as on the phone */
            z.rx *= tab;
            z.ry *= tab;
        }
        if (o)
            zone_keep_on_screen(&z);
        s_zones[m++] = z;
    }
    s_zone_n = m;
    zones_safe_shift_locked(sl, sr);
    __atomic_add_fetch(&s_layout_gen, 1u, __ATOMIC_RELEASE);
}

/* Table order is hit-test priority: the first zone whose ellipse contains
 * the touch wins, which settles the handful of pixels where neighbours
 * overlap. A STEER zone is the exception: a wide strip is hit as a rectangle
 * (rx / ry are half-extents), because the pointed ends of an ellipse would
 * leave the corners of the visible track to the dynamic stick. */
static int zone_at(float x, float y)
{
    int i;
    for (i = 0; i < s_zone_n; i++) {
        float dx = (x - s_zones[i].cx) / s_zones[i].rx;
        float dy = (y - s_zones[i].cy) / s_zones[i].ry;
        if (s_zones[i].kind == XBOX_IOS_ZONE_STEER) {
            if (fabsf(dx) <= 1.0f && fabsf(dy) <= 1.0f)
                return (int)i;
        } else if (dx * dx + dy * dy <= 1.0f) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Finger bookkeeping                                                  */
/* ------------------------------------------------------------------ */

#define TOUCH_MAX_FINGERS 5   /* two sticks and all the buttons, with slack */

/* What a tracked finger drives. Zone fingers carry the zone's table index
 * offset from ROLE_ZONE(0). */
#define ROLE_NONE   0
#define ROLE_LSTICK 1
#define ROLE_RSTICK 2
#define ROLE_ZONE(i) (100 + (i))

typedef struct {
    Sint64 id;      /* SDL's finger id, -1 = slot free */
    float  x, y;    /* latest position, normalized to the window */
    float  sx, sy;  /* position when it went down: a stick's centre */
    int    role;    /* ROLE_* */
} TouchFinger;

static SDL_mutex  *s_lock;
static TouchFinger s_finger[TOUCH_MAX_FINGERS];
static float       s_aspect = 9.0f / 16.0f;   /* window height / width (landscape) */
static DWORD       s_packet;
static int         s_live;                    /* the watch is installed */
static int         s_enabled = 1;             /* RECOMP_TOUCH, then the panel */
static int         s_prefs_done;
static float       s_tilt_x;                  /* under s_lock */
static int         s_tilt_on;                 /* under s_lock */
static void      (*s_press_hook)(int);        /* zone index; see set_press_hook */
static void      (*s_activity_hook)(void);    /* see set_activity_hook */

static void touch_clear_locked(void);

/* First use from any thread: read the env defaults, build the zone table and
 * create the lock. Every public entry point calls this first, so the main
 * thread (overlay) and the game thread (input poll) never see a half-built
 * layer, whichever comes first. */
/* An env switch: unset or empty = def; 0/n/f = off; 1/y/t = on. */
static int env_switch(const char *name, int def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    if (*v == '0' || *v == 'n' || *v == 'N' || *v == 'f' || *v == 'F')
        return 0;
    if (*v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T')
        return 1;
    return def;
}

static float steer_sens_clamp(float k)
{
    return !(k >= 0.5f) ? 0.5f : k > 2.0f ? 2.0f : k;
}

#define STEER_CURVE_DEF 1.35f

static float steer_curve_clamp(float c)
{
    return !(c >= 1.0f) ? 1.0f : c > 2.0f ? 2.0f : c;   /* NaN gives 1.0 */
}

/* "absolute" (any case) = 0; anything else, including unset, relative = 1. */
static int steer_mode_parse(const char *v)
{
    return !(v && strcasecmp(v, "absolute") == 0);
}

static void touch_prefs_once(void)
{
    static SDL_SpinLock s_prefs_spin;
    if (__atomic_load_n(&s_prefs_done, __ATOMIC_ACQUIRE))
        return;
    SDL_AtomicLock(&s_prefs_spin);
    if (!s_prefs_done) {
        const char *v;
        int i;

        v = getenv("RECOMP_TOUCH");
        s_enabled = !(v && strcmp(v, "0") == 0);
        v = getenv("NFSU2_TOUCH_LAYOUT");
        s_drive = (v && strcasecmp(v, "drive") == 0);
        v = getenv("NFSU2_TOUCH_SCALE");
        if (v && *v) {
            float k = (float)atof(v);
            s_scale = !(k >= 0.7f) ? 0.7f : k > 1.5f ? 1.5f : k;
        }
        s_analog_pedals = env_switch("NFSU2_ANALOG_PEDALS", 0);
        s_steer_on = env_switch("NFSU2_STEER", 1);
        s_drive_classic = env_switch("NFSU2_DRIVE_CLASSIC", 0);
        v = getenv("NFSU2_STEER_SENS");
        s_steer_sens = (v && *v) ? steer_sens_clamp((float)atof(v)) : 1.0f;
        s_steer_rel = steer_mode_parse(getenv("NFSU2_STEER_MODE"));
        v = getenv("NFSU2_STEER_CURVE");
        s_steer_curve = (v && *v) ? steer_curve_clamp((float)atof(v)) : STEER_CURVE_DEF;
        for (i = 0; i < TOUCH_MAX_FINGERS; i++)
            s_finger[i].id = -1;
        s_lock = SDL_CreateMutex();
        zones_rebuild_locked();
        __atomic_store_n(&s_prefs_done, 1, __ATOMIC_RELEASE);
    }
    SDL_AtomicUnlock(&s_prefs_spin);
}

/* Settings hooks (nfsu2_env.txt / gear panel). Called from the main thread
 * between polls; races with a poll in flight settle on the next poll. Off
 * means off: fingers are dropped, new ones are ignored, and
 * xbox_ios_touch_state() reports nothing. */
void xbox_ios_touch_set_enabled(int on)
{
    touch_prefs_once();
    __atomic_store_n(&s_enabled, on ? 1 : 0, __ATOMIC_RELEASE);
    SDL_LockMutex(s_lock);
    touch_clear_locked();
    s_tilt_on = 0;   /* tilt rides on the touch layer: off with it */
    SDL_UnlockMutex(s_lock);
}

int xbox_ios_touch_enabled(void)
{
    touch_prefs_once();
    return __atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE);
}

/* PAD (0) or DRIVE (1). Fingers are cleared first: a finger that holds
 * "zone 7" must not keep meaning something else in the new table. */
void xbox_ios_touch_set_layout(int drive)
{
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    touch_clear_locked();
    s_drive = drive ? 1 : 0;
    zones_rebuild_locked();
    SDL_UnlockMutex(s_lock);
}

/* Zone size factor, clamped 0.7..1.5 (rx/ry only; centres stay). */
void xbox_ios_touch_set_scale(float k)
{
    touch_prefs_once();
    k = !(k >= 0.7f) ? 0.7f : k > 1.5f ? 1.5f : k;
    SDL_LockMutex(s_lock);
    touch_clear_locked();
    s_scale = k;
    zones_rebuild_locked();
    SDL_UnlockMutex(s_lock);
}

/* Analog pedals on / off (NFSU2_ANALOG_PEDALS, off by default): off = GAS /
 * BRAKE read a flat 255 wherever the finger is. No finger is dropped: the next poll just reads the other
 * rule. */
void xbox_ios_touch_set_analog_pedals(int on)
{
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    s_analog_pedals = on ? 1 : 0;
    SDL_UnlockMutex(s_lock);
}

/* Steering strip on / off (NFSU2_STEER). Rebuilds the zone table, so every
 * finger is dropped, like a layout change. Only DRIVE has the strip. */
void xbox_ios_touch_set_steer(int on)
{
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    touch_clear_locked();
    s_steer_on = on ? 1 : 0;
    zones_rebuild_locked();
    SDL_UnlockMutex(s_lock);
}

/* Strip sensitivity (NFSU2_STEER_SENS), clamped 0.5..2.0. */
void xbox_ios_touch_set_steer_sens(float k)
{
    touch_prefs_once();
    k = steer_sens_clamp(k);
    SDL_LockMutex(s_lock);
    s_steer_sens = k;
    SDL_UnlockMutex(s_lock);
}

/* Steering mode (NFSU2_STEER_MODE): 1 relative (default), 0 absolute. No
 * rebuild, no finger is dropped: the next poll reads a held finger by the other
 * rule (its touch-down point is already recorded). */
void xbox_ios_touch_set_steer_mode(int relative)
{
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    s_steer_rel = relative ? 1 : 0;
    SDL_UnlockMutex(s_lock);
}

int xbox_ios_touch_get_steer_mode(void)
{
    int r;
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    r = s_steer_rel;
    SDL_UnlockMutex(s_lock);
    return r;
}

/* Response curve (NFSU2_STEER_CURVE), clamped 1.0..2.0, NaN gives 1.0. */
void xbox_ios_touch_set_steer_curve(float c)
{
    touch_prefs_once();
    c = steer_curve_clamp(c);
    SDL_LockMutex(s_lock);
    s_steer_curve = c;
    SDL_UnlockMutex(s_lock);
}

/* Old DRIVE table (NFSU2_DRIVE_CLASSIC): rebuilds the table, drops fingers. */
void xbox_ios_touch_set_drive_classic(int on)
{
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    touch_clear_locked();
    s_drive_classic = on ? 1 : 0;
    zones_rebuild_locked();
    SDL_UnlockMutex(s_lock);
}

/* ---- Screen adaptation (safe area, aspect, tablet size) ----
 * Each setter does nothing when the value in force would not change: the
 * overlay calls them from layoutSubviews, and a rebuild bumps the generation
 * (a full relay of the layers). None clears fingers: the table keeps its
 * order, so a held zone stays the same zone. */
static float safe_clamp(float v)
{
    return !(v >= 0.0f) ? 0.0f : v > SAFE_MAX ? SAFE_MAX : v;   /* NaN gives 0 */
}

/* Fractions of the window width taken by the unsafe bands on the left and on
 * the right. Zones are mapped into the band between them. */
void xbox_ios_touch_set_safe_area(float left, float right)
{
    touch_prefs_once();
    left = safe_clamp(left);
    right = safe_clamp(right);
    SDL_LockMutex(s_lock);
    if (fabsf(left - s_safe_l) > 1e-5f || fabsf(right - s_safe_r) > 1e-5f) {
        float l0, r0, l1, r1;
        safe_effective_locked(&l0, &r0);
        s_safe_l = left;
        s_safe_r = right;
        safe_effective_locked(&l1, &r1);
        if (fabsf(l0 - l1) > 1e-5f || fabsf(r0 - r1) > 1e-5f)
            zones_rebuild_locked();
    }
    SDL_UnlockMutex(s_lock);
}

/* NFSU2_TOUCH_SAFE: 0 = map nothing (full width, as before the safe area). */
void xbox_ios_touch_set_safe_enabled(int on)
{
    touch_prefs_once();
    on = on ? 1 : 0;
    SDL_LockMutex(s_lock);
    if (on != s_safe_on) {
        s_safe_on = on;
        if (s_safe_l > 0.0f || s_safe_r > 0.0f)
            zones_rebuild_locked();
    }
    SDL_UnlockMutex(s_lock);
}

/* The insets in force (zero while the setting is off). */
void xbox_ios_touch_get_safe_area(float *left, float *right)
{
    float l, r;
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    safe_effective_locked(&l, &r);
    SDL_UnlockMutex(s_lock);
    if (left)  *left = l;
    if (right) *right = r;
}

/* Base x (what zone_geom / set_geom use) <-> screen x for zone idx of the
 * active table: the zone's group shift is added / removed (see
 * zones_safe_shift_locked). The shift is the one in force now; an index out of
 * range gives the value back unchanged. */
float xbox_ios_touch_base_to_screen_x(int idx, float bx)
{
    float sh = 0.0f;
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    if (idx >= 0 && idx < s_zone_n)
        sh = s_zone_shift[idx];
    SDL_UnlockMutex(s_lock);
    return bx + sh;
}

float xbox_ios_touch_screen_to_base_x(int idx, float sx)
{
    float sh = 0.0f;
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    if (idx >= 0 && idx < s_zone_n)
        sh = s_zone_shift[idx];
    SDL_UnlockMutex(s_lock);
    return sx - sh;
}

/* Window width / height, for the round controls and the tablet size factor.
 * <= 0 or NaN = unknown (tables as laid out for the phone). Only an aspect
 * more than 10 % from 852/393 changes the table. */
void xbox_ios_touch_set_aspect(float w_over_h)
{
    float e0, e1;

    touch_prefs_once();
    if (!(w_over_h > 0.0f))
        w_over_h = 0.0f;
    SDL_LockMutex(s_lock);
    e0 = aspect_effective_locked();
    s_zone_aspect = w_over_h;
    e1 = aspect_effective_locked();
    if (fabsf(e0 - e1) > 1e-4f)
        zones_rebuild_locked();
    SDL_UnlockMutex(s_lock);
}

/* NFSU2_TOUCH_ROUND: 0 = keep the normalized radii of the tables on any aspect
 * (the tablet size factor still applies). */
void xbox_ios_touch_set_round(int on)
{
    touch_prefs_once();
    on = on ? 1 : 0;
    SDL_LockMutex(s_lock);
    if (on != s_round_on) {
        s_round_on = on;
        if (aspect_effective_locked() > 0.0f)
            zones_rebuild_locked();
    }
    SDL_UnlockMutex(s_lock);
}

/* Size factor on top of the Size setting, applied only while the aspect
 * differs from the phone's. Clamped 0.6..1.2; NaN gives 1.0. The strip takes
 * it in height only. */
void xbox_ios_touch_set_tablet_scale(float k)
{
    touch_prefs_once();
    k = (k != k) ? 1.0f : k < TABLET_MIN ? TABLET_MIN : k > TABLET_MAX ? TABLET_MAX : k;
    SDL_LockMutex(s_lock);
    if (fabsf(k - s_tablet) > 1e-5f) {
        s_tablet = k;
        if (aspect_effective_locked() > 0.0f)
            zones_rebuild_locked();
    }
    SDL_UnlockMutex(s_lock);
}

/* Label for the overlay to draw on zone idx (active table order), or NULL
 * for a plain shape. */
const char *xbox_ios_touch_zone_label(int idx)
{
    const XBOX_IOS_TOUCH_ZONE *z;

    touch_prefs_once();
    if (idx < 0 || idx >= s_zone_n)
        return NULL;
    z = &s_zones[idx];
    if (z->kind != XBOX_IOS_ZONE_ANALOG)
        return NULL;   /* BACK/START/DPAD/STEER stay shapes */
    if (s_drive && !s_drive_classic) {
        /* DRIVE labels name the game's function (Control Type 1). The shift
         * arrows are U+2191 / U+2193 as UTF-8. */
        switch (z->value) {
        case XBOX_BUTTON_A:        return "E-BRAKE";
        case XBOX_BUTTON_B:        return "NOS";
        case XBOX_BUTTON_X:        return "SHIFT\xE2\x86\x91";
        case XBOX_BUTTON_Y:        return "SHIFT\xE2\x86\x93";
        case XBOX_BUTTON_BLACK:    return "LOOK";
        case XBOX_BUTTON_WHITE:    return "CAM";
        case XBOX_BUTTON_LTRIGGER: return "BRAKE";
        case XBOX_BUTTON_RTRIGGER: return "GAS";
        default:                   return NULL;
        }
    }
    switch (z->value) {
    case XBOX_BUTTON_A:        return "A";
    case XBOX_BUTTON_B:        return "B";
    case XBOX_BUTTON_X:        return "X";
    case XBOX_BUTTON_Y:        return "Y";
    case XBOX_BUTTON_BLACK:    return "LB";
    case XBOX_BUTTON_WHITE:    return "RB";
    case XBOX_BUTTON_LTRIGGER: return s_drive ? "BRAKE" : "LT";
    case XBOX_BUTTON_RTRIGGER: return s_drive ? "GAS" : "RT";
    default:                   return NULL;
    }
}

/* Short function text the overlay may draw under a zone's main glyph (see the
 * header). Same index convention as xbox_ios_touch_zone_label(). */
const char *xbox_ios_touch_zone_caption(int idx)
{
    const XBOX_IOS_TOUCH_ZONE *z;

    touch_prefs_once();
    if (idx < 0 || idx >= s_zone_n)
        return NULL;
    if (s_drive && !s_drive_classic)
        return NULL;   /* the new DRIVE labels already name the function */
    z = &s_zones[idx];
    if (z->kind == XBOX_IOS_ZONE_FLAG)
        return z->value == XBOX_GAMEPAD_START ? "PAUSE" : NULL;
    if (z->kind != XBOX_IOS_ZONE_ANALOG)
        return NULL;
    switch (z->value) {
    case XBOX_BUTTON_A:        return "E-BRAKE";
    case XBOX_BUTTON_B:        return "NOS";
    case XBOX_BUTTON_X:        return "SHIFT UP";
    case XBOX_BUTTON_Y:        return "SHIFT DN";
    case XBOX_BUTTON_BLACK:    return "LOOK";
    case XBOX_BUTTON_WHITE:    return "CAM";
    case XBOX_BUTTON_LTRIGGER: return "BRAKE";
    case XBOX_BUTTON_RTRIGGER: return "GAS";
    default:                   return NULL;
    }
}

/* SF Symbol name for zone idx, or NULL (see the header). */
const char *xbox_ios_touch_zone_icon(int idx)
{
    const XBOX_IOS_TOUCH_ZONE *z;
    int new_drive;

    touch_prefs_once();
    if (idx < 0 || idx >= s_zone_n)
        return NULL;
    z = &s_zones[idx];
    new_drive = (s_drive && !s_drive_classic);
    if (z->kind == XBOX_IOS_ZONE_FLAG) {
        if (z->value == XBOX_GAMEPAD_START) return "pause.fill";
        if (z->value == XBOX_GAMEPAD_BACK)  return "arrow.uturn.backward";
        return NULL;
    }
    if (z->kind != XBOX_IOS_ZONE_ANALOG)
        return NULL;
    switch (z->value) {
    case XBOX_BUTTON_BLACK: return "eye.fill";
    case XBOX_BUTTON_WHITE: return "camera.fill";
    case XBOX_BUTTON_A:     return new_drive ? "exclamationmark.octagon.fill" : NULL;
    case XBOX_BUTTON_B:     return new_drive ? "flame.fill" : NULL;
    case XBOX_BUTTON_X:     return new_drive ? "arrow.up" : NULL;
    case XBOX_BUTTON_Y:     return new_drive ? "arrow.down" : NULL;
    default:                return NULL;   /* pedals / triggers keep their text */
    }
}

/* Called (on whichever thread pumps SDL events) when a finger lands on a
 * zone. The overlay uses it for haptics. NULL clears. */
void xbox_ios_touch_set_press_hook(void (*hook)(int))
{
    __atomic_store_n(&s_press_hook, hook, __ATOMIC_RELEASE);
}

/* Called (same threads) when ANY finger goes down and is tracked: a zone, the
 * steering strip or a dynamic stick. Not for a repeated down of a finger that
 * is already held, not for a finger beyond the tracked limit, not while touch
 * is off. Runs outside the layer's lock, after the finger is recorded, and
 * before the press hook. The overlay uses it to wake its display link. NULL
 * clears. */
void xbox_ios_touch_set_activity_hook(void (*hook)(void))
{
    __atomic_store_n(&s_activity_hook, hook, __ATOMIC_RELEASE);
}

/* Tilt steering from the overlay's motion updates: x in -1..1, active 0
 * stops it. Used for the left stick X while no finger holds that stick. */
void xbox_ios_touch_set_tilt(float x, int active)
{
    touch_prefs_once();
    x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
    SDL_LockMutex(s_lock);
    s_tilt_x = active ? x : 0.0f;
    s_tilt_on = active ? 1 : 0;
    SDL_UnlockMutex(s_lock);
}

/* A stick's maximum deflection, as a fraction of the screen width -- the
 * same in normalized x units on every device, which is why the radius is
 * not normalized to the height. */
#define TOUCH_STICK_RADIUS 0.12f

/* Below this fraction of full deflection a stick reads zero; the game
 * applies its own deadzone on top, this only keeps a resting thumb from
 * creeping. */
#define TOUCH_DEADZONE 0.10f

/* Deflection of a dynamic stick from its finger: offset from the touch-down
 * point, scaled so TOUCH_STICK_RADIUS of screen width is full travel,
 * rescaled past the deadzone so travel starts smoothly, y inverted (screen
 * y grows downward, the Xbox stick's grows upward). */
static void stick_deflection(const TouchFinger *f, float aspect,
                             float *ox, float *oy)
{
    float dx = f->x - f->sx;              /* already width-relative */
    float dy = (f->y - f->sy) * aspect;   /* y is normalized by the height */
    float r = sqrtf(dx * dx + dy * dy);
    float m, k;

    *ox = 0.0f;
    *oy = 0.0f;
    if (r < 1e-5f)
        return;
    m = r / TOUCH_STICK_RADIUS;
    if (m > 1.0f) {
        dx *= 1.0f / m;
        dy *= 1.0f / m;
        m = 1.0f;
    }
    if (m <= TOUCH_DEADZONE)
        return;
    /* Rescale deadzone→full travel onto the FULL Xbox axis range: at m=1 the
     * deflection magnitude must be 1.0 (dx has magnitude TOUCH_STICK_RADIUS
     * there), hence the extra 1/TOUCH_STICK_RADIUS. */
    k = ((m - TOUCH_DEADZONE) / (1.0f - TOUCH_DEADZONE)) / m
        / TOUCH_STICK_RADIUS;
    *ox = dx * k;
    *oy = -dy * k;
}

static SHORT to_short(float v)
{
    int n = (int)(v * 32767.0f);
    if (n >  32767) n =  32767;
    if (n < -32767) n = -32767;
    return (SHORT)n;
}

/* Analog pedals (opt-in, NFSU2_ANALOG_PEDALS=1; DRIVE, the LT / RT zones): the
 * value follows the finger's height inside the zone. The upper PEDAL_FULL (60 %) of the pedal is full
 * travel; from there it eases linearly to PEDAL_MIN_BYTE at the very bottom,
 * so sliding the thumb down eases off while a normal tap anywhere in the
 * middle stays at 255. A latched finger that leaves the zone is clamped to
 * the zone's top / bottom. Called with s_lock held. */
#define PEDAL_FULL      0.60f
#define PEDAL_MIN_BYTE  90.0f

static int zone_is_pedal(const XBOX_IOS_TOUCH_ZONE *z)
{
    return s_analog_pedals && s_drive && z->kind == XBOX_IOS_ZONE_ANALOG &&
           (z->value == XBOX_BUTTON_LTRIGGER || z->value == XBOX_BUTTON_RTRIGGER);
}

/* 0..1 (1 = 255). */
static float pedal_level(const XBOX_IOS_TOUCH_ZONE *z, float y)
{
    float f = (y - (z->cy - z->ry)) / (2.0f * z->ry);   /* 0 top .. 1 bottom */
    float lo = PEDAL_MIN_BYTE / 255.0f;
    if (!(f > PEDAL_FULL))
        return 1.0f;                                     /* also NaN */
    if (f > 1.0f)
        f = 1.0f;
    return 1.0f - (f - PEDAL_FULL) / (1.0f - PEDAL_FULL) * (1.0f - lo);
}

static BYTE pedal_byte(const XBOX_IOS_TOUCH_ZONE *z, float y)
{
    int n = (int)(pedal_level(z, y) * 255.0f + 0.5f);
    return (BYTE)(n > 255 ? 255 : n < 0 ? 0 : n);
}

/* Steering strip: signed -1..1 from a finger's x. Called with s_lock held.
 *
 * Relative (default): the touch-down x (sx) is straight ahead. The deflection
 * is d = (x - sx) / travel with travel = STEER_REL_TRAVEL of the window width
 * (0.075, about 64 pt on an 852 pt screen) divided by the sensitivity, clamped
 * to +-1 (a finger that goes on past full lock, or off the strip, stays at
 * full lock). A deadzone of 4 % of travel is rescaled away (no jump), then the
 * response curve out = sign(d) * |d|^NFSU2_STEER_CURVE gives fine control near
 * the centre (1.0 = linear). The strip's own size plays no part.
 *
 * Absolute (NFSU2_STEER_MODE=absolute, the first version): the zone centre is
 * straight ahead and full travel is the zone's half-width divided by the
 * sensitivity (at 2.0 the thumb needs half the distance; at 0.5, full lock is
 * out of reach on the left edge -- the strip cannot be wider than the
 * screen). 5 % deadzone, rescaled. No curve: this is the old behaviour. */
#define STEER_DEADZONE      0.05f
#define STEER_REL_TRAVEL    0.075f
#define STEER_REL_DEADZONE  0.04f

static float steer_value(const XBOX_IOS_TOUCH_ZONE *z, const TouchFinger *f)
{
    float v, a, dz;

    if (s_steer_rel) {
        v = (f->x - f->sx) / (STEER_REL_TRAVEL / s_steer_sens);
        dz = STEER_REL_DEADZONE;
    } else {
        v = (f->x - z->cx) / (z->rx / s_steer_sens);
        dz = STEER_DEADZONE;
    }
    a = fabsf(v);
    if (!(a >= dz))
        return 0.0f;                               /* also NaN */
    if (a > 1.0f)
        a = 1.0f;
    a = (a - dz) / (1.0f - dz);
    if (s_steer_rel && s_steer_curve > 1.0f)
        a = powf(a, s_steer_curve);
    return v < 0.0f ? -a : a;
}

/* Everything a held finger means, folded into the caller's accumulators.
 * Called with s_lock held. A steering-strip finger outranks a dynamic
 * left-stick finger whatever their slot order (the strip writes X and zeroes
 * Y even after the stick wrote them; a stick finger that comes second finds
 * have_l set); the first strip finger in slot order steers, others are
 * ignored. */
static void fold_finger(const TouchFinger *f, float aspect, WORD *flags,
                        BYTE *analog, float *lstick, float *rstick,
                        int *have_l, int *have_r, int *have_steer)
{
    if (f->role >= ROLE_ZONE(0)) {
        const XBOX_IOS_TOUCH_ZONE *z;
        if (f->role - ROLE_ZONE(0) >= s_zone_n)
            return;
        z = &s_zones[f->role - ROLE_ZONE(0)];
        switch (z->kind) {
        case XBOX_IOS_ZONE_FLAG:
            *flags |= (WORD)z->value;
            break;
        case XBOX_IOS_ZONE_ANALOG: {
            BYTE v = zone_is_pedal(z) ? pedal_byte(z, f->y) : (BYTE)255;
            if (v > analog[z->value])
                analog[z->value] = v;
            break;
        }
        case XBOX_IOS_ZONE_STEER:
            if (!*have_steer) {
                lstick[0] = steer_value(z, f);
                lstick[1] = 0.0f;
                *have_steer = 1;
                *have_l = 1;
            }
            break;
        case XBOX_IOS_ZONE_DPAD: {  /* the touch's quadrant inside the zone,
                                     * centre neutral, diagonals when both
                                     * axes clear the mark */
            float nx = (f->x - z->cx) / z->rx;
            float ny = (f->y - z->cy) / z->ry;
            if (nx * nx + ny * ny > 0.16f) {   /* 0.4^2 */
                if (ny < -0.4f) *flags |= XBOX_GAMEPAD_DPAD_UP;
                if (ny >  0.4f) *flags |= XBOX_GAMEPAD_DPAD_DOWN;
                if (nx < -0.4f) *flags |= XBOX_GAMEPAD_DPAD_LEFT;
                if (nx >  0.4f) *flags |= XBOX_GAMEPAD_DPAD_RIGHT;
            }
            break;
        }
        default:
            break;
        }
    } else if (f->role == ROLE_LSTICK && !*have_l) {
        stick_deflection(f, aspect, &lstick[0], &lstick[1]);
        *have_l = 1;
    } else if (f->role == ROLE_RSTICK && !*have_r) {
        stick_deflection(f, aspect, &rstick[0], &rstick[1]);
        *have_r = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Event accumulation (the watch runs where SDL pumps events)          */
/* ------------------------------------------------------------------ */

static void touch_clear_locked(void)
{
    int i;
    for (i = 0; i < TOUCH_MAX_FINGERS; i++)
        s_finger[i].id = -1;
}

static void finger_down(SDL_TouchID touch_id, SDL_FingerID id, float x, float y)
{
    int i, zone;
    void (*hook)(int);
    void (*act)(void);

    (void)touch_id;   /* iOS has the one touch screen; ids are its own */

    if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE))
        return;
    SDL_LockMutex(s_lock);
    for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
        if (s_finger[i].id == id) {
            /* Already tracked (a device can repeat a down): just move it. */
            s_finger[i].x = x;
            s_finger[i].y = y;
            SDL_UnlockMutex(s_lock);
            return;
        }
    }
    for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
        if (s_finger[i].id < 0)
            break;
    }
    if (i == TOUCH_MAX_FINGERS) {   /* more fingers than tracked: ignore */
        SDL_UnlockMutex(s_lock);
        return;
    }

    zone = zone_at(x, y);
    s_finger[i].id = id;
    s_finger[i].x = x;
    s_finger[i].y = y;
    s_finger[i].sx = x;
    s_finger[i].sy = y;
    if (zone >= 0) {
        s_finger[i].role = ROLE_ZONE(zone);
    } else {
        /* A second finger on a side whose stick is already held waits its
         * turn (ROLE_NONE). finger_up promotes it re-centred, so a finger
         * that was merely resting never jolts the stick at the moment the
         * steering thumb lifts. */
        int want_l = (x < 0.5f);
        int taken = 0, j;
        for (j = 0; j < TOUCH_MAX_FINGERS; j++)
            if (s_finger[j].id >= 0 && j != i
                    && s_finger[j].role == (want_l ? ROLE_LSTICK : ROLE_RSTICK))
                taken = 1;
        s_finger[i].role = taken ? ROLE_NONE
                         : (want_l ? ROLE_LSTICK : ROLE_RSTICK);
    }
    SDL_UnlockMutex(s_lock);
    /* Outside the lock: the hooks may take their own locks or hop threads.
     * The finger is recorded by now, so a hook may query the layer. */
    act = __atomic_load_n(&s_activity_hook, __ATOMIC_ACQUIRE);
    if (act)
        act();
    if (zone >= 0) {
        hook = __atomic_load_n(&s_press_hook, __ATOMIC_ACQUIRE);
        if (hook)
            hook(zone);
    }
}

static void finger_move(SDL_FingerID id, float x, float y)
{
    int i;

    SDL_LockMutex(s_lock);
    for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
        if (s_finger[i].id == id) {
            s_finger[i].x = x;
            s_finger[i].y = y;
            break;
        }
    }
    SDL_UnlockMutex(s_lock);
}

/* A stick's finger lifting hands the stick to a finger that has been
 * waiting on the same half (re-centred where it rests), so steering does
 * not go dead while a thumb is still on the glass. */
static void finger_up(SDL_FingerID id)
{
    int i, role = ROLE_NONE;

    SDL_LockMutex(s_lock);
    for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
        if (s_finger[i].id == id) {
            role = s_finger[i].role;
            s_finger[i].id = -1;
            s_finger[i].role = ROLE_NONE;
            break;
        }
    }
    if (role == ROLE_LSTICK || role == ROLE_RSTICK) {
        int left = (role == ROLE_LSTICK);
        for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
            TouchFinger *f = &s_finger[i];
            int on_left = (f->x < 0.5f);
            if (f->id >= 0 && f->role == ROLE_NONE && on_left == left) {
                f->role = role;
                f->sx = f->x;   /* the stick recentres under the thumb */
                f->sy = f->y;
                break;
            }
        }
    }
    SDL_UnlockMutex(s_lock);
}

static int SDLCALL touch_watch(void *arg, SDL_Event *ev)
{
    (void)arg;
    switch (ev->type) {
    case SDL_FINGERDOWN:
        finger_down(ev->tfinger.touchId, ev->tfinger.fingerId,
                    ev->tfinger.x, ev->tfinger.y);
        break;
    case SDL_FINGERMOTION:
        finger_move(ev->tfinger.fingerId, ev->tfinger.x, ev->tfinger.y);
        break;
    case SDL_FINGERUP:
        finger_up(ev->tfinger.fingerId);
        break;
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            ev->window.event == SDL_WINDOWEVENT_RESIZED) {
            int w = ev->window.data1, h = ev->window.data2;
            if (w > 0 && h > 0) {
                SDL_LockMutex(s_lock);
                s_aspect = (float)h / (float)w;
                SDL_UnlockMutex(s_lock);
            }
        } else if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            /* The surface can go away without a background transition
             * (a call, the app switcher); a latched button must not
             * survive it. */
            SDL_LockMutex(s_lock);
            touch_clear_locked();
            SDL_UnlockMutex(s_lock);
        }
        break;
    case SDL_APP_WILLENTERBACKGROUND:
        /* Same on suspend: iOS would hold the touches while the app is
         * frozen, and the game would drive itself after resume. */
        SDL_LockMutex(s_lock);
        touch_clear_locked();
        SDL_UnlockMutex(s_lock);
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

int xbox_ios_touch_init(void)
{
    static SDL_SpinLock s_spin;   /* first caller from any thread wins */
    int live;

    touch_prefs_once();
    /* The watch goes in even with touch off: the panel can switch it on
     * later, and a disabled layer ignores every finger anyway. */
    SDL_AtomicLock(&s_spin);
    live = s_live;
    if (!live && s_lock) {
        /* The event system is core; the watch simply sees nothing until
         * the UIKit video backend exists and starts delivering touches.
         * SDL_AddEventWatch cannot fail, so the mutex is what can. */
        SDL_AddEventWatch(touch_watch, NULL);
        s_live = live = 1;
        fprintf(stderr, "[INPUT] iOS touch gamepad active%s "
                        "(RECOMP_TOUCH=0 disables)\n",
                s_enabled ? "" : " (currently off)");
    }
    SDL_AtomicUnlock(&s_spin);
    return live && s_enabled;
}

int xbox_ios_touch_state(XBOX_INPUT_STATE *pState)
{
    WORD  flags = 0;
    BYTE  analog[8] = { 0 };
    float lstick[2] = { 0.0f, 0.0f };
    float rstick[2] = { 0.0f, 0.0f };
    float aspect;
    DWORD packet;
    int   have_l = 0, have_r = 0, have_steer = 0, i;

    touch_prefs_once();
    if (!pState || !s_lock || !s_live ||
        !__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE))
        return 0;

    SDL_LockMutex(s_lock);
    aspect = s_aspect;
    for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
        if (s_finger[i].id >= 0)
            fold_finger(&s_finger[i], aspect, &flags, analog,
                        lstick, rstick, &have_l, &have_r, &have_steer);
    }
    /* Tilt steers while no finger steers: the left stick or the strip. */
    if (s_tilt_on && !have_l) {
        lstick[0] = s_tilt_x;
        lstick[1] = 0.0f;
    }
    /* The input layer looks for button edges on the packet number, so it
     * has to move on every read, the way the other backends' counters do. */
    packet = ++s_packet;
    SDL_UnlockMutex(s_lock);

    /* The struct is the caller's: zero what this layer does not answer
     * for rather than assume it arrived zeroed. */
    memset(pState, 0, sizeof(*pState));
    pState->dwPacketNumber = packet;
    pState->Gamepad.wButtons = flags;
    memcpy(pState->Gamepad.bAnalogButtons, analog, sizeof analog);
    pState->Gamepad.sThumbLX = to_short(lstick[0]);
    pState->Gamepad.sThumbLY = to_short(lstick[1]);
    pState->Gamepad.sThumbRX = to_short(rstick[0]);
    pState->Gamepad.sThumbRY = to_short(rstick[1]);
    return 1;
}

const XBOX_IOS_TOUCH_ZONE *xbox_ios_touch_zones(int *count)
{
    touch_prefs_once();
    if (count)
        *count = s_zone_n;
    return s_zones;
}

/* Live stick state for the overlay's floating-stick visual: the touch-down
 * point is the base ring, the current position the knob. side: 0 left,
 * 1 right. Returns 1 while a finger drives that stick. */
int xbox_ios_touch_stick(int side, float *bx, float *by, float *kx, float *ky)
{
    int role = side ? ROLE_RSTICK : ROLE_LSTICK;
    int i, found = 0;

    touch_prefs_once();
    SDL_LockMutex(s_lock);
    for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
        if (s_finger[i].id >= 0 && s_finger[i].role == role) {
            *bx = s_finger[i].sx;
            *by = s_finger[i].sy;
            *kx = s_finger[i].x;
            *ky = s_finger[i].y;
            found = 1;
            break;
        }
    }
    SDL_UnlockMutex(s_lock);
    return found;
}

/* Whether any finger is on zone idx (table order): pressed highlight. */
int xbox_ios_touch_zone_pressed(int idx)
{
    int i, hit = 0;

    touch_prefs_once();
    SDL_LockMutex(s_lock);
    for (i = 0; i < TOUCH_MAX_FINGERS; i++)
        if (s_finger[i].id >= 0 && s_finger[i].role >= ROLE_ZONE(0) &&
            s_finger[i].role - ROLE_ZONE(0) == idx) {
            hit = 1;
            break;
        }
    SDL_UnlockMutex(s_lock);
    return hit;
}

/* Current analog value of zone idx for the overlay, from the same helpers the
 * game state uses: 0 when no finger is on it; a pedal (analog pedals on) its
 * level 0..1 (1 = 255); the steering strip the signed steer -1..1 of the first
 * finger on it; any other held zone 1. */
float xbox_ios_touch_zone_value(int idx)
{
    float best = 0.0f;
    int i;

    touch_prefs_once();
    SDL_LockMutex(s_lock);
    if (idx >= 0 && idx < s_zone_n) {
        const XBOX_IOS_TOUCH_ZONE *z = &s_zones[idx];
        for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
            const TouchFinger *f = &s_finger[i];
            float v;
            if (f->id < 0 || f->role != ROLE_ZONE(idx))
                continue;
            if (z->kind == XBOX_IOS_ZONE_STEER) {
                best = steer_value(z, f);
                break;
            }
            /* The game sees the byte; report the same quantized value. */
            v = zone_is_pedal(z) ? (float)pedal_byte(z, f->y) / 255.0f : 1.0f;
            if (v > best)
                best = v;
        }
    }
    SDL_UnlockMutex(s_lock);
    return best;
}

/* Relative-mode indicator for the overlay: the first finger on strip zone idx
 * (the one that steers). *down_x is its touch-down x and *knob_x that x plus
 * the finger's offset clamped to full travel, both normalized to the window.
 * Returns 1 in relative mode while such a finger is held; 0 otherwise
 * (absolute mode, no finger, not a strip), with the outputs untouched. The
 * knob is linear in the finger (the curve is not applied): it marks where the
 * finger is, capped where the steering is at full lock. */
int xbox_ios_touch_steer_knob(int idx, float *down_x, float *knob_x)
{
    int i, found = 0;

    touch_prefs_once();
    SDL_LockMutex(s_lock);
    if (s_steer_rel && idx >= 0 && idx < s_zone_n &&
        s_zones[idx].kind == XBOX_IOS_ZONE_STEER) {
        float travel = STEER_REL_TRAVEL / s_steer_sens;
        for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
            const TouchFinger *f = &s_finger[i];
            float d;
            if (f->id < 0 || f->role != ROLE_ZONE(idx))
                continue;
            d = f->x - f->sx;
            if (!(d >= -travel))
                d = d < 0.0f ? -travel : 0.0f;     /* NaN gives 0 */
            else if (d > travel)
                d = travel;
            if (down_x) *down_x = f->sx;
            if (knob_x) *knob_x = f->sx + d;
            found = 1;
            break;
        }
    }
    SDL_UnlockMutex(s_lock);
    return found;
}

/* Look of zone idx (active table order) for the overlay: one of the
 * XBOX_IOS_STYLE_* values. In the DRIVE layout the LT/RT zones are pedals.
 * Out of range gives XBOX_IOS_STYLE_OTHER. */
int xbox_ios_touch_zone_style(int idx)
{
    const XBOX_IOS_TOUCH_ZONE *z;

    touch_prefs_once();
    if (idx < 0 || idx >= s_zone_n)
        return XBOX_IOS_STYLE_OTHER;
    z = &s_zones[idx];
    switch (z->kind) {
    case XBOX_IOS_ZONE_DPAD:
        return XBOX_IOS_STYLE_DPAD;
    case XBOX_IOS_ZONE_STEER:
        return XBOX_IOS_STYLE_STEER;
    case XBOX_IOS_ZONE_FLAG:
        if (z->value == XBOX_GAMEPAD_BACK)  return XBOX_IOS_STYLE_MENU_BACK;
        if (z->value == XBOX_GAMEPAD_START) return XBOX_IOS_STYLE_MENU_START;
        return XBOX_IOS_STYLE_OTHER;
    case XBOX_IOS_ZONE_ANALOG:
        switch (z->value) {
        case XBOX_BUTTON_A:        return XBOX_IOS_STYLE_FACE_A;
        case XBOX_BUTTON_B:        return XBOX_IOS_STYLE_FACE_B;
        case XBOX_BUTTON_X:        return XBOX_IOS_STYLE_FACE_X;
        case XBOX_BUTTON_Y:        return XBOX_IOS_STYLE_FACE_Y;
        case XBOX_BUTTON_BLACK:    return XBOX_IOS_STYLE_SHOULDER_BLACK;
        case XBOX_BUTTON_WHITE:    return XBOX_IOS_STYLE_SHOULDER_WHITE;
        case XBOX_BUTTON_LTRIGGER: return s_drive ? XBOX_IOS_STYLE_PEDAL_BRAKE
                                                  : XBOX_IOS_STYLE_TRIGGER_L;
        case XBOX_BUTTON_RTRIGGER: return s_drive ? XBOX_IOS_STYLE_PEDAL_GAS
                                                  : XBOX_IOS_STYLE_TRIGGER_R;
        default:                   return XBOX_IOS_STYLE_OTHER;
        }
    default:
        return XBOX_IOS_STYLE_OTHER;
    }
}

/* The D-pad directions held through the D-pad zone right now, for the
 * overlay's arm highlight. Same quadrant rule as fold_finger(), as bits
 * XBOX_IOS_DPAD_BIT_*; 0 when no finger is on the D-pad. */
unsigned xbox_ios_touch_dpad_bits(void)
{
    unsigned bits = 0;
    int i;

    touch_prefs_once();
    SDL_LockMutex(s_lock);
    for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
        const TouchFinger *f = &s_finger[i];
        const XBOX_IOS_TOUCH_ZONE *z;
        float nx, ny;

        if (f->id < 0 || f->role < ROLE_ZONE(0) ||
            f->role - ROLE_ZONE(0) >= s_zone_n)
            continue;
        z = &s_zones[f->role - ROLE_ZONE(0)];
        if (z->kind != XBOX_IOS_ZONE_DPAD)
            continue;
        nx = (f->x - z->cx) / z->rx;
        ny = (f->y - z->cy) / z->ry;
        if (nx * nx + ny * ny > 0.16f) {   /* 0.4^2, as in fold_finger */
            if (ny < -0.4f) bits |= XBOX_IOS_DPAD_BIT_UP;
            if (ny >  0.4f) bits |= XBOX_IOS_DPAD_BIT_DOWN;
            if (nx < -0.4f) bits |= XBOX_IOS_DPAD_BIT_LEFT;
            if (nx >  0.4f) bits |= XBOX_IOS_DPAD_BIT_RIGHT;
        }
    }
    SDL_UnlockMutex(s_lock);
    return bits;
}

/* Generation of the zone table: changes whenever it is rebuilt (layout,
 * size, geometry edits), so the overlay knows when to relay its
 * layers. */
unsigned xbox_ios_touch_layout_gen(void)
{
    touch_prefs_once();
    return __atomic_load_n(&s_layout_gen, __ATOMIC_ACQUIRE);
}

int xbox_ios_touch_get_layout(void)
{
    touch_prefs_once();
    return s_drive ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Layout editor: geometry overrides, text form                         */
/* ------------------------------------------------------------------ */

/* Base geometry of zone idx of the active table: the override, else the
 * active preset. Called with s_lock held; 0 when idx is out of range. */
static int zone_base_locked(int idx, GeomOv *out)
{
    const GeomOv *o;
    const XBOX_IOS_TOUCH_ZONE *p;
    int id;

    if (idx < 0 || idx >= s_zone_n)
        return 0;
    id = s_zones[idx].id;
    o = geom_ov_get(s_drive, id);
    if (o) {
        *out = *o;
        return 1;
    }
    p = !s_drive ? preset_find(s_zones_pad, PAD_COUNT, id)
      : s_drive_classic ? preset_find(s_zones_drive_classic, DRIVE_CLASSIC_COUNT, id)
                        : preset_find(s_zones_drive, DRIVE_NEW_COUNT, id);
    if (!p)
        return 0;
    out->used = 0;
    out->cx = p->cx;
    out->cy = p->cy;
    out->rx = p->rx;
    out->ry = p->ry;
    return 1;
}

int xbox_ios_touch_zone_geom(int idx, float *cx, float *cy, float *rx, float *ry)
{
    GeomOv g;
    int ok;

    touch_prefs_once();
    SDL_LockMutex(s_lock);
    ok = zone_base_locked(idx, &g);
    SDL_UnlockMutex(s_lock);
    if (!ok)
        return 0;
    if (cx) *cx = g.cx;
    if (cy) *cy = g.cy;
    if (rx) *rx = g.rx;
    if (ry) *ry = g.ry;
    return 1;
}

static int geom_same(const GeomOv *a, float cx, float cy, float rx, float ry)
{
    return fabsf(a->cx - cx) < 1e-6f && fabsf(a->cy - cy) < 1e-6f &&
           fabsf(a->rx - rx) < 1e-6f && fabsf(a->ry - ry) < 1e-6f;
}

int xbox_ios_touch_zone_set_geom(int idx, float cx, float cy, float rx, float ry)
{
    GeomOv cur;
    int ok = 0, id, steer;

    touch_prefs_once();
    SDL_LockMutex(s_lock);
    if (zone_base_locked(idx, &cur)) {
        id = s_zones[idx].id;
        steer = (s_zones[idx].kind == XBOX_IOS_ZONE_STEER);
        if (id >= 0 && id < GEOM_IDS) {
            geom_clamp(steer, &cx, &cy, &rx, &ry);
            if (!geom_same(&cur, cx, cy, rx, ry)) {
                GeomOv *o = &s_geom_ov[s_drive ? 1 : 0][id];
                o->used = 1;
                o->cx = cx;
                o->cy = cy;
                o->rx = rx;
                o->ry = ry;
                zones_rebuild_locked();
            }
            ok = 1;
        }
    }
    SDL_UnlockMutex(s_lock);
    return ok;
}

void xbox_ios_touch_layout_reset(int drive)
{
    touch_prefs_once();
    SDL_LockMutex(s_lock);
    memset(s_geom_ov[drive ? 1 : 0], 0, sizeof s_geom_ov[0]);
    zones_rebuild_locked();
    SDL_UnlockMutex(s_lock);
}

/* Append s to buf at *len, counting what does not fit (snprintf style). */
static void text_put(char *buf, size_t n, size_t *len, const char *s)
{
    size_t k = strlen(s);
    if (buf && n > 0 && *len < n - 1) {
        size_t room = n - 1 - *len;
        memcpy(buf + *len, s, k < room ? k : room);
    }
    *len += k;
}

int xbox_ios_touch_layout_format(char *buf, size_t n)
{
    static const char *const names[2] = { "pad", "drive" };
    size_t len = 0;
    int lay, id;
    char line[96];

    touch_prefs_once();
    text_put(buf, n, &len,
             "# nfsu2 touch layout v1: v1 <pad|drive> <zone id> <cx> <cy> <rx> <ry>"
             " (normalized, before the size setting)\n");
    SDL_LockMutex(s_lock);
    for (lay = 0; lay < 2; lay++)
        for (id = 0; id < GEOM_IDS; id++) {
            const GeomOv *o = &s_geom_ov[lay][id];
            if (!o->used)
                continue;
            snprintf(line, sizeof line, "v1 %s %d %.4f %.4f %.4f %.4f\n",
                     names[lay], id, (double)o->cx, (double)o->cy,
                     (double)o->rx, (double)o->ry);
            text_put(buf, n, &len, line);
        }
    SDL_UnlockMutex(s_lock);
    if (buf && n > 0)
        buf[len < n ? len : n - 1] = 0;
    return (int)len;
}

/* One "v1 <layout> <id> <cx> <cy> <rx> <ry>" line into tmp; 1 when applied. */
static int parse_geom_line(const char *line, GeomOv tmp[2][GEOM_IDS])
{
    char lay[8];
    int id, drive, steer, got;
    float cx, cy, rx, ry;
    const XBOX_IOS_TOUCH_ZONE *p;

    got = sscanf(line, " v1 %7s %d %f %f %f %f", lay, &id, &cx, &cy, &rx, &ry);
    if (got != 6)
        return 0;
    if (strcasecmp(lay, "pad") == 0)
        drive = 0;
    else if (strcasecmp(lay, "drive") == 0)
        drive = 1;
    else
        return 0;
    if (id < 0 || id >= GEOM_IDS)
        return 0;
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(rx) || !isfinite(ry))
        return 0;
    /* The new DRIVE table holds every DRIVE id (the classic one is a subset). */
    p = drive ? preset_find(s_zones_drive, DRIVE_NEW_COUNT, id)
              : preset_find(s_zones_pad, PAD_COUNT, id);
    if (!p)
        return 0;
    steer = (p->kind == XBOX_IOS_ZONE_STEER);
    geom_clamp(steer, &cx, &cy, &rx, &ry);
    tmp[drive][id].used = 1;
    tmp[drive][id].cx = cx;
    tmp[drive][id].cy = cy;
    tmp[drive][id].rx = rx;
    tmp[drive][id].ry = ry;
    return 1;
}

int xbox_ios_touch_layout_parse(const char *text)
{
    GeomOv tmp[2][GEOM_IDS];
    const char *p = text;
    int applied = 0;

    if (!text)
        return 0;
    touch_prefs_once();
    memset(tmp, 0, sizeof tmp);
    while (*p) {
        char line[160];
        const char *e = p;
        size_t len;

        while (*e && *e != '\n')
            e++;
        len = (size_t)(e - p);
        if (len >= sizeof line)
            len = sizeof line - 1;   /* an over-long line is garbage anyway */
        memcpy(line, p, len);
        line[len] = 0;
        if (parse_geom_line(line, tmp))
            applied++;
        p = *e ? e + 1 : e;
    }
    SDL_LockMutex(s_lock);
    memcpy(s_geom_ov, tmp, sizeof s_geom_ov);
    zones_rebuild_locked();
    SDL_UnlockMutex(s_lock);
    return applied;
}

/* ------------------------------------------------------------------ */
/* Hardware keyboard                                                   */
/* ------------------------------------------------------------------ */

/* Bit numbers of the keyboard action mask (one bit per Xbox control, built
 * by xbox_ios_keyboard_mask_from_sdl, applied by xbox_ios_keyboard_fold). */
#define KB_DPAD_UP    0
#define KB_DPAD_DOWN  1
#define KB_DPAD_LEFT  2
#define KB_DPAD_RIGHT 3
#define KB_A          4
#define KB_B          5
#define KB_X          6
#define KB_Y          7
#define KB_LB         8    /* Xbox BLACK */
#define KB_RB         9    /* Xbox WHITE */
#define KB_LT         10
#define KB_RT         11
#define KB_START      12
#define KB_BACK       13
#define KB_LS_UP      14
#define KB_LS_DOWN    15
#define KB_LS_LEFT    16
#define KB_LS_RIGHT   17
#define KB_RS_UP      18
#define KB_RS_DOWN    19
#define KB_RS_LEFT    20
#define KB_RS_RIGHT   21

#define KBIT(b) ((int)((mask >> (b)) & 1ull))

/* The keyboard's action mask, from SDL's own key state. SDL's UIKit backend
 * feeds that array from GCKeyboard (it clears it on focus loss), and the
 * game thread only reads bytes of it, which is benign. Keys:
 *   arrows         D-pad                   W / S     gas (RT) / brake (LT)
 *   A / D          left stick X            Return, Space   A
 *   Esc, Backspace B                       Left Shift X;  Left Ctrl Y
 *   Q / E          LB (Black) / RB (White) Tab, P    START
 *   O, ` (grave)   BACK                    I J K L   right stick up/left/down/right */
unsigned long long xbox_ios_keyboard_mask_from_sdl(void)
{
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    unsigned long long m = 0;

    if (!ks)
        return 0;
#define KEYBIT(sc, bit) do { if (ks[sc]) m |= 1ull << (bit); } while (0)
    KEYBIT(SDL_SCANCODE_UP,        KB_DPAD_UP);
    KEYBIT(SDL_SCANCODE_DOWN,      KB_DPAD_DOWN);
    KEYBIT(SDL_SCANCODE_LEFT,      KB_DPAD_LEFT);
    KEYBIT(SDL_SCANCODE_RIGHT,     KB_DPAD_RIGHT);
    KEYBIT(SDL_SCANCODE_W,         KB_RT);
    KEYBIT(SDL_SCANCODE_S,         KB_LT);
    KEYBIT(SDL_SCANCODE_A,         KB_LS_LEFT);
    KEYBIT(SDL_SCANCODE_D,         KB_LS_RIGHT);
    KEYBIT(SDL_SCANCODE_RETURN,    KB_A);
    KEYBIT(SDL_SCANCODE_SPACE,     KB_A);
    KEYBIT(SDL_SCANCODE_ESCAPE,    KB_B);
    KEYBIT(SDL_SCANCODE_BACKSPACE, KB_B);
    KEYBIT(SDL_SCANCODE_LSHIFT,    KB_X);
    KEYBIT(SDL_SCANCODE_LCTRL,     KB_Y);
    KEYBIT(SDL_SCANCODE_Q,         KB_LB);
    KEYBIT(SDL_SCANCODE_E,         KB_RB);
    KEYBIT(SDL_SCANCODE_TAB,       KB_START);
    KEYBIT(SDL_SCANCODE_P,         KB_START);
    KEYBIT(SDL_SCANCODE_O,         KB_BACK);
    KEYBIT(SDL_SCANCODE_GRAVE,     KB_BACK);
    KEYBIT(SDL_SCANCODE_I,         KB_RS_UP);
    KEYBIT(SDL_SCANCODE_J,         KB_RS_LEFT);
    KEYBIT(SDL_SCANCODE_K,         KB_RS_DOWN);
    KEYBIT(SDL_SCANCODE_L,         KB_RS_RIGHT);
#undef KEYBIT
    return m;
}

/* OR the keyboard's actions into *s: buttons add, analog buttons go to 255,
 * and a stick takes full deflection only where it is not already deflected
 * (a thumb on the glass or a tilt keeps priority). */
void xbox_ios_keyboard_fold(XBOX_INPUT_STATE *s, unsigned long long mask)
{
    WORD fl = 0;
    int d;

    if (!s || !mask)
        return;
    if (KBIT(KB_DPAD_UP))    fl |= XBOX_GAMEPAD_DPAD_UP;
    if (KBIT(KB_DPAD_DOWN))  fl |= XBOX_GAMEPAD_DPAD_DOWN;
    if (KBIT(KB_DPAD_LEFT))  fl |= XBOX_GAMEPAD_DPAD_LEFT;
    if (KBIT(KB_DPAD_RIGHT)) fl |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (KBIT(KB_START))      fl |= XBOX_GAMEPAD_START;
    if (KBIT(KB_BACK))       fl |= XBOX_GAMEPAD_BACK;
    s->Gamepad.wButtons |= fl;

    if (KBIT(KB_A))  s->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
    if (KBIT(KB_B))  s->Gamepad.bAnalogButtons[XBOX_BUTTON_B] = 255;
    if (KBIT(KB_X))  s->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
    if (KBIT(KB_Y))  s->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
    if (KBIT(KB_LB)) s->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] = 255;
    if (KBIT(KB_RB)) s->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] = 255;
    if (KBIT(KB_LT)) s->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
    if (KBIT(KB_RT)) s->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;

    d = KBIT(KB_LS_RIGHT) - KBIT(KB_LS_LEFT);
    if (d && !s->Gamepad.sThumbLX)
        s->Gamepad.sThumbLX = (SHORT)(d * 32767);
    d = KBIT(KB_LS_UP) - KBIT(KB_LS_DOWN);   /* Xbox Y grows upward */
    if (d && !s->Gamepad.sThumbLY)
        s->Gamepad.sThumbLY = (SHORT)(d * 32767);
    d = KBIT(KB_RS_RIGHT) - KBIT(KB_RS_LEFT);
    if (d && !s->Gamepad.sThumbRX)
        s->Gamepad.sThumbRX = (SHORT)(d * 32767);
    d = KBIT(KB_RS_UP) - KBIT(KB_RS_DOWN);
    if (d && !s->Gamepad.sThumbRY)
        s->Gamepad.sThumbRY = (SHORT)(d * 32767);
}

#endif /* NFSU2_IOS */
