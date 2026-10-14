/*
 * touch_test.c -- Linux behaviour test for the iOS touch gamepad layer
 * (xboxrecomp/src/input/xinput_ios.c).
 *
 * The layer is driven only through its public API and through synthetic SDL
 * finger / window events (SDL_PushEvent calls event watches synchronously, so
 * every event is applied before the call returns). No video, no window.
 *
 * Zone positions are read from xbox_ios_touch_zones() and
 * xbox_ios_touch_zone_label(), never hard-coded, so edits to the zone table do
 * not break the test. Only the behaviour contract is fixed here: what a press
 * on a zone produces, how a dynamic stick is scaled, what the settings do.
 *
 * Output: one PASS / FAIL / XFAIL line per case, then
 *   touch_test: N passed, M failed[, K known]
 * Exit code = number of FAIL (and unexpected XPASS) cases.
 *
 * Build: see run_touch_test.sh.
 */
#define _GNU_SOURCE
#include <SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xinput_xbox.h"
#include "xinput_ios.h"

/* Public in the .c but not declared in the header (overlay-only helpers). */
int xbox_ios_touch_stick(int side, float *bx, float *by, float *kx, float *ky);
int xbox_ios_touch_zone_pressed(int idx);

/* ------------------------------------------------------------------ */
/* Tiny harness                                                        */
/* ------------------------------------------------------------------ */

#define STICK_FULL   32767
#define STICK_NEAR   150          /* tolerance for "full deflection" */
#define STICK_RADIUS 0.12f        /* of the window width; contract in xinput_ios.c */

static char g_msg[1024];
static int  g_nfail_checks;       /* failed checks in the current case */

static void record_fail(int line, const char *fmt, ...)
{
    char buf[256];
    size_t used = strlen(g_msg);
    va_list ap;
    int n;

    g_nfail_checks++;
    if (g_nfail_checks > 4)       /* the first few reasons are enough */
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    n = snprintf(g_msg + used, sizeof g_msg - used, "%s[line %d] %s",
                 used ? "; " : "", line, buf);
    (void)n;
}

#define CHECK(cond, ...) \
    do { if (!(cond)) record_fail(__LINE__, __VA_ARGS__); } while (0)

typedef void (*case_fn)(void);

static int g_pass, g_fail, g_known;

/* known_reason != NULL: this case fails on the current code because of a
 * real bug. It prints XFAIL (not counted as a failure). If it starts passing
 * it prints XPASS and counts as a failure, so the marker gets removed. */
static void run_case(const char *name, case_fn fn, const char *known_reason)
{
    g_msg[0] = 0;
    g_nfail_checks = 0;
    fn();
    if (known_reason) {
        if (g_nfail_checks) {
            g_known++;
            printf("XFAIL %s\n        known bug: %s\n        observed: %s\n",
                   name, known_reason, g_msg);
        } else {
            g_fail++;
            printf("FAIL  %s\n        unexpected pass of a known-failure case; remove the marker\n",
                   name);
        }
        return;
    }
    if (g_nfail_checks) {
        g_fail++;
        printf("FAIL  %s\n        %s%s\n", name, g_msg,
               g_nfail_checks > 4 ? " (more...)" : "");
    } else {
        g_pass++;
        printf("PASS  %s\n", name);
    }
}

/* ------------------------------------------------------------------ */
/* Synthetic input                                                     */
/* ------------------------------------------------------------------ */

static void push(SDL_Event *e)
{
    int r = SDL_PushEvent(e);
    if (r < 0) {
        fprintf(stderr, "touch_test: SDL_PushEvent failed: %s\n", SDL_GetError());
        exit(99);
    }
}

static void finger(Uint32 type, int id, float x, float y)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.tfinger.touchId = 1;
    e.tfinger.fingerId = (SDL_FingerID)id;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.pressure = 1.0f;
    push(&e);
}

static void fdown(int id, float x, float y) { finger(SDL_FINGERDOWN, id, x, y); }
static void fmove(int id, float x, float y) { finger(SDL_FINGERMOTION, id, x, y); }
static void fup(int id)                     { finger(SDL_FINGERUP, id, 0.0f, 0.0f); }

static void wevent(int ev, int w, int h)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_WINDOWEVENT;
    e.window.event = (Uint8)ev;
    e.window.data1 = w;
    e.window.data2 = h;
    push(&e);
}

static void wsize(int w, int h) { wevent(SDL_WINDOWEVENT_SIZE_CHANGED, w, h); }

#define WIN_ASPECT (720.0f / 1280.0f)

static void lift_all(void)
{
    int i;
    for (i = 0; i < 16; i++)
        fup(i);
}

/* Back to a known state: touch on, PAD layout, scale 1, no tilt, no hooks,
 * analog pedals on, steering strip on at sensitivity 1 in the shipping mode
 * (relative, curve 1.35), new DRIVE table, no
 * geometry overrides, no safe area, aspect unknown, round shape on, tablet
 * factor 1.0, no fingers, 16:9 window. The setters clear all fingers by contract; the
 * explicit lift keeps reset() sound even if that contract breaks. */
static void reset(void)
{
    xbox_ios_touch_set_press_hook(NULL);
    xbox_ios_touch_set_activity_hook(NULL);
    xbox_ios_touch_set_enabled(1);
    xbox_ios_touch_set_analog_pedals(1);
    xbox_ios_touch_set_steer(1);
    xbox_ios_touch_set_steer_sens(1.0f);
    xbox_ios_touch_set_steer_mode(1);
    xbox_ios_touch_set_steer_curve(1.35f);
    xbox_ios_touch_set_drive_classic(0);
    xbox_ios_touch_layout_reset(1);
    xbox_ios_touch_layout_reset(0);
    xbox_ios_touch_set_layout(0);
    xbox_ios_touch_set_scale(1.0f);
    xbox_ios_touch_set_safe_enabled(1);
    xbox_ios_touch_set_safe_area(0.0f, 0.0f);
    xbox_ios_touch_set_aspect(0.0f);
    xbox_ios_touch_set_round(1);
    xbox_ios_touch_set_tablet_scale(1.0f);
    xbox_ios_touch_set_tilt(0.0f, 0);
    lift_all();
    wsize(1280, 720);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

/* ------------------------------------------------------------------ */
/* State access and zone lookup                                        */
/* ------------------------------------------------------------------ */

static XBOX_INPUT_STATE rd(void)
{
    XBOX_INPUT_STATE s;
    memset(&s, 0x5A, sizeof s);
    if (!xbox_ios_touch_state(&s)) {
        record_fail(__LINE__, "xbox_ios_touch_state returned 0 (layer unavailable)");
        memset(&s, 0, sizeof s);
    }
    return s;
}

static int zcount(void)
{
    int n = 0;
    xbox_ios_touch_zones(&n);
    return n;
}

static const XBOX_IOS_TOUCH_ZONE *zone(int i)
{
    int n = 0;
    const XBOX_IOS_TOUCH_ZONE *z = xbox_ios_touch_zones(&n);
    return (i >= 0 && i < n) ? &z[i] : NULL;
}

/* One zone's shape test, same convention as the layer: inclusive, an ellipse
 * for every zone but the steering strip, which is a rectangle. */
static int in_shape(const XBOX_IOS_TOUCH_ZONE *z, float x, float y)
{
    float dx = (x - z->cx) / z->rx;
    float dy = (y - z->cy) / z->ry;
    if (z->kind == XBOX_IOS_ZONE_STEER)
        return fabsf(dx) <= 1.0f && fabsf(dy) <= 1.0f;
    return dx * dx + dy * dy <= 1.0f;
}

static int zone_hit(float x, float y)
{
    int n = 0, i;
    const XBOX_IOS_TOUCH_ZONE *z = xbox_ios_touch_zones(&n);
    for (i = 0; i < n; i++)
        if (in_shape(&z[i], x, y))
            return i;
    return -1;
}

static int zone_by_label(const char *label)
{
    int n = zcount(), i, found = -1;
    for (i = 0; i < n; i++) {
        const char *l = xbox_ios_touch_zone_label(i);
        if (l && strcmp(l, label) == 0) {
            if (found >= 0)
                record_fail(__LINE__, "label \"%s\" is on more than one zone", label);
            found = i;
        }
    }
    if (found < 0)
        record_fail(__LINE__, "no zone labelled \"%s\"", label);
    return found;
}

static int zone_by_flag(int flag)
{
    int n = zcount(), i;
    for (i = 0; i < n; i++) {
        const XBOX_IOS_TOUCH_ZONE *z = zone(i);
        if (z->kind == XBOX_IOS_ZONE_FLAG && z->value == flag)
            return i;
    }
    record_fail(__LINE__, "no FLAG zone for wButtons bit 0x%x", flag);
    return -1;
}

/* Like zone_by_kind, but a missing zone is a result, not a failure. */
static int zone_find_kind(int kind)
{
    int n = zcount(), i;
    for (i = 0; i < n; i++)
        if (zone(i)->kind == kind)
            return i;
    return -1;
}

static int zone_by_kind(int kind)
{
    int n = zcount(), i;
    for (i = 0; i < n; i++)
        if (zone(i)->kind == kind)
            return i;
    record_fail(__LINE__, "no zone of kind %d", kind);
    return -1;
}

/* A point that must be bare (no zone) so a touch there is a stick touch. */
static void require_bare(float x, float y)
{
    int h = zone_hit(x, y);
    CHECK(h < 0, "test setup: (%.3f,%.3f) is inside zone %d, not bare", x, y, h);
}

/* Touch-down on a point that must be a stick touch (no zone there). */
static void fdown_bare(int id, float x, float y)
{
    require_bare(x, y);
    fdown(id, x, y);
}

/* ------------------------------------------------------------------ */
/* Assertions on a state                                               */
/* ------------------------------------------------------------------ */

static void expect_sticks(const XBOX_INPUT_STATE *s, int lx, int ly, int rx, int ry,
                          int tol, const char *what)
{
    CHECK(abs(s->Gamepad.sThumbLX - lx) <= tol, "%s: LX=%d, want %d", what, s->Gamepad.sThumbLX, lx);
    CHECK(abs(s->Gamepad.sThumbLY - ly) <= tol, "%s: LY=%d, want %d", what, s->Gamepad.sThumbLY, ly);
    CHECK(abs(s->Gamepad.sThumbRX - rx) <= tol, "%s: RX=%d, want %d", what, s->Gamepad.sThumbRX, rx);
    CHECK(abs(s->Gamepad.sThumbRY - ry) <= tol, "%s: RY=%d, want %d", what, s->Gamepad.sThumbRY, ry);
}

static void expect_idle(const XBOX_INPUT_STATE *s, const char *what)
{
    int i;
    CHECK(s->Gamepad.wButtons == 0, "%s: wButtons=0x%x, want 0", what, s->Gamepad.wButtons);
    for (i = 0; i < 8; i++)
        CHECK(s->Gamepad.bAnalogButtons[i] == 0, "%s: analog[%d]=%d, want 0",
              what, i, s->Gamepad.bAnalogButtons[i]);
    expect_sticks(s, 0, 0, 0, 0, 0, what);
}

/* Exactly one analog button at 255, all else idle. */
static void expect_only_analog(const XBOX_INPUT_STATE *s, int button, const char *what)
{
    int i;
    CHECK(s->Gamepad.wButtons == 0, "%s: wButtons=0x%x, want 0", what, s->Gamepad.wButtons);
    for (i = 0; i < 8; i++)
        CHECK(s->Gamepad.bAnalogButtons[i] == (i == button ? 255 : 0),
              "%s: analog[%d]=%d, want %d", what, i, s->Gamepad.bAnalogButtons[i],
              i == button ? 255 : 0);
    expect_sticks(s, 0, 0, 0, 0, 0, what);
}

/* ------------------------------------------------------------------ */
/* Cases                                                               */
/* ------------------------------------------------------------------ */

/* Press-hook recorder. */
static int touch_test_hook_calls;
static int s_hook_zone[32];
static void touch_test_count_hook(int zone_idx)
{
    if (touch_test_hook_calls < 32)
        s_hook_zone[touch_test_hook_calls] = zone_idx;
    touch_test_hook_calls++;
}

typedef struct { const char *label; int button; } PadButton;

static const PadButton k_pad_buttons[] = {
    { "A",  XBOX_BUTTON_A },        { "B",  XBOX_BUTTON_B },
    { "X",  XBOX_BUTTON_X },        { "Y",  XBOX_BUTTON_Y },
    { "LB", XBOX_BUTTON_BLACK },    { "RB", XBOX_BUTTON_WHITE },
    { "LT", XBOX_BUTTON_LTRIGGER }, { "RT", XBOX_BUTTON_RTRIGGER },
};

/* DRIVE (function names, Control Type 1). The shift arrows are U+2191 / U+2193
 * in UTF-8. */
#define LBL_SHIFT_UP "SHIFT\xE2\x86\x91"
#define LBL_SHIFT_DN "SHIFT\xE2\x86\x93"
static const PadButton k_drive_buttons[] = {
    { "E-BRAKE",    XBOX_BUTTON_A },     { "NOS",        XBOX_BUTTON_B },
    { LBL_SHIFT_UP, XBOX_BUTTON_X },     { LBL_SHIFT_DN, XBOX_BUTTON_Y },
    { "LOOK",       XBOX_BUTTON_BLACK }, { "CAM",        XBOX_BUTTON_WHITE },
};

/* The contract of the packet number: moves on every read; the rest of the
 * struct is the layer's, whatever the caller had in it. */
static void t_state_contract(void)
{
    XBOX_INPUT_STATE a, b;
    reset();
    a = rd();
    b = rd();
    CHECK(b.dwPacketNumber != a.dwPacketNumber, "packet number did not change between reads (%u)",
          (unsigned)a.dwPacketNumber);
    CHECK(b.dwPacketNumber == a.dwPacketNumber + 1, "packet number step is %u, want 1",
          (unsigned)(b.dwPacketNumber - a.dwPacketNumber));
    expect_idle(&a, "garbage-filled struct after idle read");
    CHECK(!xbox_ios_touch_state(NULL), "state(NULL) should return 0");
}

/* 1. PAD: every analog zone centre presses exactly its button. */
static void t_pad_analog(void)
{
    size_t k;
    reset();
    for (k = 0; k < sizeof k_pad_buttons / sizeof k_pad_buttons[0]; k++) {
        const PadButton *pb = &k_pad_buttons[k];
        int zi = zone_by_label(pb->label);
        const XBOX_IOS_TOUCH_ZONE *z = zone(zi);
        XBOX_INPUT_STATE s;
        char what[64];
        if (!z)
            continue;
        snprintf(what, sizeof what, "%s down", pb->label);
        CHECK(z->kind == XBOX_IOS_ZONE_ANALOG, "%s: zone kind %d, want ANALOG", pb->label, z->kind);
        CHECK(z->value == pb->button, "%s: zone value %d, want XBOX_BUTTON %d",
              pb->label, z->value, pb->button);
        CHECK(zone_hit(z->cx, z->cy) == zi, "%s: centre is shadowed by zone %d",
              pb->label, zone_hit(z->cx, z->cy));
        fdown(1, z->cx, z->cy);
        s = rd();
        expect_only_analog(&s, pb->button, what);
        CHECK(xbox_ios_touch_zone_pressed(zi), "%s: zone_pressed() is 0 while held", pb->label);
        fup(1);
        s = rd();
        snprintf(what, sizeof what, "%s up", pb->label);
        expect_idle(&s, what);
        CHECK(!xbox_ios_touch_zone_pressed(zi), "%s: zone_pressed() still 1 after lift", pb->label);
    }
}

/* The zone is held until the finger lifts, wherever the finger drifts. */
static void t_zone_latched(void)
{
    int zi;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    reset();
    zi = zone_by_label("A");
    z = zone(zi);
    if (!z)
        return;
    fdown(1, z->cx, z->cy);
    fmove(1, 0.5f, 0.5f);               /* far outside every zone */
    s = rd();
    expect_only_analog(&s, XBOX_BUTTON_A, "A after the finger drifted away");
    fup(1);
}

/* Two buttons at once (A + RT), each lifts on its own. */
static void t_pad_multitouch(void)
{
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    const XBOX_IOS_TOUCH_ZONE *rt = zone(zone_by_label("RT"));
    XBOX_INPUT_STATE s;
    reset();
    if (!a || !rt)
        return;
    fdown(1, a->cx, a->cy);
    fdown(2, rt->cx, rt->cy);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not held");
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "RT not held");
    fup(1);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0, "A still held after its lift");
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255, "RT dropped by A's lift");
    fup(2);
    s = rd();
    expect_idle(&s, "both lifted");
}

/* 2. BACK / START set their wButtons flag and nothing else. */
static void t_flags(void)
{
    static const int flags[] = { XBOX_GAMEPAD_BACK, XBOX_GAMEPAD_START };
    size_t k;
    reset();
    for (k = 0; k < 2; k++) {
        int zi = zone_by_flag(flags[k]);
        const XBOX_IOS_TOUCH_ZONE *z = zone(zi);
        XBOX_INPUT_STATE s;
        int i;
        if (!z)
            continue;
        CHECK(zone_hit(z->cx, z->cy) == zi, "flag 0x%x: centre shadowed by zone %d",
              flags[k], zone_hit(z->cx, z->cy));
        fdown(1, z->cx, z->cy);
        s = rd();
        CHECK(s.Gamepad.wButtons == flags[k], "flag 0x%x: wButtons=0x%x", flags[k], s.Gamepad.wButtons);
        for (i = 0; i < 8; i++)
            CHECK(s.Gamepad.bAnalogButtons[i] == 0, "flag 0x%x: analog[%d]=%d",
                  flags[k], i, s.Gamepad.bAnalogButtons[i]);
        fup(1);
        s = rd();
        expect_idle(&s, "flag zone up");
    }
}

/* 3. D-pad: offsets of 0.7 radius, cardinals and diagonals; centre neutral. */
static void t_dpad(void)
{
    int zi;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    size_t k;
    static const struct { float ox, oy; int want; const char *name; } dirs[] = {
        {  0.0f, -0.7f, XBOX_GAMEPAD_DPAD_UP,                          "up" },
        {  0.0f,  0.7f, XBOX_GAMEPAD_DPAD_DOWN,                        "down" },
        { -0.7f,  0.0f, XBOX_GAMEPAD_DPAD_LEFT,                        "left" },
        {  0.7f,  0.0f, XBOX_GAMEPAD_DPAD_RIGHT,                       "right" },
        { -0.7f, -0.7f, XBOX_GAMEPAD_DPAD_UP | XBOX_GAMEPAD_DPAD_LEFT,    "up-left" },
        {  0.7f, -0.7f, XBOX_GAMEPAD_DPAD_UP | XBOX_GAMEPAD_DPAD_RIGHT,   "up-right" },
        { -0.7f,  0.7f, XBOX_GAMEPAD_DPAD_DOWN | XBOX_GAMEPAD_DPAD_LEFT,  "down-left" },
        {  0.7f,  0.7f, XBOX_GAMEPAD_DPAD_DOWN | XBOX_GAMEPAD_DPAD_RIGHT, "down-right" },
    };
    reset();
    zi = zone_by_kind(XBOX_IOS_ZONE_DPAD);
    z = zone(zi);
    if (!z)
        return;

    fdown(1, z->cx, z->cy);
    s = rd();
    expect_idle(&s, "d-pad centre");
    CHECK(xbox_ios_touch_zone_pressed(zi), "d-pad centre: zone_pressed() is 0");
    fup(1);

    for (k = 0; k < sizeof dirs / sizeof dirs[0]; k++) {
        float x = z->cx + dirs[k].ox * z->rx;
        float y = z->cy + dirs[k].oy * z->ry;
        char what[48];
        CHECK(zone_hit(x, y) == zi, "d-pad %s point lands on zone %d, not the d-pad",
              dirs[k].name, zone_hit(x, y));
        fdown(1, x, y);
        s = rd();
        CHECK(s.Gamepad.wButtons == dirs[k].want, "d-pad %s: wButtons=0x%x, want 0x%x",
              dirs[k].name, s.Gamepad.wButtons, dirs[k].want);
        snprintf(what, sizeof what, "d-pad %s analog/stick", dirs[k].name);
        s.Gamepad.wButtons = 0;
        expect_idle(&s, what);
        fup(1);
    }

    /* A small offset inside the neutral radius stays neutral. */
    fdown(1, z->cx + 0.2f * z->rx, z->cy - 0.2f * z->ry);
    s = rd();
    expect_idle(&s, "d-pad small offset");
    fmove(1, z->cx, z->cy - 0.7f * z->ry);   /* sliding while held re-reads the offset */
    s = rd();
    CHECK(s.Gamepad.wButtons == XBOX_GAMEPAD_DPAD_UP, "d-pad slide up: wButtons=0x%x",
          s.Gamepad.wButtons);
    fup(1);
}

/* 4a. Left stick, all four directions, deadzone, half travel, clamp. */
static void t_left_stick(void)
{
    const float sx = 0.30f, sy = 0.50f, R = STICK_RADIUS;
    XBOX_INPUT_STATE s;
    reset();
    require_bare(sx, sy);
    fdown_bare(1, sx, sy);
    s = rd();
    expect_idle(&s, "left stick, finger just down");

    fmove(1, sx + R, sy);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "left stick, full right");
    fmove(1, sx - R, sy);
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, 0, 0, STICK_NEAR, "left stick, full left");

    /* Y: finger UP the screen = positive. Full travel is R of the width in
     * pixels, which is R / WIN_ASPECT in normalized y (y is height-relative). */
    fmove(1, sx, sy - R / WIN_ASPECT);
    s = rd();
    expect_sticks(&s, 0, STICK_FULL, 0, 0, STICK_NEAR, "left stick, full up");
    fmove(1, sx, sy + R / WIN_ASPECT);
    s = rd();
    expect_sticks(&s, 0, -STICK_FULL, 0, 0, STICK_NEAR, "left stick, full down");

    /* Back to the start: zero. A resting thumb (below 10% of travel) too. */
    fmove(1, sx, sy);
    s = rd();
    expect_idle(&s, "left stick, back at the start");
    fmove(1, sx + 0.08f * R, sy);
    s = rd();
    expect_idle(&s, "left stick, inside the deadzone");

    /* 55% of travel: (0.55 - 0.10) / 0.90 = 50% deflection. */
    fmove(1, sx + 0.55f * R, sy);
    s = rd();
    expect_sticks(&s, STICK_FULL / 2, 0, 0, 0, 300, "left stick, 55% travel");

    /* Overshoot clamps; the diagonal has unit magnitude, not sqrt(2). */
    fmove(1, sx + 0.5f, sy);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "left stick, 4x overshoot");
    fmove(1, sx + R * 0.7071f, sy - R * 0.7071f / WIN_ASPECT);
    s = rd();
    expect_sticks(&s, (int)(STICK_FULL * 0.7071f), (int)(STICK_FULL * 0.7071f), 0, 0,
                  300, "left stick, full diagonal");

    fup(1);
    s = rd();
    expect_idle(&s, "left stick, lifted");
}

/* 4b. Right stick on the bare right half; independent of the left. */
static void t_right_stick(void)
{
    const float sx = 0.60f, sy = 0.50f, R = STICK_RADIUS;
    XBOX_INPUT_STATE s;
    reset();
    require_bare(sx, sy);
    require_bare(0.30f, 0.50f);
    fdown_bare(1, sx, sy);
    s = rd();
    expect_idle(&s, "right stick, finger just down");
    fmove(1, sx + R, sy);
    s = rd();
    expect_sticks(&s, 0, 0, STICK_FULL, 0, STICK_NEAR, "right stick, full right");
    fmove(1, sx - R, sy);
    s = rd();
    expect_sticks(&s, 0, 0, -STICK_FULL, 0, STICK_NEAR, "right stick, full left");
    fmove(1, sx, sy - R / WIN_ASPECT);
    s = rd();
    expect_sticks(&s, 0, 0, 0, STICK_FULL, STICK_NEAR, "right stick, full up");
    fmove(1, sx, sy + R / WIN_ASPECT);
    s = rd();
    expect_sticks(&s, 0, 0, 0, -STICK_FULL, STICK_NEAR, "right stick, full down");
    fmove(1, sx, sy);
    s = rd();
    expect_idle(&s, "right stick, back at the start");

    /* Both sticks at once. */
    fdown_bare(2, 0.30f, 0.50f);
    fmove(2, 0.30f - R, 0.50f);
    fmove(1, sx + R, sy);
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, STICK_FULL, 0, STICK_NEAR, "both sticks");
    fup(2);
    s = rd();
    expect_sticks(&s, 0, 0, STICK_FULL, 0, STICK_NEAR, "left lifted, right held");
    fup(1);
    s = rd();
    expect_idle(&s, "right stick, lifted");
}

/* The stick radius is a fraction of the width, so the vertical travel in
 * normalized units depends on the window aspect, and follows resizes. */
static void t_stick_aspect(void)
{
    const float sx = 0.30f, sy = 0.50f, R = STICK_RADIUS;
    XBOX_INPUT_STATE s;
    reset();
    fdown_bare(1, sx, sy);

    wsize(1000, 1000);                       /* square: aspect 1 */
    fmove(1, sx, sy - R);
    s = rd();
    expect_sticks(&s, 0, STICK_FULL, 0, 0, STICK_NEAR, "SIZE_CHANGED 1000x1000, up by R");

    wevent(SDL_WINDOWEVENT_RESIZED, 2000, 1000);   /* aspect 0.5 */
    fmove(1, sx, sy - R / 0.5f);
    s = rd();
    expect_sticks(&s, 0, STICK_FULL, 0, 0, STICK_NEAR, "RESIZED 2000x1000, up by 2R");

    wsize(1280, 720);
    fmove(1, sx, sy - 0.5f * R / WIN_ASPECT);   /* half travel: partial deflection */
    s = rd();
    CHECK(s.Gamepad.sThumbLY > 0 && s.Gamepad.sThumbLY < STICK_FULL - 1000,
          "back to 16:9: LY=%d, want partial deflection", s.Gamepad.sThumbLY);

    wsize(0, 0);                             /* bogus sizes are ignored */
    fmove(1, sx, sy - R / WIN_ASPECT);
    s = rd();
    expect_sticks(&s, 0, STICK_FULL, 0, 0, STICK_NEAR, "bogus 0x0 resize ignored");
    fup(1);
}

/* 5. Two fingers on the left half: lifting the driver hands the stick to the
 * waiting finger, re-centred under it. */
static void t_stick_handover(void)
{
    const float R = STICK_RADIUS;
    XBOX_INPUT_STATE s;
    reset();
    require_bare(0.30f, 0.50f);
    require_bare(0.38f, 0.62f);
    fdown_bare(1, 0.30f, 0.50f);
    fdown_bare(2, 0.38f, 0.62f);                  /* waits: the stick is taken */
    fmove(2, 0.40f, 0.65f);                  /* the waiting finger does nothing */
    s = rd();
    expect_idle(&s, "two fingers, none moved");
    fmove(1, 0.30f + R, 0.50f);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "finger 1 drives, finger 2 waits");

    fup(1);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "right after promotion of finger 2");
    fmove(2, 0.40f + R, 0.65f);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "promoted finger 2 drives");
    fup(2);
    s = rd();
    expect_idle(&s, "all lifted");

    /* A finger waiting on the other half is not promoted to this stick. */
    fdown_bare(1, 0.30f, 0.50f);                  /* left stick */
    fdown_bare(2, 0.60f, 0.50f);                  /* right stick */
    fdown_bare(3, 0.70f, 0.30f);                  /* right waiter */
    fmove(3, 0.70f + R, 0.30f);
    fup(1);
    fmove(2, 0.60f + R, 0.50f);
    s = rd();
    expect_sticks(&s, 0, 0, STICK_FULL, 0, STICK_NEAR, "left lifted: right waiter must not take the left");
    fup(2);                                  /* right waiter takes over, re-centred */
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "right waiter promoted");
    fup(3);
    s = rd();
    expect_idle(&s, "all lifted 2");
}

/* A finger that lifts without ever being a stick or zone changes nothing. */
static void t_up_unknown_finger(void)
{
    XBOX_INPUT_STATE s;
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    reset();
    if (!a)
        return;
    fdown(1, a->cx, a->cy);
    fup(7);                                  /* never went down */
    fmove(8, 0.3f, 0.3f);                    /* never went down */
    s = rd();
    expect_only_analog(&s, XBOX_BUTTON_A, "A held, stray up/move events");
    fup(1);
}

/* 6. Touch off. */
static void t_touch_off(void)
{
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    const XBOX_IOS_TOUCH_ZONE *b = zone(zone_by_label("B"));
    XBOX_INPUT_STATE s;
    int r;
    reset();
    if (!a || !b)
        return;
    CHECK(xbox_ios_touch_enabled() == 1, "enabled() = %d after reset, want 1", xbox_ios_touch_enabled());

    fdown(1, a->cx, a->cy);                  /* held while the layer goes off */
    xbox_ios_touch_set_enabled(0);
    CHECK(xbox_ios_touch_enabled() == 0, "enabled() = %d after set_enabled(0)", xbox_ios_touch_enabled());
    memset(&s, 0xA5, sizeof s);
    r = xbox_ios_touch_state(&s);
    CHECK(r == 0, "state() returned %d while off, want 0", r);
    fdown(2, b->cx, b->cy);                  /* ignored while off */
    r = xbox_ios_touch_state(&s);
    CHECK(r == 0, "state() returned %d with a finger down while off, want 0", r);

    xbox_ios_touch_set_enabled(1);
    CHECK(xbox_ios_touch_enabled() == 1, "enabled() = %d after set_enabled(1)", xbox_ios_touch_enabled());
    s = rd();
    expect_idle(&s, "back on: the finger held across the off period is gone, the new one was ignored");
    fup(1);
    fup(2);

    fdown(3, b->cx, b->cy);
    s = rd();
    expect_only_analog(&s, XBOX_BUTTON_B, "back on, new finger on B");
    fup(3);
}

/* Off means off: no press hook either; tilt does not survive an off/on. */
static void t_touch_off_side_effects(void)
{
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    XBOX_INPUT_STATE s;
    reset();
    if (!a)
        return;
    touch_test_hook_calls = 0;
    xbox_ios_touch_set_press_hook(touch_test_count_hook);
    xbox_ios_touch_set_enabled(0);
    fdown(1, a->cx, a->cy);
    CHECK(touch_test_hook_calls == 0, "press hook fired %d times while off", touch_test_hook_calls);
    xbox_ios_touch_set_press_hook(NULL);
    xbox_ios_touch_set_enabled(1);
    fup(1);

    xbox_ios_touch_set_tilt(0.5f, 1);
    xbox_ios_touch_set_enabled(0);
    xbox_ios_touch_set_enabled(1);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "tilt after off/on");
}

/* 7. Layout: DRIVE has GAS / BRAKE and the function-named buttons (changed
 * with the DRIVE redesign: it used to look for the pad labels A / B / X / Y /
 * LB / RB in DRIVE); switching clears held fingers. */
static void t_layout_drive(void)
{
    XBOX_INPUT_STATE s;
    int gas, brake;
    const XBOX_IOS_TOUCH_ZONE *z;
    int pad_n;
    reset();
    pad_n = zcount();
    CHECK(pad_n > 0, "no zones in PAD layout");
    CHECK(zone_by_label("RT") >= 0 && zone_by_label("LT") >= 0, "PAD layout lacks LT/RT");

    xbox_ios_touch_set_layout(1);
    gas = zone_by_label("GAS");
    brake = zone_by_label("BRAKE");
    {
        int n = zcount(), i;
        for (i = 0; i < n; i++) {
            const char *l = xbox_ios_touch_zone_label(i);
            CHECK(!(l && (!strcmp(l, "RT") || !strcmp(l, "LT"))), "DRIVE still has a \"%s\" zone", l);
        }
    }
    z = zone(gas);
    if (z) {
        CHECK(zone_hit(z->cx, z->cy) == gas, "GAS centre shadowed by zone %d", zone_hit(z->cx, z->cy));
        fdown(1, z->cx, z->cy);
        s = rd();
        expect_only_analog(&s, XBOX_BUTTON_RTRIGGER, "GAS down");
        fup(1);
        s = rd();
        expect_idle(&s, "GAS up");
    }
    z = zone(brake);
    if (z) {
        CHECK(zone_hit(z->cx, z->cy) == brake, "BRAKE centre shadowed by zone %d", zone_hit(z->cx, z->cy));
        fdown(1, z->cx, z->cy);
        s = rd();
        expect_only_analog(&s, XBOX_BUTTON_LTRIGGER, "BRAKE down");
        fup(1);
    }

    /* The face buttons and shoulders keep their buttons under the DRIVE
     * function names (E-BRAKE = A, NOS = B, SHIFT up / down = X / Y, LOOK =
     * Black, CAM = White); BACK / START, the d-pad and the strip are there. */
    {
        size_t k;
        int n = zcount(), i;
        for (i = 0; i < n; i++) {
            const char *l = xbox_ios_touch_zone_label(i);
            CHECK(!(l && (!strcmp(l, "A") || !strcmp(l, "B") || !strcmp(l, "X") ||
                          !strcmp(l, "Y") || !strcmp(l, "LB") || !strcmp(l, "RB"))),
                  "DRIVE still has the pad label \"%s\"", l);
        }
        for (k = 0; k < sizeof k_drive_buttons / sizeof k_drive_buttons[0]; k++) {
            const char *l = k_drive_buttons[k].label;
            z = zone(zone_by_label(l));
            if (!z)
                continue;
            CHECK(zone_hit(z->cx, z->cy) == zone_by_label(l), "%s centre is shadowed", l);
            fdown(1, z->cx, z->cy);
            s = rd();
            expect_only_analog(&s, k_drive_buttons[k].button, l);
            fup(1);
        }
        zone_by_flag(XBOX_GAMEPAD_BACK);
        zone_by_flag(XBOX_GAMEPAD_START);
        zone_by_kind(XBOX_IOS_ZONE_DPAD);
        zone_by_kind(XBOX_IOS_ZONE_STEER);
    }

    /* Back to PAD restores the old table. */
    xbox_ios_touch_set_layout(0);
    CHECK(zcount() == pad_n, "zone count %d after returning to PAD, was %d", zcount(), pad_n);
    zone_by_label("RT");
    zone_by_label("LT");
}

static void t_layout_clears_fingers(void)
{
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    XBOX_INPUT_STATE s;
    reset();
    if (!a)
        return;
    fdown(1, a->cx, a->cy);                  /* a zone finger */
    fdown_bare(2, 0.30f, 0.50f);                  /* a stick finger */
    fmove(2, 0.30f + STICK_RADIUS, 0.50f);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 && s.Gamepad.sThumbLX > 0,
          "test setup: A and stick not held");
    xbox_ios_touch_set_layout(1);
    s = rd();
    expect_idle(&s, "after set_layout(1) with fingers held");
    fup(1);
    fup(2);

    fdown_bare(1, 0.30f, 0.50f);
    xbox_ios_touch_set_layout(0);
    s = rd();
    expect_idle(&s, "after set_layout(0) with a stick finger held");
    fup(1);

    /* set_scale clears too. */
    fdown(1, a->cx, a->cy);
    xbox_ios_touch_set_scale(1.2f);
    s = rd();
    expect_idle(&s, "after set_scale with a zone finger held");
    fup(1);
}

/* 8. Scale: zones grow and shrink around their centres; clamped 0.7..1.5. */
static void t_scale(void)
{
    int zi;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_IOS_TOUCH_ZONE base;
    XBOX_INPUT_STATE s;
    float px, py;
    reset();
    zi = zone_by_label("A");
    z = zone(zi);
    if (!z)
        return;
    base = *z;

    /* Just outside the radius at 1.0 (1.2 radii, straight down). */
    px = base.cx;
    py = base.cy + 1.2f * base.ry;
    CHECK(py < 1.0f, "test setup: probe y %.3f is off screen", py);
    CHECK(zone_hit(px, py) != zi, "test setup: probe is inside A at scale 1.0");
    fdown(1, px, py);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0, "A pressed by a point outside its 1.0x radius");
    fup(1);

    xbox_ios_touch_set_scale(1.5f);
    z = zone(zi);
    CHECK(fabsf(z->rx - base.rx * 1.5f) < 1e-5f && fabsf(z->ry - base.ry * 1.5f) < 1e-5f,
          "radii at 1.5: %.4f,%.4f want %.4f,%.4f", z->rx, z->ry, base.rx * 1.5f, base.ry * 1.5f);
    CHECK(z->cx == base.cx && z->cy == base.cy, "centre moved with the scale");
    CHECK(zone_hit(px, py) == zi, "test setup: probe is not inside A at 1.5x (zone %d)", zone_hit(px, py));
    fdown(1, px, py);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not pressed at 1.5x by a point inside its radius");
    fup(1);

    /* Shrinking: a point at 0.85 radii hits at 1.0, misses at 0.7. */
    xbox_ios_touch_set_scale(1.0f);
    px = base.cx;
    py = base.cy - 0.85f * base.ry;
    fdown(1, px, py);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not pressed at 0.85 radii, scale 1.0");
    fup(1);
    xbox_ios_touch_set_scale(0.7f);
    fdown(1, px, py);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0, "A pressed at 0.85 radii, scale 0.7");
    fup(1);

    /* Clamps. */
    xbox_ios_touch_set_scale(9.0f);
    z = zone(zi);
    CHECK(fabsf(z->rx - base.rx * 1.5f) < 1e-5f, "scale 9.0 not clamped to 1.5 (rx %.4f)", z->rx);
    xbox_ios_touch_set_scale(0.1f);
    z = zone(zi);
    CHECK(fabsf(z->rx - base.rx * 0.7f) < 1e-5f, "scale 0.1 not clamped to 0.7 (rx %.4f)", z->rx);
    xbox_ios_touch_set_scale(-3.0f);
    z = zone(zi);
    CHECK(fabsf(z->rx - base.rx * 0.7f) < 1e-5f, "scale -3 not clamped to 0.7 (rx %.4f)", z->rx);
    xbox_ios_touch_set_scale(NAN);
    z = zone(zi);
    CHECK(fabsf(z->rx - base.rx * 0.7f) < 1e-5f, "scale NaN not clamped to 0.7 (rx %.4f)", z->rx);
    xbox_ios_touch_set_scale(1.0f);
    z = zone(zi);
    CHECK(fabsf(z->rx - base.rx) < 1e-5f, "scale 1.0 does not restore rx (%.4f vs %.4f)", z->rx, base.rx);
}

/* 9. Tilt. */
static void t_tilt(void)
{
    XBOX_INPUT_STATE s;
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    reset();

    xbox_ios_touch_set_tilt(0.5f, 1);
    s = rd();
    expect_sticks(&s, 16383, 0, 0, 0, 2, "tilt 0.5");
    xbox_ios_touch_set_tilt(-1.0f, 1);
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, 0, 0, 0, "tilt -1");
    xbox_ios_touch_set_tilt(7.0f, 1);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, 0, "tilt 7 clamps to 1");

    xbox_ios_touch_set_tilt(0.5f, 1);
    /* Not overriding: a right-stick finger, a zone finger. */
    fdown_bare(1, 0.60f, 0.50f);
    fmove(1, 0.60f + STICK_RADIUS, 0.50f);
    if (a)
        fdown(2, a->cx, a->cy);
    s = rd();
    expect_sticks(&s, 16383, 0, STICK_FULL, 0, STICK_NEAR, "tilt with right stick and zone fingers");
    fup(1);
    fup(2);

    /* A left-stick finger overrides, even one that has not moved yet. */
    fdown_bare(3, 0.30f, 0.50f);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "tilt vs a resting left-stick finger");
    fmove(3, 0.30f - STICK_RADIUS, 0.50f);
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, 0, 0, STICK_NEAR, "tilt vs a left-stick finger moving left");
    fup(3);
    s = rd();
    expect_sticks(&s, 16383, 0, 0, 0, 2, "tilt returns after the left-stick finger lifts");

    xbox_ios_touch_set_tilt(0.0f, 0);
    s = rd();
    expect_idle(&s, "tilt off");
    xbox_ios_touch_set_tilt(0.9f, 0);        /* value ignored while inactive */
    s = rd();
    expect_idle(&s, "tilt inactive with a value");
}

/* 10. Press hook (the hook itself is defined above the cases). */
static void t_press_hook(void)
{
    int za = zone_by_label("A"), zb = zone_by_label("B");
    int zd = zone_by_kind(XBOX_IOS_ZONE_DPAD);
    int zs = zone_by_flag(XBOX_GAMEPAD_START);
    const XBOX_IOS_TOUCH_ZONE *a = zone(za), *b = zone(zb), *d = zone(zd), *st = zone(zs);
    reset();
    if (!a || !b || !d || !st)
        return;
    touch_test_hook_calls = 0;
    xbox_ios_touch_set_press_hook(touch_test_count_hook);

    fdown(1, a->cx, a->cy);
    CHECK(touch_test_hook_calls == 1, "hook calls after A down: %d, want 1", touch_test_hook_calls);
    CHECK(s_hook_zone[0] == za, "hook zone %d, want A = %d", s_hook_zone[0], za);
    fmove(1, a->cx + 0.001f, a->cy);
    fdown(1, a->cx, a->cy);                  /* a repeated down for a held finger */
    CHECK(touch_test_hook_calls == 1, "hook calls after move + repeated down: %d, want 1",
          touch_test_hook_calls);
    fup(1);
    CHECK(touch_test_hook_calls == 1, "hook calls after up: %d, want 1", touch_test_hook_calls);

    fdown(2, b->cx, b->cy);
    fdown(3, d->cx, d->cy);
    fdown(4, st->cx, st->cy);
    CHECK(touch_test_hook_calls == 4, "hook calls after B, d-pad, START: %d, want 4", touch_test_hook_calls);
    CHECK(s_hook_zone[1] == zb && s_hook_zone[2] == zd && s_hook_zone[3] == zs,
          "hook zones %d,%d,%d want %d,%d,%d", s_hook_zone[1], s_hook_zone[2], s_hook_zone[3], zb, zd, zs);
    lift_all();

    /* Stick touches are not zone presses. */
    touch_test_hook_calls = 0;
    fdown_bare(1, 0.30f, 0.50f);
    fdown_bare(2, 0.60f, 0.50f);
    fdown_bare(3, 0.35f, 0.60f);                  /* a waiting second finger */
    fmove(1, 0.40f, 0.50f);
    fup(1);
    fup(2);
    fup(3);
    CHECK(touch_test_hook_calls == 0, "hook fired %d times for stick touches", touch_test_hook_calls);

    /* NULL clears it. */
    xbox_ios_touch_set_press_hook(NULL);
    fdown(1, a->cx, a->cy);
    fup(1);
    CHECK(touch_test_hook_calls == 0, "hook fired %d times after being cleared", touch_test_hook_calls);
}

/* xbox_ios_touch_init is idempotent: a second call must not install a second
 * watch (that would run every finger event twice; the hook would show it). */
static void t_init_idempotent(void)
{
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    int r1, r2;
    reset();
    if (!a)
        return;
    r1 = xbox_ios_touch_init();
    r2 = xbox_ios_touch_init();
    CHECK(r1 == 1 && r2 == 1, "init returned %d, %d; want 1, 1", r1, r2);
    touch_test_hook_calls = 0;
    xbox_ios_touch_set_press_hook(touch_test_count_hook);
    fdown(1, a->cx, a->cy);
    CHECK(touch_test_hook_calls == 1, "hook fired %d times for one press after double init", touch_test_hook_calls);
    fup(1);
}

/* Five fingers are tracked; a sixth is ignored until a slot frees. */
static void t_finger_limit(void)
{
    static const char *five[] = { "A", "B", "X", "Y", "LB" };
    const XBOX_IOS_TOUCH_ZONE *rb = zone(zone_by_label("RB"));
    XBOX_INPUT_STATE s;
    int i;
    reset();
    if (!rb)
        return;
    for (i = 0; i < 5; i++) {
        const XBOX_IOS_TOUCH_ZONE *z = zone(zone_by_label(five[i]));
        if (!z)
            return;
        fdown(i + 1, z->cx, z->cy);
    }
    fdown(6, rb->cx, rb->cy);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 && s.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 0
          && s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 0,
          "sixth finger should be ignored (RB=%d)", s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 255, "fifth finger (LB) not held");
    fup(1);
    fdown(7, rb->cx, rb->cy);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255, "RB not pressed after a slot freed");
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0, "A still held after its lift");
    lift_all();
}

/* Focus loss and backgrounding drop every finger. */
static void t_lifecycle(void)
{
    const XBOX_IOS_TOUCH_ZONE *a = zone(zone_by_label("A"));
    XBOX_INPUT_STATE s;
    SDL_Event e;
    reset();
    if (!a)
        return;
    fdown(1, a->cx, a->cy);
    fdown_bare(2, 0.30f, 0.50f);
    fmove(2, 0.30f + STICK_RADIUS, 0.50f);
    wevent(SDL_WINDOWEVENT_FOCUS_LOST, 0, 0);
    s = rd();
    expect_idle(&s, "after FOCUS_LOST");
    lift_all();

    fdown(1, a->cx, a->cy);
    memset(&e, 0, sizeof e);
    e.type = SDL_APP_WILLENTERBACKGROUND;
    push(&e);
    s = rd();
    expect_idle(&s, "after APP_WILLENTERBACKGROUND");
    lift_all();

    /* Still alive afterwards. */
    fdown(1, a->cx, a->cy);
    s = rd();
    expect_only_analog(&s, XBOX_BUTTON_A, "A after the lifecycle events");
    lift_all();
}

/* Overlay helpers: the floating-stick visual follows the finger. */
static void t_overlay_queries(void)
{
    float bx, by, kx, ky;
    reset();
    CHECK(xbox_ios_touch_stick(0, &bx, &by, &kx, &ky) == 0, "stick(0) reports a finger when none is down");
    fdown_bare(1, 0.30f, 0.50f);
    fmove(1, 0.35f, 0.52f);
    CHECK(xbox_ios_touch_stick(0, &bx, &by, &kx, &ky) == 1, "stick(0) reports no finger while held");
    CHECK(fabsf(bx - 0.30f) < 1e-6f && fabsf(by - 0.50f) < 1e-6f, "stick base %.3f,%.3f want 0.30,0.50", bx, by);
    CHECK(fabsf(kx - 0.35f) < 1e-6f && fabsf(ky - 0.52f) < 1e-6f, "stick knob %.3f,%.3f want 0.35,0.52", kx, ky);
    CHECK(xbox_ios_touch_stick(1, &bx, &by, &kx, &ky) == 0, "stick(1) reports a finger on the left half");
    fup(1);
    CHECK(xbox_ios_touch_stick(0, &bx, &by, &kx, &ky) == 0, "stick(0) still reports a finger after the lift");
    CHECK(zone_by_label("A") >= 0, "label A missing");
    CHECK(xbox_ios_touch_zone_label(-1) == NULL && xbox_ios_touch_zone_label(zcount()) == NULL,
          "zone_label out of range should be NULL");
    CHECK(xbox_ios_touch_zone_label(zone_by_kind(XBOX_IOS_ZONE_DPAD)) == NULL, "d-pad zone has a label");
    CHECK(xbox_ios_touch_zone_label(zone_by_flag(XBOX_GAMEPAD_START)) == NULL, "START zone has a label");
}

/* Zone styles for the overlay: derived from kind / value and the layout. */
static void t_zone_style(void)
{
    static const struct { const char *label; int want; const char *name; } pad[] = {
        { "A", XBOX_IOS_STYLE_FACE_A, "A" },
        { "B", XBOX_IOS_STYLE_FACE_B, "B" },
        { "X", XBOX_IOS_STYLE_FACE_X, "X" },
        { "Y", XBOX_IOS_STYLE_FACE_Y, "Y" },
        { "LB", XBOX_IOS_STYLE_SHOULDER_BLACK, "LB" },
        { "RB", XBOX_IOS_STYLE_SHOULDER_WHITE, "RB" },
        { "LT", XBOX_IOS_STYLE_TRIGGER_L, "LT" },
        { "RT", XBOX_IOS_STYLE_TRIGGER_R, "RT" },
    };
    size_t k;
    int n, i;

    reset();
    for (k = 0; k < sizeof pad / sizeof pad[0]; k++) {
        int zi = zone_by_label(pad[k].label);
        CHECK(zi >= 0 && xbox_ios_touch_zone_style(zi) == pad[k].want,
              "PAD %s: style %d, want %d", pad[k].name,
              zi >= 0 ? xbox_ios_touch_zone_style(zi) : -1, pad[k].want);
    }
    CHECK(xbox_ios_touch_zone_style(zone_by_kind(XBOX_IOS_ZONE_DPAD)) == XBOX_IOS_STYLE_DPAD,
          "d-pad style");
    CHECK(xbox_ios_touch_zone_style(zone_by_flag(XBOX_GAMEPAD_BACK)) == XBOX_IOS_STYLE_MENU_BACK,
          "BACK style");
    CHECK(xbox_ios_touch_zone_style(zone_by_flag(XBOX_GAMEPAD_START)) == XBOX_IOS_STYLE_MENU_START,
          "START style");
    CHECK(xbox_ios_touch_zone_style(-1) == XBOX_IOS_STYLE_OTHER &&
          xbox_ios_touch_zone_style(zcount()) == XBOX_IOS_STYLE_OTHER,
          "out-of-range style should be OTHER");

    /* DRIVE: LT / RT zones are the pedals, the rest keeps its style. */
    xbox_ios_touch_set_layout(1);
    {
        int gas = zone_by_label("GAS"), brake = zone_by_label("BRAKE");
        CHECK(gas >= 0 && xbox_ios_touch_zone_style(gas) == XBOX_IOS_STYLE_PEDAL_GAS,
              "DRIVE GAS style");
        CHECK(brake >= 0 && xbox_ios_touch_zone_style(brake) == XBOX_IOS_STYLE_PEDAL_BRAKE,
              "DRIVE BRAKE style");
        CHECK(zone(gas) && zone(gas)->value == XBOX_BUTTON_RTRIGGER, "DRIVE GAS is not RT");
        CHECK(zone(brake) && zone(brake)->value == XBOX_BUTTON_LTRIGGER, "DRIVE BRAKE is not LT");
        /* Changed with the DRIVE redesign: A is "E-BRAKE" there now. */
        CHECK(xbox_ios_touch_zone_style(zone_by_label("E-BRAKE")) == XBOX_IOS_STYLE_FACE_A,
              "DRIVE E-BRAKE (A) style");
        CHECK(xbox_ios_touch_zone_style(zone_by_label("NOS")) == XBOX_IOS_STYLE_FACE_B,
              "DRIVE NOS (B) style");
        CHECK(xbox_ios_touch_zone_style(zone_by_label("LOOK")) == XBOX_IOS_STYLE_SHOULDER_BLACK,
              "DRIVE LOOK (Black) style");
        CHECK(xbox_ios_touch_zone_style(zone_by_kind(XBOX_IOS_ZONE_STEER)) == XBOX_IOS_STYLE_STEER,
              "DRIVE strip style");
        CHECK(xbox_ios_touch_zone_label(zone_by_kind(XBOX_IOS_ZONE_STEER)) == NULL,
              "the strip has a label");
    }
    /* Every zone gets a real style, never OTHER, in both layouts. */
    for (int pass = 0; pass < 2; pass++) {
        xbox_ios_touch_set_layout(pass);
        n = zcount();
        for (i = 0; i < n; i++)
            CHECK(xbox_ios_touch_zone_style(i) != XBOX_IOS_STYLE_OTHER,
                  "layout %d zone %d has style OTHER", pass, i);
    }
    reset();
}

/* Zone with this analog button, -1 when the table has none (no failure). */
static int zone_by_button(int button)
{
    int n = zcount(), i;
    for (i = 0; i < n; i++) {
        const XBOX_IOS_TOUCH_ZONE *z = zone(i);
        if (z->kind == XBOX_IOS_ZONE_ANALOG && z->value == button)
            return i;
    }
    return -1;
}

static const char *cap_of(int zi)  { return zi >= 0 ? xbox_ios_touch_zone_caption(zi) : NULL; }
static const char *icon_of(int zi) { return zi >= 0 ? xbox_ios_touch_zone_icon(zi) : NULL; }

/* want == NULL: the accessor must return NULL. */
static void expect_str(const char *got, const char *want, const char *what)
{
    if (!want)
        CHECK(got == NULL, "%s: \"%s\", want NULL", what, got ? got : "");
    else
        CHECK(got && strcmp(got, want) == 0, "%s: \"%s\", want \"%s\"", what,
              got ? got : "(null)", want);
}

/* Captions and icons for the overlay: PAD, new DRIVE, classic DRIVE. */
static void t_caption_icon(void)
{
    static const struct { int button; const char *name, *cap, *icon; } pad[] = {
        { XBOX_BUTTON_A,        "A",  "E-BRAKE",  NULL },
        { XBOX_BUTTON_B,        "B",  "NOS",      NULL },
        { XBOX_BUTTON_X,        "X",  "SHIFT UP", NULL },
        { XBOX_BUTTON_Y,        "Y",  "SHIFT DN", NULL },
        { XBOX_BUTTON_BLACK,    "LB", "LOOK",     "eye.fill" },
        { XBOX_BUTTON_WHITE,    "RB", "CAM",      "camera.fill" },
        { XBOX_BUTTON_LTRIGGER, "LT", "BRAKE",    NULL },
        { XBOX_BUTTON_RTRIGGER, "RT", "GAS",      NULL },
    };
    static const struct { int button; const char *name, *icon; } drv[] = {
        { XBOX_BUTTON_A,        "E-BRAKE", "exclamationmark.octagon.fill" },
        { XBOX_BUTTON_B,        "NOS",     "flame.fill" },
        { XBOX_BUTTON_X,        "SHIFT up", "arrow.up" },
        { XBOX_BUTTON_Y,        "SHIFT down", "arrow.down" },
        { XBOX_BUTTON_BLACK,    "LOOK",    "eye.fill" },
        { XBOX_BUTTON_WHITE,    "CAM",     "camera.fill" },
        { XBOX_BUTTON_LTRIGGER, "BRAKE",   NULL },
        { XBOX_BUTTON_RTRIGGER, "GAS",     NULL },
    };
    size_t k;
    int pass, i, zi;

    reset();
    /* PAD */
    for (k = 0; k < sizeof pad / sizeof pad[0]; k++) {
        zi = zone_by_button(pad[k].button);
        CHECK(zi >= 0, "PAD %s: no zone", pad[k].name);
        expect_str(cap_of(zi), pad[k].cap, pad[k].name);
        expect_str(icon_of(zi), pad[k].icon, pad[k].name);
        /* The primary label stays the controller name. */
        if (zi >= 0)
            expect_str(xbox_ios_touch_zone_label(zi), pad[k].name, "PAD primary label");
    }
    zi = zone_by_flag(XBOX_GAMEPAD_START);
    expect_str(cap_of(zi), "PAUSE", "PAD START caption");
    expect_str(icon_of(zi), "pause.fill", "PAD START icon");
    zi = zone_by_flag(XBOX_GAMEPAD_BACK);
    expect_str(cap_of(zi), NULL, "PAD BACK caption");
    expect_str(icon_of(zi), "arrow.uturn.backward", "PAD BACK icon");
    zi = zone_by_kind(XBOX_IOS_ZONE_DPAD);
    expect_str(cap_of(zi), NULL, "PAD D-pad caption");
    expect_str(icon_of(zi), NULL, "PAD D-pad icon");

    /* New DRIVE: no captions at all, icons on the function buttons. */
    xbox_ios_touch_set_layout(1);
    for (i = 0; i < zcount(); i++)
        CHECK(xbox_ios_touch_zone_caption(i) == NULL, "DRIVE zone %d has a caption", i);
    for (k = 0; k < sizeof drv / sizeof drv[0]; k++) {
        zi = zone_by_button(drv[k].button);
        CHECK(zi >= 0, "DRIVE %s: no zone", drv[k].name);
        expect_str(icon_of(zi), drv[k].icon, drv[k].name);
    }
    expect_str(icon_of(zone_by_flag(XBOX_GAMEPAD_START)), "pause.fill", "DRIVE START icon");
    expect_str(icon_of(zone_by_flag(XBOX_GAMEPAD_BACK)), "arrow.uturn.backward", "DRIVE BACK icon");
    expect_str(icon_of(zone_by_kind(XBOX_IOS_ZONE_DPAD)), NULL, "DRIVE D-pad icon");
    expect_str(icon_of(zone_by_kind(XBOX_IOS_ZONE_STEER)), NULL, "DRIVE strip icon");
    /* The primary labels did not move. */
    expect_str(xbox_ios_touch_zone_label(zone_by_button(XBOX_BUTTON_B)), "NOS", "DRIVE B label");

    /* Classic DRIVE: PAD captions, old labels, no face icons. */
    xbox_ios_touch_set_drive_classic(1);
    for (k = 0; k < sizeof pad / sizeof pad[0]; k++) {
        zi = zone_by_button(pad[k].button);
        CHECK(zi >= 0, "classic %s: no zone", pad[k].name);
        expect_str(cap_of(zi), pad[k].cap, pad[k].name);
        expect_str(icon_of(zi), pad[k].icon, pad[k].name);
    }
    expect_str(xbox_ios_touch_zone_label(zone_by_button(XBOX_BUTTON_A)), "A", "classic A label");
    expect_str(xbox_ios_touch_zone_label(zone_by_button(XBOX_BUTTON_BLACK)), "LB", "classic LB label");
    expect_str(cap_of(zone_by_flag(XBOX_GAMEPAD_START)), "PAUSE", "classic START caption");
    expect_str(icon_of(zone_by_flag(XBOX_GAMEPAD_BACK)), "arrow.uturn.backward", "classic BACK icon");

    /* Out of range, in every table. */
    for (pass = 0; pass < 3; pass++) {
        xbox_ios_touch_set_drive_classic(pass == 2);
        xbox_ios_touch_set_layout(pass != 0);
        CHECK(xbox_ios_touch_zone_caption(-1) == NULL && xbox_ios_touch_zone_caption(zcount()) == NULL &&
              xbox_ios_touch_zone_caption(1000) == NULL, "caption out of range (table %d)", pass);
        CHECK(xbox_ios_touch_zone_icon(-1) == NULL && xbox_ios_touch_zone_icon(zcount()) == NULL &&
              xbox_ios_touch_zone_icon(1000) == NULL, "icon out of range (table %d)", pass);
    }
    /* Back in PAD the captions return. */
    xbox_ios_touch_set_drive_classic(0);
    xbox_ios_touch_set_layout(0);
    expect_str(cap_of(zone_by_button(XBOX_BUTTON_B)), "NOS", "PAD B caption after the round trip");
    reset();
}

/* The pressed D-pad arms, as bits, agree with what the game reads. */
static void t_dpad_bits(void)
{
    int zi;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    size_t k;
    static const struct { float ox, oy; unsigned want; const char *name; } dirs[] = {
        {  0.0f, -0.7f, XBOX_IOS_DPAD_BIT_UP,    "up" },
        {  0.0f,  0.7f, XBOX_IOS_DPAD_BIT_DOWN,  "down" },
        { -0.7f,  0.0f, XBOX_IOS_DPAD_BIT_LEFT,  "left" },
        {  0.7f,  0.0f, XBOX_IOS_DPAD_BIT_RIGHT, "right" },
        { -0.7f, -0.7f, XBOX_IOS_DPAD_BIT_UP | XBOX_IOS_DPAD_BIT_LEFT,    "up-left" },
        {  0.7f,  0.7f, XBOX_IOS_DPAD_BIT_DOWN | XBOX_IOS_DPAD_BIT_RIGHT, "down-right" },
    };
    reset();
    zi = zone_by_kind(XBOX_IOS_ZONE_DPAD);
    z = zone(zi);
    if (!z)
        return;
    CHECK(XBOX_IOS_DPAD_BIT_UP == 1u && XBOX_IOS_DPAD_BIT_DOWN == 2u &&
          XBOX_IOS_DPAD_BIT_LEFT == 4u && XBOX_IOS_DPAD_BIT_RIGHT == 8u,
          "bit values are part of the overlay contract");
    CHECK(xbox_ios_touch_dpad_bits() == 0, "bits set with no finger down");

    fdown(1, z->cx, z->cy);
    CHECK(xbox_ios_touch_dpad_bits() == 0, "bits set at the d-pad centre");
    fup(1);

    for (k = 0; k < sizeof dirs / sizeof dirs[0]; k++) {
        unsigned fromstate = 0;
        fdown(1, z->cx + dirs[k].ox * z->rx, z->cy + dirs[k].oy * z->ry);
        CHECK(xbox_ios_touch_dpad_bits() == dirs[k].want, "d-pad %s: bits 0x%x, want 0x%x",
              dirs[k].name, xbox_ios_touch_dpad_bits(), dirs[k].want);
        s = rd();
        if (s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_UP)    fromstate |= XBOX_IOS_DPAD_BIT_UP;
        if (s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_DOWN)  fromstate |= XBOX_IOS_DPAD_BIT_DOWN;
        if (s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_LEFT)  fromstate |= XBOX_IOS_DPAD_BIT_LEFT;
        if (s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_RIGHT) fromstate |= XBOX_IOS_DPAD_BIT_RIGHT;
        CHECK(fromstate == xbox_ios_touch_dpad_bits(),
              "d-pad %s: bits 0x%x differ from xbox_ios_touch_state 0x%x", dirs[k].name,
              xbox_ios_touch_dpad_bits(), fromstate);
        fup(1);
        CHECK(xbox_ios_touch_dpad_bits() == 0, "d-pad %s: bits stay after the lift", dirs[k].name);
    }

    /* A finger that slides keeps following the zone's rule. */
    fdown(1, z->cx, z->cy);
    fmove(1, z->cx + 0.7f * z->rx, z->cy);
    CHECK(xbox_ios_touch_dpad_bits() == XBOX_IOS_DPAD_BIT_RIGHT, "slide to right: bits 0x%x",
          xbox_ios_touch_dpad_bits());
    /* Other zones and the sticks never set a bit. */
    fdown(2, zone(zone_by_label("A"))->cx, zone(zone_by_label("A"))->cy);
    fdown_bare(3, 0.30f, 0.50f);
    fmove(3, 0.30f - STICK_RADIUS, 0.50f);
    CHECK(xbox_ios_touch_dpad_bits() == XBOX_IOS_DPAD_BIT_RIGHT, "A / stick fingers changed the bits");
    lift_all();

    fdown(1, zone(zone_by_label("A"))->cx, zone(zone_by_label("A"))->cy);
    CHECK(xbox_ios_touch_dpad_bits() == 0, "bits set by a finger on A");
    lift_all();

    /* Layout switch clears fingers, so the bits go too. */
    fdown(1, z->cx + 0.7f * z->rx, z->cy);
    CHECK(xbox_ios_touch_dpad_bits() != 0, "setup: no bits before the layout switch");
    xbox_ios_touch_set_layout(1);
    CHECK(xbox_ios_touch_dpad_bits() == 0, "bits survive a layout switch");
    reset();
}

/* layout_gen moves on every rebuild of the zone table and only then. */
static void t_layout_gen(void)
{
    unsigned g0, g1;
    const XBOX_IOS_TOUCH_ZONE *z;
    float bx, by, kx, ky;

    reset();
    g0 = xbox_ios_touch_layout_gen();
    CHECK(g0 == xbox_ios_touch_layout_gen(), "gen changes between two reads");

    /* Reads, touches, tilt, the enable switch and queries leave it alone. */
    z = zone(0);
    if (z)
        fdown(1, z->cx, z->cy);
    fdown_bare(2, 0.30f, 0.50f);
    fmove(2, 0.35f, 0.52f);
    (void)xbox_ios_touch_stick(0, &bx, &by, &kx, &ky);
    (void)xbox_ios_touch_zone_pressed(0);
    (void)xbox_ios_touch_dpad_bits();
    (void)xbox_ios_touch_zone_style(0);
    xbox_ios_touch_set_tilt(0.5f, 1);
    xbox_ios_touch_set_tilt(0.0f, 0);
    lift_all();
    xbox_ios_touch_set_enabled(0);
    xbox_ios_touch_set_enabled(1);
    CHECK(xbox_ios_touch_layout_gen() == g0, "gen moved without a rebuild");

    xbox_ios_touch_set_layout(1);
    g1 = xbox_ios_touch_layout_gen();
    CHECK(g1 != g0, "gen unchanged after set_layout");
    xbox_ios_touch_set_scale(1.25f);
    CHECK(xbox_ios_touch_layout_gen() != g1, "gen unchanged after set_scale");
    g0 = xbox_ios_touch_layout_gen();
    xbox_ios_touch_set_layout(0);
    CHECK(xbox_ios_touch_layout_gen() != g0, "gen unchanged after set_layout back");
    reset();
}

/* ------------------------------------------------------------------ */
/* Drive feel: analog pedals, steering strip, activity hook, geometry   */
/* ------------------------------------------------------------------ */

#define PEDAL_FLAT_BYTE 255
#define PEDAL_LOW_BYTE  90

/* Finger height for fraction f of a zone (0 top .. 1 bottom). */
static float zone_y(const XBOX_IOS_TOUCH_ZONE *z, float f)
{
    return z->cy - z->ry + 2.0f * z->ry * f;
}

static int analog_of(const XBOX_INPUT_STATE *s, int button)
{
    return s->Gamepad.bAnalogButtons[button];
}

/* One pedal: finger down at the zone centre (inside, latched from then on),
 * then slid to each height. Expected byte for fraction f. */
static void pedal_ramp(const char *label, int button)
{
    int zi = zone_by_label(label);
    const XBOX_IOS_TOUCH_ZONE *z = zone(zi);
    XBOX_INPUT_STATE s;
    static const struct { float f; int want; int tol; const char *name; } pts[] = {
        { 0.50f, 255, 0, "middle" },
        { 0.05f, 255, 0, "top" },
        { 0.58f, 255, 0, "just above the 40% mark" },
        { 0.60f, 255, 1, "the 40% mark" },
        { 0.80f, 172, 2, "80% down (half way down the ramp)" },
        { 1.00f,  90, 1, "the very bottom" },
        { 1.40f,  90, 1, "below the zone (latched finger)" },
        { 0.20f, 255, 0, "back up" },
        { -0.40f, 255, 0, "above the zone (latched finger)" },
        { 0.70f, 214, 2, "70% down" },
    };
    size_t k;
    char what[80];

    if (!z)
        return;
    fdown(1, z->cx, z->cy);
    for (k = 0; k < sizeof pts / sizeof pts[0]; k++) {
        float v;
        fmove(1, z->cx, zone_y(z, pts[k].f));
        s = rd();
        snprintf(what, sizeof what, "%s %s", label, pts[k].name);
        CHECK(abs(analog_of(&s, button) - pts[k].want) <= pts[k].tol,
              "%s: %d, want %d (+-%d)", what, analog_of(&s, button), pts[k].want, pts[k].tol);
        v = xbox_ios_touch_zone_value(zi);
        CHECK(fabsf(v - (float)analog_of(&s, button) / 255.0f) < 0.004f,
              "%s: zone_value %.3f differs from the state byte %d", what, v, analog_of(&s, button));
    }
    /* The ramp is monotonic from the 40% mark down. */
    {
        int prev = 256, i;
        for (i = 0; i <= 20; i++) {
            float f = 0.6f + 0.4f * (float)i / 20.0f;
            fmove(1, z->cx, zone_y(z, f));
            s = rd();
            CHECK(analog_of(&s, button) <= prev, "%s ramp not monotonic at f=%.2f", label, f);
            prev = analog_of(&s, button);
        }
    }
    fup(1);
    CHECK(xbox_ios_touch_zone_value(zi) == 0.0f, "%s: zone_value %.3f after the lift",
          label, xbox_ios_touch_zone_value(zi));
    s = rd();
    expect_idle(&s, "pedal lifted");
}

/* 16. Analog pedals: bottom ~90, 40% mark 255, upper part 255, follows the
 * finger, only the DRIVE pedals, scale-independent. */
static void t_pedal_analog(void)
{
    int zi, a, n;
    XBOX_INPUT_STATE s;
    const XBOX_IOS_TOUCH_ZONE *z;
    reset();
    xbox_ios_touch_set_layout(1);
    pedal_ramp("GAS", XBOX_BUTTON_RTRIGGER);
    pedal_ramp("BRAKE", XBOX_BUTTON_LTRIGGER);

    /* Not a pedal: NOS (B) and E-BRAKE (A) stay flat 255 at the bottom. */
    a = zone_by_label("NOS");
    z = zone(a);
    if (z) {
        fdown(1, z->cx, z->cy);
        fmove(1, z->cx, zone_y(z, 1.0f));
        s = rd();
        expect_only_analog(&s, XBOX_BUTTON_B, "NOS at the bottom of its zone");
        CHECK(xbox_ios_touch_zone_value(a) == 1.0f, "NOS zone_value %.3f, want 1",
              xbox_ios_touch_zone_value(a));
        fup(1);
    }

    /* The ramp follows the scaled zone. */
    xbox_ios_touch_set_scale(1.3f);
    zi = zone_by_label("GAS");
    z = zone(zi);
    if (z) {
        fdown(1, z->cx, z->cy);
        fmove(1, z->cx, zone_y(z, 1.0f));
        s = rd();
        CHECK(abs(analog_of(&s, XBOX_BUTTON_RTRIGGER) - PEDAL_LOW_BYTE) <= 1,
              "GAS bottom at scale 1.3: %d, want 90", analog_of(&s, XBOX_BUTTON_RTRIGGER));
        fup(1);
    }
    xbox_ios_touch_set_scale(1.0f);

    /* PAD triggers stay flat. */
    xbox_ios_touch_set_layout(0);
    zi = zone_by_label("RT");
    z = zone(zi);
    n = zcount();
    CHECK(n > 0, "no PAD zones");
    if (z) {
        fdown(1, z->cx, z->cy);
        fmove(1, z->cx, zone_y(z, 1.0f));
        s = rd();
        expect_only_analog(&s, XBOX_BUTTON_RTRIGGER, "PAD RT at the bottom of its zone");
        CHECK(xbox_ios_touch_zone_value(zi) == 1.0f, "PAD RT zone_value %.3f, want 1",
              xbox_ios_touch_zone_value(zi));
        fup(1);
    }
    CHECK(xbox_ios_touch_zone_value(-1) == 0.0f && xbox_ios_touch_zone_value(zcount()) == 0.0f,
          "zone_value out of range should be 0");
}

/* 16b. Digital switch: set_analog_pedals(0) gives today's flat 255, live and
 * with a finger held. */
static void t_pedal_digital(void)
{
    int zi;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    reset();
    xbox_ios_touch_set_layout(1);
    zi = zone_by_label("GAS");
    z = zone(zi);
    if (!z)
        return;
    fdown(1, z->cx, z->cy);
    fmove(1, z->cx, zone_y(z, 1.0f));
    s = rd();
    CHECK(abs(analog_of(&s, XBOX_BUTTON_RTRIGGER) - PEDAL_LOW_BYTE) <= 1, "analog GAS bottom %d",
          analog_of(&s, XBOX_BUTTON_RTRIGGER));
    xbox_ios_touch_set_analog_pedals(0);            /* the finger stays */
    s = rd();
    expect_only_analog(&s, XBOX_BUTTON_RTRIGGER, "digital GAS at the bottom (finger held across the switch)");
    CHECK(xbox_ios_touch_zone_value(zi) == 1.0f, "digital zone_value %.3f, want 1",
          xbox_ios_touch_zone_value(zi));
    xbox_ios_touch_set_analog_pedals(1);
    s = rd();
    CHECK(abs(analog_of(&s, XBOX_BUTTON_RTRIGGER) - PEDAL_LOW_BYTE) <= 1, "analog again: %d",
          analog_of(&s, XBOX_BUTTON_RTRIGGER));
    fup(1);
}

/* Two fingers on one pedal read the higher value. */
static void t_pedal_two_fingers(void)
{
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    reset();
    xbox_ios_touch_set_layout(1);
    z = zone(zone_by_label("GAS"));
    if (!z)
        return;
    fdown(1, z->cx, zone_y(z, 0.9f));
    fdown(2, z->cx, zone_y(z, 0.3f));
    s = rd();
    CHECK(analog_of(&s, XBOX_BUTTON_RTRIGGER) == 255, "two fingers on GAS: %d, want the higher, 255",
          analog_of(&s, XBOX_BUTTON_RTRIGGER));
    fup(2);
    s = rd();
    CHECK(analog_of(&s, XBOX_BUTTON_RTRIGGER) < 255 && analog_of(&s, XBOX_BUTTON_RTRIGGER) > PEDAL_LOW_BYTE,
          "after the high finger lifted: %d", analog_of(&s, XBOX_BUTTON_RTRIGGER));
    lift_all();
}

static int strip_zone(void)
{
    return zone_by_kind(XBOX_IOS_ZONE_STEER);
}

#define LX(s) ((s).Gamepad.sThumbLX)

/* 17. Steering strip, ABSOLUTE mode (the first version, kept as an option):
 * deflection from the zone centre, deadzone rescaled (no jump), clamp, Y stays
 * 0, latch, release. The relative default is cases 17g..17k. */
static void t_steer_strip(void)
{
    int si;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    float half;
    reset();
    xbox_ios_touch_set_steer_mode(0);
    xbox_ios_touch_set_layout(1);
    si = strip_zone();
    z = zone(si);
    if (!z)
        return;
    half = z->rx;                                    /* sensitivity 1 */
    CHECK(zone_hit(z->cx, z->cy) == si, "strip centre shadowed by zone %d", zone_hit(z->cx, z->cy));
    CHECK(xbox_ios_touch_zone_value(si) == 0.0f, "strip value %.3f with no finger",
          xbox_ios_touch_zone_value(si));

    fdown(1, z->cx, z->cy);
    s = rd();
    expect_idle(&s, "strip, finger at the centre");
    CHECK(xbox_ios_touch_zone_pressed(si), "strip: zone_pressed() is 0 while held");

    fmove(1, z->cx + 0.5f * half, z->cy);
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.5f - 0.05f) / 0.95f), 0, 0, 0, 60, "strip, half way right");
    CHECK(fabsf(xbox_ios_touch_zone_value(si) - 0.4737f) < 0.002f, "strip value %.4f, want 0.4737",
          xbox_ios_touch_zone_value(si));
    fmove(1, z->cx - 0.25f * half, z->cy);
    s = rd();
    expect_sticks(&s, -(int)(32767.0f * (0.25f - 0.05f) / 0.95f), 0, 0, 0, 60, "strip, quarter left");
    CHECK(fabsf(xbox_ios_touch_zone_value(si) + 0.2105f) < 0.002f, "strip value %.4f, want -0.2105",
          xbox_ios_touch_zone_value(si));

    /* Y never moves, wherever the finger is vertically. */
    fmove(1, z->cx + 0.5f * half, z->cy - z->ry);
    s = rd();
    CHECK(s.Gamepad.sThumbLY == 0 && s.Gamepad.sThumbRX == 0 && s.Gamepad.sThumbRY == 0,
          "strip moved another axis: LY=%d RX=%d RY=%d", s.Gamepad.sThumbLY, s.Gamepad.sThumbRX,
          s.Gamepad.sThumbRY);

    /* Deadzone 0.05 of full travel, rescaled so there is no jump at its edge. */
    fmove(1, z->cx + 0.04f * half, z->cy);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "strip, inside the deadzone");
    fmove(1, z->cx + 0.05f * half, z->cy);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 40, "strip, at the deadzone edge");
    fmove(1, z->cx + 0.06f * half, z->cy);
    s = rd();
    CHECK(LX(s) > 0 && LX(s) < 600, "strip just past the deadzone: LX=%d (a jump?)", LX(s));
    fmove(1, z->cx - 0.04f * half, z->cy);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "strip, inside the deadzone on the left");

    /* Full lock exactly at the half-width; clamped beyond, also off the zone. */
    fmove(1, z->cx + half, z->cy);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "strip, full right");
    fmove(1, z->cx + 3.0f * half, z->cy + 0.2f);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, 0, "strip, 3x overshoot right (clamped, latched)");
    CHECK(xbox_ios_touch_zone_value(si) == 1.0f, "strip value %.4f, want 1", xbox_ios_touch_zone_value(si));
    fmove(1, z->cx - 3.0f * half, z->cy);
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, 0, 0, 0, "strip, 3x overshoot left");
    CHECK(xbox_ios_touch_zone_value(si) == -1.0f, "strip value %.4f, want -1", xbox_ios_touch_zone_value(si));

    /* Latched: sliding onto another zone presses nothing, steering goes on. */
    {
        const XBOX_IOS_TOUCH_ZONE *g = zone(zone_by_label("GAS"));
        if (g) {
            fmove(1, g->cx, g->cy);
            s = rd();
            CHECK(s.Gamepad.wButtons == 0 && analog_of(&s, XBOX_BUTTON_RTRIGGER) == 0,
                  "a strip finger pressed GAS by sliding over it");
            CHECK(LX(s) == STICK_FULL, "strip finger over GAS: LX=%d, want full right", LX(s));
        }
    }

    /* Release: back to 0. */
    fup(1);
    s = rd();
    expect_idle(&s, "strip lifted");
    CHECK(!xbox_ios_touch_zone_pressed(si), "strip still pressed after the lift");
    CHECK(xbox_ios_touch_zone_value(si) == 0.0f, "strip value %.3f after the lift",
          xbox_ios_touch_zone_value(si));
}

/* 17b. Strip sensitivity (absolute): full lock at half-width / sens; clamped 0.5..2. */
static void t_steer_sens(void)
{
    int si;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    float half;
    reset();
    xbox_ios_touch_set_steer_mode(0);
    xbox_ios_touch_set_layout(1);
    si = strip_zone();
    z = zone(si);
    if (!z)
        return;
    half = z->rx;
    fdown(1, z->cx, z->cy);

    fmove(1, z->cx + 0.25f * half, z->cy);
    xbox_ios_touch_set_steer_sens(2.0f);              /* live, finger held */
    CHECK(zone(si) && zone(si)->rx == half, "the strip width moved with the sensitivity");
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.5f - 0.05f) / 0.95f), 0, 0, 0, 60, "sens 2: quarter = half deflection");
    fmove(1, z->cx + 0.5f * half, z->cy);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "sens 2: half the width = full lock");

    xbox_ios_touch_set_steer_sens(0.5f);
    fmove(1, z->cx + 0.5f * half, z->cy);
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.25f - 0.05f) / 0.95f), 0, 0, 0, 60, "sens 0.5: half the width = quarter");
    fmove(1, z->cx + half, z->cy);
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.5f - 0.05f) / 0.95f), 0, 0, 0, 60, "sens 0.5: full width = half deflection");

    xbox_ios_touch_set_steer_sens(9.0f);              /* clamps to 2 */
    fmove(1, z->cx + 0.5f * half, z->cy);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "sens 9 clamps to 2");
    xbox_ios_touch_set_steer_sens(0.01f);             /* clamps to 0.5 */
    fmove(1, z->cx + half, z->cy);
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.5f - 0.05f) / 0.95f), 0, 0, 0, 60, "sens 0.01 clamps to 0.5");
    xbox_ios_touch_set_steer_sens(NAN);
    fmove(1, z->cx + half, z->cy);
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.5f - 0.05f) / 0.95f), 0, 0, 0, 60, "sens NaN clamps to 0.5");
    fup(1);
}

/* 17c. The strip is hit as a rectangle, keeps its width at every size and
 * stays on screen. */
static void t_steer_shape(void)
{
    int si;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_IOS_TOUCH_ZONE base;
    XBOX_INPUT_STATE s;
    float lx, ty;
    reset();
    xbox_ios_touch_set_steer_mode(0);        /* the corner finger sits far from the centre */
    xbox_ios_touch_set_layout(1);
    si = strip_zone();
    z = zone(si);
    if (!z)
        return;
    base = *z;
    /* A point that an ellipse of the same radii would miss: the top-left
     * corner, inset 1%. */
    lx = base.cx - 0.99f * base.rx;
    ty = base.cy - 0.99f * base.ry;
    CHECK(zone_hit(lx, ty) == si, "strip corner (%.3f,%.3f) is not in the strip (zone %d)", lx, ty,
          zone_hit(lx, ty));
    fdown(1, lx, ty);
    CHECK(xbox_ios_touch_zone_pressed(si), "a finger in the strip corner is not a strip finger");
    s = rd();
    CHECK(LX(s) < -STICK_FULL / 2, "strip corner finger: LX=%d, want a hard left", LX(s));
    fup(1);
    /* Just outside the rectangle is not the strip. */
    CHECK(zone_hit(base.cx + 1.05f * base.rx, base.cy) != si, "a point 5%% outside the strip hits it");

    /* Island strip (x 11-48 pt, y 133-260 pt of 852x393) stays clear. */
    CHECK((base.cx - base.rx) * 852.0f > 48.0f ||
          (base.cy - base.ry) * 393.0f > 260.0f, "the strip meets the Dynamic Island strip");

    /* Size: the width stays; the height follows the size slider but stays on
     * screen. */
    xbox_ios_touch_set_scale(1.5f);
    z = zone(strip_zone());
    if (z) {
        CHECK(z->rx == base.rx, "strip rx %.4f changed with the size (was %.4f)", z->rx, base.rx);
        CHECK(z->cx - z->rx >= 0.0f && z->cx + z->rx <= 0.5f, "strip leaves x 0..0.5 at 1.5x");
        CHECK(z->cy + z->ry <= 1.0f && z->cy - z->ry >= 0.5f, "strip leaves the screen at 1.5x (y %.3f..%.3f)",
              z->cy - z->ry, z->cy + z->ry);
        CHECK(z->ry > base.ry, "strip did not grow at 1.5x");
    }
    xbox_ios_touch_set_scale(0.7f);
    z = zone(strip_zone());
    if (z)
        CHECK(z->rx == base.rx && z->ry < base.ry, "strip at 0.7x: rx %.4f ry %.4f", z->rx, z->ry);
    xbox_ios_touch_set_scale(1.0f);
}

/* 17d. Strip off (setter): no zone, the dynamic stick as before; on again
 * brings it back. Both toggles drop the fingers. PAD is not affected. */
static void t_steer_disabled(void)
{
    int n_on, si, i;
    float cx, cy;
    float bx, by, kx, ky;
    XBOX_INPUT_STATE s;
    const XBOX_IOS_TOUCH_ZONE *z;
    unsigned gen;
    reset();
    xbox_ios_touch_set_layout(1);
    n_on = zcount();
    si = strip_zone();
    z = zone(si);
    if (!z)
        return;
    cx = z->cx;
    cy = z->cy;

    fdown(1, cx, cy);                                 /* a strip finger across the toggle */
    gen = xbox_ios_touch_layout_gen();
    xbox_ios_touch_set_steer(0);
    CHECK(xbox_ios_touch_layout_gen() != gen, "set_steer did not bump the layout generation");
    CHECK(zcount() == n_on - 1, "zone count %d with the strip off, want %d", zcount(), n_on - 1);
    CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) < 0, "a STEER zone is still in the table");
    for (i = 0; i < zcount(); i++)
        CHECK(xbox_ios_touch_zone_style(i) != XBOX_IOS_STYLE_STEER, "STEER style left in the table");
    s = rd();
    expect_idle(&s, "strip toggled off with a finger held");
    fup(1);

    /* The same spot is now the bare left half: a dynamic stick. */
    require_bare(cx, cy);
    fdown(1, cx, cy);
    CHECK(xbox_ios_touch_stick(0, &bx, &by, &kx, &ky) == 1, "no dynamic stick finger where the strip was");
    fmove(1, cx + STICK_RADIUS, cy);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "dynamic stick where the strip was");
    fup(1);

    gen = xbox_ios_touch_layout_gen();
    xbox_ios_touch_set_steer(1);
    CHECK(xbox_ios_touch_layout_gen() != gen, "set_steer(1) did not bump the layout generation");
    CHECK(zcount() == n_on, "zone count %d with the strip back, want %d", zcount(), n_on);
    CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) >= 0, "no STEER zone after set_steer(1)");

    /* PAD has no strip either way. */
    xbox_ios_touch_set_layout(0);
    CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) < 0, "PAD has a STEER zone");
    xbox_ios_touch_set_steer(0);
    xbox_ios_touch_set_steer(1);
    CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) < 0, "PAD has a STEER zone after toggling the strip");
}

/* 17e. A strip finger outranks the dynamic left stick in either order of
 * arrival, two strip fingers: the first steers, right stick independent. */
static void t_steer_priority(void)
{
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    float half;
    reset();
    xbox_ios_touch_set_steer_mode(0);
    xbox_ios_touch_set_layout(1);
    z = zone(strip_zone());
    if (!z)
        return;
    half = z->rx;
    require_bare(0.30f, 0.50f);
    require_bare(0.60f, 0.50f);

    /* Stick first (lower slot), strip second. */
    fdown(1, 0.30f, 0.50f);
    fmove(1, 0.30f + STICK_RADIUS, 0.50f - STICK_RADIUS / WIN_ASPECT);
    s = rd();
    CHECK(LX(s) > 0 && s.Gamepad.sThumbLY > 0, "setup: stick finger not driving (%d,%d)", LX(s),
          s.Gamepad.sThumbLY);
    fdown(2, z->cx - 0.5f * half, z->cy);
    s = rd();
    expect_sticks(&s, -(int)(32767.0f * (0.5f - 0.05f) / 0.95f), 0, 0, 0, 60,
                  "stick finger first, strip second: the strip wins, Y is 0");
    fup(2);
    s = rd();
    CHECK(LX(s) > 0 && s.Gamepad.sThumbLY > 0, "stick does not drive again after the strip lifted");
    fup(1);

    /* Strip first (lower slot), stick second. */
    fdown(1, z->cx + 0.5f * half, z->cy);
    fdown(2, 0.30f, 0.50f);
    fmove(2, 0.30f - STICK_RADIUS, 0.50f);
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.5f - 0.05f) / 0.95f), 0, 0, 0, 60,
                  "strip first, stick second: the strip wins");
    lift_all();

    /* Two strip fingers: the first one steers; the second takes over after. */
    fdown(1, z->cx + 0.5f * half, z->cy);
    fdown(2, z->cx - 0.5f * half, z->cy);
    s = rd();
    CHECK(LX(s) > 0, "two strip fingers: LX=%d, want the first (right)", LX(s));
    CHECK(fabsf(xbox_ios_touch_zone_value(strip_zone()) - 0.4737f) < 0.002f,
          "strip zone_value %.3f differs from the state", xbox_ios_touch_zone_value(strip_zone()));
    fup(1);
    s = rd();
    CHECK(LX(s) < 0, "after the first lifted: LX=%d, want the second (left)", LX(s));
    lift_all();

    /* The right stick is independent of the strip. */
    fdown(1, z->cx + 0.5f * half, z->cy);
    fdown(2, 0.60f, 0.50f);
    fmove(2, 0.60f + STICK_RADIUS, 0.50f);
    s = rd();
    CHECK(abs(LX(s) - (int)(32767.0f * (0.5f - 0.05f) / 0.95f)) <= 60 && s.Gamepad.sThumbLY == 0 &&
          abs(s.Gamepad.sThumbRX - STICK_FULL) <= STICK_NEAR && s.Gamepad.sThumbRY == 0,
          "strip + right stick: L=(%d,%d) R=(%d,%d)", LX(s), s.Gamepad.sThumbLY, s.Gamepad.sThumbRX,
          s.Gamepad.sThumbRY);
    lift_all();
}

/* 17f. Tilt applies only while no finger steers: strip or stick. */
static void t_tilt_vs_strip(void)
{
    const XBOX_IOS_TOUCH_ZONE *z;
    const XBOX_IOS_TOUCH_ZONE *g;
    XBOX_INPUT_STATE s;
    reset();
    xbox_ios_touch_set_layout(1);
    z = zone(strip_zone());
    g = zone(zone_by_label("GAS"));
    if (!z || !g)
        return;
    xbox_ios_touch_set_tilt(0.5f, 1);
    s = rd();
    expect_sticks(&s, 16383, 0, 0, 0, 2, "tilt alone");

    fdown(1, g->cx, g->cy);                           /* a pedal does not stop the tilt */
    s = rd();
    CHECK(LX(s) == 16383 || abs(LX(s) - 16383) <= 2, "tilt with GAS held: LX=%d", LX(s));
    fup(1);

    fdown(1, z->cx, z->cy);                           /* a resting strip finger does */
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "tilt vs a resting strip finger");
    fmove(1, z->cx - 0.075f, z->cy);                 /* full lock, either mode */
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, 0, 0, STICK_NEAR, "tilt vs a strip finger steering left");
    fup(1);
    s = rd();
    expect_sticks(&s, 16383, 0, 0, 0, 2, "tilt returns after the strip finger lifts");
    xbox_ios_touch_set_tilt(0.0f, 0);
}

/* ---- Relative steering (the default): cases 17g..17j ---- */

#define REL_TRAVEL 0.075f                 /* of the window width at sens 1; contract in xinput_ios.c */
#define REL_TOL    40                     /* stick units */

/* Expected LX for a finger offset of `frac` x travel in relative mode: clamp,
 * 4 % deadzone rescaled, then |d|^curve. */
static int rel_lx(float frac, float curve)
{
    float a = fabsf(frac);
    if (a < 0.04f)
        return 0;
    if (a > 1.0f)
        a = 1.0f;
    a = (a - 0.04f) / 0.96f;
    a = powf(a, curve);
    return (int)(32767.0f * (frac < 0.0f ? -a : a));
}

static int rel_lx_ok(int lx, float frac, float curve)
{
    return abs(lx - rel_lx(frac, curve)) <= REL_TOL;
}

/* 17g. Relative steering: the touch-down point is straight ahead wherever it
 * lands in the strip; +travel is full lock; half travel with the default curve
 * 1.35 is about 0.5^1.35; the deadzone is rescaled; the finger keeps steering
 * outside the strip; lifting gives 0. */
static void t_steer_rel(void)
{
    int si;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    float x0, y0, dn = -1.0f, kn = -1.0f, v;
    int lx_half;

    reset();
    xbox_ios_touch_set_layout(1);
    CHECK(xbox_ios_touch_get_steer_mode() == 1, "default steer mode is not relative");
    si = strip_zone();
    z = zone(si);
    if (!z)
        return;
    CHECK(xbox_ios_touch_steer_knob(si, &dn, &kn) == 0, "steer_knob() is 1 with no finger");
    x0 = z->cx + 0.8f * z->rx;                       /* far right inside the strip */
    y0 = z->cy;
    CHECK(zone_hit(x0, y0) == si, "setup: (%.3f,%.3f) is not in the strip", x0, y0);

    fdown(1, x0, y0);
    s = rd();
    expect_idle(&s, "relative: landing off-centre is straight ahead");
    CHECK(xbox_ios_touch_zone_value(si) == 0.0f, "relative: value %.4f at touch-down",
          xbox_ios_touch_zone_value(si));
    CHECK(xbox_ios_touch_zone_pressed(si), "relative: strip not pressed");
    CHECK(xbox_ios_touch_steer_knob(si, &dn, &kn) == 1 && fabsf(dn - x0) < 1e-6f && fabsf(kn - x0) < 1e-6f,
          "steer_knob at touch-down: %.5f %.5f, want %.5f twice", dn, kn, x0);
    CHECK(xbox_ios_touch_steer_knob(si == 0 ? 1 : 0, &dn, &kn) == 0, "steer_knob() answers for a non-strip zone");

    /* +travel is full lock; the knob query follows the finger. */
    fmove(1, x0 + REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "relative: +travel");
    v = xbox_ios_touch_zone_value(si);
    CHECK(v > 0.999f && v <= 1.0f, "relative: value %.4f at +travel, want 1", v);
    CHECK(xbox_ios_touch_steer_knob(si, &dn, &kn) == 1 && fabsf(kn - (x0 + REL_TRAVEL)) < 1e-5f,
          "steer_knob at +travel: %.5f, want %.5f", kn, x0 + REL_TRAVEL);

    /* Half travel with curve 1.35: deadzone-rescaled, then ^1.35 (about 0.5^1.35). */
    fmove(1, x0 + 0.5f * REL_TRAVEL, y0);
    s = rd();
    lx_half = rel_lx(0.5f, 1.35f);
    expect_sticks(&s, lx_half, 0, 0, 0, REL_TOL, "relative: half travel, curve 1.35");
    CHECK(fabsf(xbox_ios_touch_zone_value(si) - (float)lx_half / 32767.0f) < 0.002f,
          "relative: zone_value %.4f differs from the state %d", xbox_ios_touch_zone_value(si), LX(s));
    CHECK(fabsf((float)LX(s) / 32767.0f - powf(0.5f, 1.35f)) < 0.03f,
          "relative: half travel %.4f is not about 0.5^1.35 = %.4f", (float)LX(s) / 32767.0f, powf(0.5f, 1.35f));
    fmove(1, x0 - 0.25f * REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, rel_lx(-0.25f, 1.35f), 0, 0, 0, REL_TOL, "relative: quarter travel left");

    /* Y never moves. */
    fmove(1, x0 + 0.5f * REL_TRAVEL, y0 - z->ry);
    s = rd();
    CHECK(s.Gamepad.sThumbLY == 0 && s.Gamepad.sThumbRX == 0 && s.Gamepad.sThumbRY == 0,
          "relative: another axis moved (LY=%d RX=%d RY=%d)", s.Gamepad.sThumbLY, s.Gamepad.sThumbRX,
          s.Gamepad.sThumbRY);

    /* Deadzone: 4 % of travel, rescaled (no jump at its edge). */
    fmove(1, x0 + 0.03f * REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "relative: inside the deadzone (right)");
    fmove(1, x0 - 0.03f * REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "relative: inside the deadzone (left)");
    fmove(1, x0 + 0.05f * REL_TRAVEL, y0);
    s = rd();
    CHECK(LX(s) >= 0 && LX(s) < 150, "relative: just past the deadzone LX=%d (a jump?)", LX(s));
    fmove(1, x0 + 0.06f * REL_TRAVEL, y0);
    s = rd();
    CHECK(LX(s) > 0 && LX(s) < 600, "relative: 6%% of travel LX=%d (a jump?)", LX(s));

    /* Overshoot clamps; the knob stops at full travel. */
    fmove(1, x0 + 3.0f * REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "relative: 3x overshoot right");
    CHECK(xbox_ios_touch_steer_knob(si, &dn, &kn) == 1 && fabsf(kn - dn - REL_TRAVEL) < 1e-5f,
          "steer_knob overshoot: knob - down = %.5f, want %.5f", kn - dn, REL_TRAVEL);

    /* Latched: outside the strip, over another zone and at the screen edge. */
    {
        const XBOX_IOS_TOUCH_ZONE *g = zone(zone_by_label("GAS"));
        if (g) {
            fmove(1, g->cx, g->cy);
            s = rd();
            CHECK(s.Gamepad.wButtons == 0 && analog_of(&s, XBOX_BUTTON_RTRIGGER) == 0,
                  "a relative strip finger pressed GAS by sliding over it");
            CHECK(LX(s) == STICK_FULL, "relative: finger over GAS LX=%d, want full right", LX(s));
        }
    }
    fmove(1, 0.0f, 0.05f);                           /* far left of the strip, top of the screen */
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, 0, 0, 0, "relative: finger at the screen's left edge");
    CHECK(xbox_ios_touch_zone_pressed(si), "relative: the strip let go of a finger outside it");

    /* The same offset gives the same value wherever the finger landed. */
    fup(1);
    fdown(2, z->cx - 0.9f * z->rx, y0);
    fmove(2, z->cx - 0.9f * z->rx + 0.5f * REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, lx_half, 0, 0, 0, REL_TOL, "relative: same offset from another landing point");
    fmove(2, z->cx - 0.9f * z->rx, y0);
    s = rd();
    expect_sticks(&s, 0, 0, 0, 0, 0, "relative: back at the touch-down point");

    /* Release. */
    fup(2);
    s = rd();
    expect_idle(&s, "relative: strip lifted");
    CHECK(xbox_ios_touch_zone_value(si) == 0.0f, "relative: value %.4f after the lift", xbox_ios_touch_zone_value(si));
    CHECK(xbox_ios_touch_steer_knob(si, &dn, &kn) == 0, "steer_knob() is 1 after the lift");

    /* Absolute mode has no indicator. */
    xbox_ios_touch_set_steer_mode(0);
    CHECK(xbox_ios_touch_get_steer_mode() == 0, "get_steer_mode after set(0)");
    fdown(3, x0, y0);
    CHECK(xbox_ios_touch_steer_knob(si, &dn, &kn) == 0, "steer_knob() is 1 in absolute mode");
    fup(3);
}

/* 17h. Relative sensitivity: travel is 0.075 / sens (clamped 0.5..2), live. */
static void t_steer_rel_sens(void)
{
    int si;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    float x0, y0, dn, kn;

    reset();
    xbox_ios_touch_set_layout(1);
    si = strip_zone();
    z = zone(si);
    if (!z)
        return;
    x0 = z->cx;
    y0 = z->cy;
    fdown(1, x0, y0);

    xbox_ios_touch_set_steer_sens(2.0f);              /* live, finger held: travel 0.0375 */
    fmove(1, x0 + 0.5f * REL_TRAVEL / 2.0f, y0);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, "rel sens 2: half of 0.0375");
    fmove(1, x0 + REL_TRAVEL / 2.0f, y0);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "rel sens 2: 0.0375 = full lock");
    fmove(1, x0 + REL_TRAVEL, y0);
    xbox_ios_touch_steer_knob(si, &dn, &kn);
    CHECK(fabsf(kn - dn - REL_TRAVEL / 2.0f) < 1e-5f, "rel sens 2: knob travel %.5f, want %.5f", kn - dn,
          REL_TRAVEL / 2.0f);

    xbox_ios_touch_set_steer_sens(0.5f);              /* travel 0.15 */
    fmove(1, x0 + 0.5f * REL_TRAVEL * 2.0f, y0);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, "rel sens 0.5: half of 0.15");
    fmove(1, x0 + REL_TRAVEL, y0);                    /* 0.075 is only half travel now */
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, "rel sens 0.5: 0.075 is half travel");
    fmove(1, x0 + REL_TRAVEL * 2.0f, y0);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "rel sens 0.5: 0.15 = full lock");
    xbox_ios_touch_steer_knob(si, &dn, &kn);
    CHECK(fabsf(kn - dn - REL_TRAVEL * 2.0f) < 1e-5f, "rel sens 0.5: knob travel %.5f", kn - dn);

    xbox_ios_touch_set_steer_sens(9.0f);              /* clamps to 2 */
    fmove(1, x0 + REL_TRAVEL / 2.0f, y0);
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "rel sens 9 clamps to 2");
    xbox_ios_touch_set_steer_sens(0.01f);             /* clamps to 0.5 */
    fmove(1, x0 + REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, "rel sens 0.01 clamps to 0.5");
    xbox_ios_touch_set_steer_sens(NAN);
    fmove(1, x0 + REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, "rel sens NaN clamps to 0.5");
    fup(1);
}

/* 17i. Response curve: 1.0 = linear, 2.0 = squared, clamped 1.0..2.0 (NaN 1.0),
 * relative mode only; absolute ignores it; the mode switches live. */
static void t_steer_curve(void)
{
    int si, k;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    float x0, y0;
    int lin, def;
    static const float fr[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

    reset();
    xbox_ios_touch_set_layout(1);
    si = strip_zone();
    z = zone(si);
    if (!z)
        return;
    x0 = z->cx;
    y0 = z->cy;
    fdown(1, x0, y0);

    xbox_ios_touch_set_steer_curve(1.0f);             /* linear */
    for (k = 0; k < 4; k++) {
        float a = (fr[k] - 0.04f) / 0.96f;
        fmove(1, x0 + fr[k] * REL_TRAVEL, y0);
        s = rd();
        CHECK(abs(LX(s) - (int)(32767.0f * a)) <= REL_TOL, "curve 1.0 at %.2f travel: LX=%d, want %d", fr[k],
              LX(s), (int)(32767.0f * a));
    }
    fmove(1, x0 + 0.5f * REL_TRAVEL, y0);
    s = rd();
    lin = LX(s);
    xbox_ios_touch_set_steer_curve(1.35f);
    s = rd();
    def = LX(s);
    CHECK(def < lin - 1000, "curve 1.35 (%d) is not gentler than linear (%d) at half travel", def, lin);
    xbox_ios_touch_set_steer_curve(2.0f);
    for (k = 0; k < 4; k++) {
        float a = (fr[k] - 0.04f) / 0.96f;
        fmove(1, x0 + fr[k] * REL_TRAVEL, y0);
        s = rd();
        CHECK(abs(LX(s) - (int)(32767.0f * a * a)) <= REL_TOL, "curve 2.0 at %.2f travel: LX=%d, want %d", fr[k],
              LX(s), (int)(32767.0f * a * a));
    }
    CHECK(xbox_ios_touch_zone_value(si) > 0.999f, "curve 2.0: full lock value %.4f", xbox_ios_touch_zone_value(si));
    fmove(1, x0 - 0.5f * REL_TRAVEL, y0);
    s = rd();
    CHECK(LX(s) < 0 && abs(LX(s) + rel_lx(0.5f, 2.0f)) <= REL_TOL, "curve 2.0 is not symmetric: LX=%d", LX(s));

    xbox_ios_touch_set_steer_curve(9.0f);             /* clamps to 2.0 */
    fmove(1, x0 + 0.5f * REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 2.0f), 0, 0, 0, REL_TOL, "curve 9 clamps to 2.0");
    xbox_ios_touch_set_steer_curve(0.2f);             /* clamps to 1.0 */
    s = rd();
    expect_sticks(&s, lin, 0, 0, 0, 1, "curve 0.2 clamps to 1.0");
    xbox_ios_touch_set_steer_curve(2.0f);
    xbox_ios_touch_set_steer_curve(NAN);              /* NaN gives 1.0 */
    s = rd();
    expect_sticks(&s, lin, 0, 0, 0, 1, "curve NaN gives 1.0");

    /* Absolute mode: no curve (today's behaviour), and the held finger is read
     * by the other rule at once: its offset from the strip centre. */
    xbox_ios_touch_set_steer_curve(2.0f);
    xbox_ios_touch_set_steer_mode(0);
    s = rd();
    expect_sticks(&s, (int)(32767.0f * (0.5f * REL_TRAVEL / z->rx - 0.05f) / 0.95f), 0, 0, 0, 60,
                  "mode switched to absolute with a finger held: curve ignored");
    xbox_ios_touch_set_steer_mode(1);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 2.0f), 0, 0, 0, REL_TOL, "mode back to relative with the finger held");
    fup(1);
    xbox_ios_touch_set_steer_curve(1.35f);
}

/* 17j. Relative steering does not depend on the strip: its size, its place
 * (override, safe area) or the Size setting. The setters move no generation
 * and drop no finger. Two strip fingers each keep their own touch-down point. */
static void t_steer_rel_indep(void)
{
    int si, pass;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;
    unsigned gen;
    float x0, y0;

    reset();
    xbox_ios_touch_set_layout(1);

    for (pass = 0; pass < 4; pass++) {
        reset();
        xbox_ios_touch_set_layout(1);
        si = strip_zone();
        if (pass == 1)
            xbox_ios_touch_set_scale(1.5f);
        else if (pass == 2)
            xbox_ios_touch_set_safe_area(0.0692f, 0.0692f);
        else if (pass == 3)
            xbox_ios_touch_zone_set_geom(si, 0.30f, 0.75f, 0.30f, 0.15f);   /* a wide user override */
        z = zone(strip_zone());
        if (!z)
            return;
        x0 = z->cx + 0.3f * z->rx;
        y0 = z->cy;
        fdown(1, x0, y0);
        fmove(1, x0 + 0.5f * REL_TRAVEL, y0);
        s = rd();
        expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, pass == 0 ? "indep: preset" :
                      pass == 1 ? "indep: size 1.5" : pass == 2 ? "indep: safe area" : "indep: wide override");
        fmove(1, x0 + REL_TRAVEL, y0);
        s = rd();
        expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "indep: full lock");
        fup(1);
    }

    reset();
    xbox_ios_touch_set_layout(1);
    z = zone(strip_zone());
    if (!z)
        return;
    x0 = z->cx;
    y0 = z->cy;
    fdown(1, x0, y0);
    fmove(1, x0 + REL_TRAVEL, y0);
    gen = xbox_ios_touch_layout_gen();
    xbox_ios_touch_set_steer_mode(0);
    xbox_ios_touch_set_steer_mode(1);
    xbox_ios_touch_set_steer_curve(1.7f);
    xbox_ios_touch_set_steer_curve(1.35f);
    CHECK(xbox_ios_touch_layout_gen() == gen, "the mode / curve setters moved the layout generation");
    CHECK(xbox_ios_touch_zone_pressed(strip_zone()), "the mode / curve setters dropped the strip finger");
    s = rd();
    expect_sticks(&s, STICK_FULL, 0, 0, 0, STICK_NEAR, "finger still at full lock after the setters");
    fup(1);

    /* Two strip fingers: each keeps its own touch-down point. */
    fdown(1, x0 - 0.04f, y0);
    fdown(2, x0 + 0.04f, y0);
    fmove(1, x0 - 0.04f + 0.5f * REL_TRAVEL, y0);
    fmove(2, x0 + 0.04f - REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, "two strip fingers: the first steers");
    fup(1);
    s = rd();
    expect_sticks(&s, -STICK_FULL, 0, 0, 0, STICK_NEAR, "two strip fingers: the second takes over from its own point");
    fup(2);

    /* A strip finger still outranks a dynamic stick finger. */
    require_bare(0.30f, 0.50f);
    fdown(1, 0.30f, 0.50f);
    fmove(1, 0.30f + STICK_RADIUS, 0.50f - STICK_RADIUS / WIN_ASPECT);
    fdown(2, x0, y0);
    fmove(2, x0 + 0.5f * REL_TRAVEL, y0);
    s = rd();
    expect_sticks(&s, rel_lx(0.5f, 1.35f), 0, 0, 0, REL_TOL, "relative strip outranks the dynamic stick");
    lift_all();
}

/* 18. Activity hook: every tracked finger down, whatever it lands on. */
static int s_act_calls, s_act_recorded;
static int s_seq[64], s_seqn;

static void note_seq(int what)
{
    if (s_seqn < 64)
        s_seq[s_seqn++] = what;
}

static void act_hook(void)
{
    float bx, by, kx, ky;
    s_act_calls++;
    note_seq('A');
    /* The finger is recorded before the hook runs: the layer can be queried. */
    {
        int n = zcount(), i, any = 0;
        for (i = 0; i < n; i++)
            any |= xbox_ios_touch_zone_pressed(i);
        any |= xbox_ios_touch_stick(0, &bx, &by, &kx, &ky);
        any |= xbox_ios_touch_stick(1, &bx, &by, &kx, &ky);
        if (any)
            s_act_recorded++;
    }
}

static void seq_press_hook(int zone_idx)
{
    (void)zone_idx;
    note_seq('P');
}

static void t_activity_hook(void)
{
    const XBOX_IOS_TOUCH_ZONE *a, *z;
    int expect = 0;
    reset();
    xbox_ios_touch_set_layout(1);
    z = zone(strip_zone());
    a = zone(zone_by_label("GAS"));
    if (!z || !a)
        return;
    require_bare(0.30f, 0.50f);
    require_bare(0.60f, 0.50f);

    s_act_calls = 0;
    s_act_recorded = 0;
    s_seqn = 0;
    xbox_ios_touch_set_activity_hook(act_hook);
    xbox_ios_touch_set_press_hook(seq_press_hook);

    fdown(1, a->cx, a->cy);                           /* a zone */
    expect++;
    CHECK(s_act_calls == expect, "after a zone down: %d calls, want %d", s_act_calls, expect);
    CHECK(s_seqn == 2 && s_seq[0] == 'A' && s_seq[1] == 'P',
          "order after a zone down: %d events, first %c second %c (want activity, then press)", s_seqn,
          s_seqn > 0 ? s_seq[0] : '-', s_seqn > 1 ? s_seq[1] : '-');
    fdown(2, z->cx, z->cy);                           /* the steering strip */
    expect++;
    CHECK(s_act_calls == expect, "after a strip down: %d calls, want %d", s_act_calls, expect);
    fdown(3, 0.30f, 0.50f);                           /* the dynamic left stick */
    expect++;
    CHECK(s_act_calls == expect, "after a left-stick down: %d calls, want %d", s_act_calls, expect);
    fdown(4, 0.60f, 0.50f);                           /* the dynamic right stick */
    expect++;
    CHECK(s_act_calls == expect, "after a right-stick down: %d calls, want %d", s_act_calls, expect);
    fdown(5, 0.35f, 0.45f);                           /* a waiting second left finger */
    expect++;
    CHECK(s_act_calls == expect, "after a waiting finger down: %d calls, want %d", s_act_calls, expect);
    CHECK(s_act_recorded == s_act_calls, "the finger was not recorded yet in %d of %d hook calls",
          s_act_calls - s_act_recorded, s_act_calls);

    /* Not for moves, repeated downs, ups or fingers past the limit. */
    fmove(1, a->cx, a->cy + 0.01f);
    fdown(2, z->cx, z->cy);                           /* a repeated down of a held finger */
    fdown(6, 0.45f, 0.45f);                           /* the sixth finger: not tracked */
    fup(1);
    fup(6);
    CHECK(s_act_calls == expect, "moves / repeated down / ups / sixth finger changed the count to %d, want %d",
          s_act_calls, expect);
    lift_all();

    /* Off means off. */
    xbox_ios_touch_set_enabled(0);
    fdown(1, 0.30f, 0.50f);
    CHECK(s_act_calls == expect, "hook fired while touch was off");
    xbox_ios_touch_set_enabled(1);
    lift_all();

    /* NULL clears it; the press hook keeps working alone. */
    xbox_ios_touch_set_activity_hook(NULL);
    fdown(1, a->cx, a->cy);
    CHECK(s_act_calls == expect, "hook fired after being cleared");
    lift_all();
    xbox_ios_touch_set_press_hook(NULL);
}

/* 19. DRIVE geometry on an 852x393 screen: nothing overlaps, nothing meets the
 * Dynamic Island strip or leaves the screen, the main controls are at least
 * 44 pt. TOUCHTEST_GEOMETRY=1 prints the table. */
static void t_drive_geometry(void)
{
    static const char *main_labels[] = { "GAS", "BRAKE", "NOS", "E-BRAKE" };
    const float W = 852.0f, H = 393.0f;
    int n, i, j, k, pass;
    int print = getenv("TOUCHTEST_GEOMETRY") != NULL;

    reset();
    xbox_ios_touch_set_layout(1);
    n = zcount();
    CHECK(n == 12, "DRIVE has %d zones, want 12", n);

    for (pass = 0; pass < 3; pass++) {
        static const float scales[3] = { 1.0f, 0.7f, 1.5f };
        xbox_ios_touch_set_scale(scales[pass]);
        n = zcount();
        for (i = 0; i < n; i++) {
            const XBOX_IOS_TOUCH_ZONE *zi = zone(i);
            const char *l = xbox_ios_touch_zone_label(i);
            /* Own centre hits itself (not shadowed). */
            CHECK(zone_hit(zi->cx, zi->cy) == i, "scale %.1f: zone %d centre shadowed by %d", scales[pass], i,
                  zone_hit(zi->cx, zi->cy));
            /* Sampled overlap with every later zone (both ways). */
            for (j = 0; j < n; j++) {
                const XBOX_IOS_TOUCH_ZONE *zj = zone(j);
                int hit = 0, a, b;
                if (j == i)
                    continue;
                for (a = -40; a <= 40 && !hit; a++)
                    for (b = -40; b <= 40 && !hit; b++) {
                        float x = zi->cx + zi->rx * (float)a / 40.0f;
                        float y = zi->cy + zi->ry * (float)b / 40.0f;
                        if (in_shape(zi, x, y) && in_shape(zj, x, y))
                            hit = 1;
                    }
                if (scales[pass] == 1.0f)
                    CHECK(!hit, "zones %d (%s) and %d overlap at scale 1.0", i, l ? l : "-", j);
            }
            /* The Dynamic Island strip, x 11-48 pt, y 133-260 pt (landscape, scale 1.0 only:
             * the size slider is the user's own choice). */
            if (scales[pass] == 1.0f) {
                int a, b, hit = 0;
                for (a = -40; a <= 40 && !hit; a++)
                    for (b = -40; b <= 40 && !hit; b++) {
                        float x = zi->cx + zi->rx * (float)a / 40.0f;
                        float y = zi->cy + zi->ry * (float)b / 40.0f;
                        if (in_shape(zi, x, y) && x * W >= 11.0f && x * W <= 48.0f &&
                            y * H >= 133.0f && y * H <= 260.0f)
                            hit = 1;
                    }
                CHECK(!hit, "zone %d (%s) meets the Dynamic Island strip", i, l ? l : "-");
                CHECK(zi->cx - zi->rx >= 0.0f && zi->cx + zi->rx <= 1.0f &&
                      zi->cy - zi->ry >= 0.0f && zi->cy + zi->ry <= 1.0f,
                      "zone %d (%s) leaves the screen", i, l ? l : "-");
            }
            if (print && pass == 0) {
                const char *what = zi->kind == XBOX_IOS_ZONE_STEER ? "STEER"
                                 : zi->kind == XBOX_IOS_ZONE_DPAD ? "DPAD"
                                 : zi->kind == XBOX_IOS_ZONE_FLAG ?
                                       (zi->value == XBOX_GAMEPAD_BACK ? "BACK" : "START") : l;
                printf("   geom %-9s id %2d  c %.3f,%.3f  r %.3f,%.3f  x %.3f-%.3f y %.3f-%.3f  %6.1f x %6.1f pt\n",
                       what ? what : "?", zi->id, zi->cx, zi->cy, zi->rx, zi->ry, zi->cx - zi->rx,
                       zi->cx + zi->rx, zi->cy - zi->ry, zi->cy + zi->ry, 2.0f * zi->rx * W, 2.0f * zi->ry * H);
            }
        }
        if (scales[pass] == 1.0f) {
            for (k = 0; k < 4; k++) {
                const XBOX_IOS_TOUCH_ZONE *zk = zone(zone_by_label(main_labels[k]));
                if (zk)
                    CHECK(2.0f * zk->rx * W >= 44.0f && 2.0f * zk->ry * H >= 44.0f,
                          "%s is %.1f x %.1f pt, want >= 44", main_labels[k], 2.0f * zk->rx * W,
                          2.0f * zk->ry * H);
            }
            {
                const XBOX_IOS_TOUCH_ZONE *zs = zone(strip_zone());
                if (zs)
                {
                    CHECK(2.0f * zs->rx * W >= 44.0f && 2.0f * zs->ry * H >= 44.0f,
                          "the strip is %.1f x %.1f pt, want >= 44", 2.0f * zs->rx * W, 2.0f * zs->ry * H);
                    /* Narrow on purpose (relative steering needs a landing area, not a
                     * wheel): about 0.22 of the width, bottom left. */
                    CHECK(2.0f * zs->rx > 0.20f && 2.0f * zs->rx < 0.24f && zs->cx < 0.25f && zs->cy > 0.7f,
                          "the strip is %.3f wide at (%.3f,%.3f), want ~0.22 wide, bottom left", 2.0f * zs->rx,
                          zs->cx, zs->cy);
                }
            }
        }
    }
    xbox_ios_touch_set_scale(1.0f);
}

/* 20. Classic switch: the old DRIVE table and labels, no strip. */
static void t_drive_classic(void)
{
    size_t k;
    int i;
    XBOX_INPUT_STATE s;
    reset();
    xbox_ios_touch_set_layout(1);
    xbox_ios_touch_set_drive_classic(1);
    CHECK(zcount() == 11, "classic DRIVE has %d zones, want 11", zcount());
    CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) < 0, "classic DRIVE has a STEER zone");
    for (k = 0; k < sizeof k_pad_buttons / sizeof k_pad_buttons[0]; k++) {
        const char *l = k_pad_buttons[k].label;
        const XBOX_IOS_TOUCH_ZONE *z;
        if (!strcmp(l, "LT") || !strcmp(l, "RT"))
            continue;
        z = zone(zone_by_label(l));
        if (!z)
            continue;
        fdown(1, z->cx, z->cy);
        s = rd();
        expect_only_analog(&s, k_pad_buttons[k].button, l);
        fup(1);
    }
    for (i = 0; i < zcount(); i++)
        CHECK(xbox_ios_touch_zone_style(i) != XBOX_IOS_STYLE_OTHER, "classic zone %d has style OTHER", i);
    /* Pedals still honour the analog switch. */
    {
        const XBOX_IOS_TOUCH_ZONE *g = zone(zone_by_label("GAS"));
        if (g) {
            fdown(1, g->cx, g->cy);
            fmove(1, g->cx, zone_y(g, 1.0f));
            s = rd();
            CHECK(abs(analog_of(&s, XBOX_BUTTON_RTRIGGER) - PEDAL_LOW_BYTE) <= 1,
                  "classic GAS bottom %d, want 90", analog_of(&s, XBOX_BUTTON_RTRIGGER));
            fup(1);
        }
    }
    xbox_ios_touch_set_drive_classic(0);
    CHECK(zcount() == 12, "new DRIVE has %d zones after leaving classic, want 12", zcount());
    zone_by_label("E-BRAKE");
}

/* ------------------------------------------------------------------ */
/* Environment defaults: need a fresh layer, so run in a forked child   */
/* ------------------------------------------------------------------ */

/* The child exits 0 when its checks pass; otherwise with the failure count.
 * Run before the parent touches SDL or the layer. */
static int env_child(const char *what)
{
    int fails = 0;
    XBOX_INPUT_STATE s;

    if (SDL_Init(SDL_INIT_EVENTS) != 0)
        return 90;
    unsetenv("RECOMP_TOUCH");
    unsetenv("NFSU2_TOUCH_LAYOUT");
    unsetenv("NFSU2_TOUCH_SCALE");
    unsetenv("NFSU2_ANALOG_PEDALS");
    unsetenv("NFSU2_STEER");
    unsetenv("NFSU2_STEER_SENS");
    unsetenv("NFSU2_STEER_MODE");
    unsetenv("NFSU2_STEER_CURVE");
    unsetenv("NFSU2_DRIVE_CLASSIC");
    g_nfail_checks = 0;

    if (!strcmp(what, "off")) {
        setenv("RECOMP_TOUCH", "0", 1);
        if (xbox_ios_touch_init() != 0) fails++;            /* reports disabled */
        if (xbox_ios_touch_enabled() != 0) fails++;
        if (xbox_ios_touch_state(&s) != 0) fails++;
        xbox_ios_touch_set_enabled(1);                      /* the panel can turn it on */
        if (xbox_ios_touch_state(&s) != 1) fails++;
    } else if (!strcmp(what, "drive")) {
        setenv("NFSU2_TOUCH_LAYOUT", "DRIVE", 1);           /* case-insensitive */
        if (xbox_ios_touch_init() != 1) fails++;
        {
            int n = zcount(), i, gas = 0;
            for (i = 0; i < n; i++) {
                const char *l = xbox_ios_touch_zone_label(i);
                if (l && !strcmp(l, "GAS")) gas = 1;
            }
            if (!gas) fails++;
        }
    } else if (!strcmp(what, "scale")) {
        setenv("NFSU2_TOUCH_SCALE", "9", 1);                /* clamps to 1.5 */
        if (xbox_ios_touch_init() != 1) fails++;
        {
            float big = zone(0)->rx, small;
            xbox_ios_touch_set_scale(1.0f);
            small = zone(0)->rx;
            if (fabsf(big / small - 1.5f) > 1e-4f) fails++;
        }
    } else if (!strcmp(what, "analog0")) {
        /* NFSU2_ANALOG_PEDALS=0: the DRIVE pedals read a flat 255 everywhere. */
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        setenv("NFSU2_ANALOG_PEDALS", "0", 1);
        if (xbox_ios_touch_init() != 1) fails++;
        {
            int gi = zone_by_label("GAS");
            const XBOX_IOS_TOUCH_ZONE *g = zone(gi);
            if (g) {
                fdown(1, g->cx, g->cy);
                fmove(1, g->cx, zone_y(g, 1.0f));
                s = rd();
                CHECK(analog_of(&s, XBOX_BUTTON_RTRIGGER) == 255, "env analog off: GAS bottom %d, want 255",
                      analog_of(&s, XBOX_BUTTON_RTRIGGER));
                CHECK(xbox_ios_touch_zone_value(gi) == 1.0f, "env analog off: zone_value %.3f",
                      xbox_ios_touch_zone_value(gi));
                fup(1);
            }
            xbox_ios_touch_set_analog_pedals(1);                  /* the panel can turn it on */
            g = zone(gi);
            if (g) {
                fdown(1, g->cx, g->cy);
                fmove(1, g->cx, zone_y(g, 1.0f));
                s = rd();
                CHECK(abs(analog_of(&s, XBOX_BUTTON_RTRIGGER) - 90) <= 1, "env analog off, panel on: %d",
                      analog_of(&s, XBOX_BUTTON_RTRIGGER));
                fup(1);
            }
        }
        fails += g_nfail_checks;
    } else if (!strcmp(what, "analog1")) {
        /* The default: analog on in DRIVE. */
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        if (xbox_ios_touch_init() != 1) fails++;
        {
            const XBOX_IOS_TOUCH_ZONE *g = zone(zone_by_label("GAS"));
            if (g) {
                fdown(1, g->cx, g->cy);
                fmove(1, g->cx, zone_y(g, 1.0f));
                s = rd();
                CHECK(abs(analog_of(&s, XBOX_BUTTON_RTRIGGER) - 90) <= 1, "default: GAS bottom %d, want 90",
                      analog_of(&s, XBOX_BUTTON_RTRIGGER));
                fup(1);
            }
            CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) >= 0, "default DRIVE has no strip");
        }
        fails += g_nfail_checks;
    } else if (!strcmp(what, "steer0")) {
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        setenv("NFSU2_STEER", "0", 1);
        if (xbox_ios_touch_init() != 1) fails++;
        CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) < 0, "env NFSU2_STEER=0: a STEER zone is in the table");
        CHECK(zcount() == 11, "env NFSU2_STEER=0: %d zones, want 11", zcount());
        xbox_ios_touch_set_steer(1);                              /* the panel can turn it on */
        CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) >= 0, "panel: strip did not come back");
        fails += g_nfail_checks;
    } else if (!strcmp(what, "sens2")) {
        /* Relative (the default): travel is 0.075 / 2 after the clamp. */
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        setenv("NFSU2_STEER_SENS", "9", 1);                       /* clamps to 2 */
        if (xbox_ios_touch_init() != 1) fails++;
        {
            const XBOX_IOS_TOUCH_ZONE *z = zone(zone_find_kind(XBOX_IOS_ZONE_STEER));
            if (z) {
                fdown(1, z->cx, z->cy);
                fmove(1, z->cx + 0.5f * REL_TRAVEL / 2.0f, z->cy);
                s = rd();
                CHECK(rel_lx_ok(LX(s), 0.5f, 1.35f), "env sens 9 (-> 2): half of 0.0375 gives LX=%d, want %d", LX(s),
                      rel_lx(0.5f, 1.35f));
                fmove(1, z->cx + REL_TRAVEL / 2.0f, z->cy);
                s = rd();
                CHECK(abs(LX(s) - 32767) <= STICK_NEAR, "env sens 9 (-> 2): 0.0375 gives LX=%d, want full", LX(s));
                fup(1);
            } else {
                fails++;
            }
        }
        fails += g_nfail_checks;
    } else if (!strcmp(what, "sens2abs")) {
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        setenv("NFSU2_STEER_MODE", "absolute", 1);
        setenv("NFSU2_STEER_SENS", "9", 1);                       /* clamps to 2 */
        if (xbox_ios_touch_init() != 1) fails++;
        {
            const XBOX_IOS_TOUCH_ZONE *z = zone(zone_find_kind(XBOX_IOS_ZONE_STEER));
            if (z) {
                fdown(1, z->cx, z->cy);
                fmove(1, z->cx + 0.5f * z->rx, z->cy);
                s = rd();
                CHECK(abs(LX(s) - 32767) <= STICK_NEAR, "env abs sens 9 (-> 2): half width gives LX=%d, want full", LX(s));
                fup(1);
            } else {
                fails++;
            }
        }
        fails += g_nfail_checks;
    } else if (!strcmp(what, "mode_default") || !strcmp(what, "mode_junk")) {
        /* Unset or not "absolute": relative, curve 1.35. */
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        if (!strcmp(what, "mode_junk"))
            setenv("NFSU2_STEER_MODE", "sideways", 1);
        if (xbox_ios_touch_init() != 1) fails++;
        {
            const XBOX_IOS_TOUCH_ZONE *z = zone(zone_find_kind(XBOX_IOS_ZONE_STEER));
            CHECK(xbox_ios_touch_get_steer_mode() == 1, "env %s: mode is not relative", what);
            if (z) {
                float x0 = z->cx + 0.8f * z->rx;
                fdown(1, x0, z->cy);
                s = rd();
                CHECK(LX(s) == 0, "env %s: landing off-centre gives LX=%d, want 0", what, LX(s));
                fmove(1, x0 + 0.5f * REL_TRAVEL, z->cy);
                s = rd();
                CHECK(rel_lx_ok(LX(s), 0.5f, 1.35f), "env %s: half travel LX=%d, want %d (curve 1.35)", what, LX(s),
                      rel_lx(0.5f, 1.35f));
                fup(1);
            } else {
                fails++;
            }
        }
        fails += g_nfail_checks;
    } else if (!strcmp(what, "mode_abs")) {
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        setenv("NFSU2_STEER_MODE", "ABSOLUTE", 1);                /* case-insensitive */
        if (xbox_ios_touch_init() != 1) fails++;
        {
            const XBOX_IOS_TOUCH_ZONE *z = zone(zone_find_kind(XBOX_IOS_ZONE_STEER));
            CHECK(xbox_ios_touch_get_steer_mode() == 0, "env absolute: mode is not absolute");
            if (z) {
                fdown(1, z->cx + 0.5f * z->rx, z->cy);
                s = rd();
                CHECK(abs(LX(s) - (int)(32767.0f * (0.5f - 0.05f) / 0.95f)) <= 60,
                      "env absolute: landing at half width gives LX=%d", LX(s));
                xbox_ios_touch_set_steer_mode(1);                 /* the panel can switch it */
                s = rd();
                CHECK(LX(s) == 0, "panel: relative with the finger held gives LX=%d, want 0", LX(s));
                fup(1);
            } else {
                fails++;
            }
        }
        fails += g_nfail_checks;
    } else if (!strcmp(what, "curve9") || !strcmp(what, "curve_bad")) {
        /* 9 clamps to 2.0; text that is not a number is 0, which clamps to 1.0. */
        float want_curve = !strcmp(what, "curve9") ? 2.0f : 1.0f;
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        setenv("NFSU2_STEER_CURVE", !strcmp(what, "curve9") ? "9" : "abc", 1);
        if (xbox_ios_touch_init() != 1) fails++;
        {
            const XBOX_IOS_TOUCH_ZONE *z = zone(zone_find_kind(XBOX_IOS_ZONE_STEER));
            if (z) {
                fdown(1, z->cx, z->cy);
                fmove(1, z->cx + 0.5f * REL_TRAVEL, z->cy);
                s = rd();
                CHECK(rel_lx_ok(LX(s), 0.5f, want_curve), "env curve (%s): half travel LX=%d, want %d", what, LX(s),
                      rel_lx(0.5f, want_curve));
                xbox_ios_touch_set_steer_curve(1.35f);            /* the panel can change it */
                s = rd();
                CHECK(rel_lx_ok(LX(s), 0.5f, 1.35f), "panel curve 1.35 after env: LX=%d", LX(s));
                fup(1);
            } else {
                fails++;
            }
        }
        fails += g_nfail_checks;
    } else if (!strcmp(what, "classic")) {
        setenv("NFSU2_TOUCH_LAYOUT", "drive", 1);
        setenv("NFSU2_DRIVE_CLASSIC", "1", 1);
        if (xbox_ios_touch_init() != 1) fails++;
        CHECK(zcount() == 11, "env classic: %d zones, want 11", zcount());
        CHECK(zone_find_kind(XBOX_IOS_ZONE_STEER) < 0, "env classic: has a STEER zone");
        CHECK(zone_by_label("A") >= 0 && zone_by_label("LB") >= 0 && zone_by_label("GAS") >= 0,
              "env classic: old labels missing");
        fails += g_nfail_checks;
    } else {
        fails = 91;
    }
    return fails;
}

static int run_env_child(const char *what)
{
    pid_t p;
    int st = 0;
    fflush(stdout);
    fflush(stderr);
    p = fork();
    if (p < 0)
        return -1;
    if (p == 0) {
        if (!freopen("/dev/null", "w", stderr))   /* the layer logs on init */
            _exit(92);
        _exit(env_child(what));
    }
    if (waitpid(p, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static void env_case(const char *name, const char *what)
{
    int r;
    g_msg[0] = 0;
    g_nfail_checks = 0;
    r = run_env_child(what);
    if (r != 0) {
        g_fail++;
        printf("FAIL  %s\n        child exit code %d (%d failed checks)\n", name, r, r);
    } else {
        g_pass++;
        printf("PASS  %s\n", name);
    }
}

/* ------------------------------------------------------------------ */
/* Layout editor: geometry overrides and their text form                */
/* ------------------------------------------------------------------ */

/* Table index of the zone with this id in the active table, -1 if none. */
static int zone_by_id(int id)
{
    int n = zcount(), i;
    for (i = 0; i < n; i++)
        if (zone(i)->id == id)
            return i;
    return -1;
}

static int near4(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

static void geom_of(int idx, float g[4])
{
    int ok = xbox_ios_touch_zone_geom(idx, &g[0], &g[1], &g[2], &g[3]);
    CHECK(ok, "zone_geom(%d) failed", idx);
    if (!ok)
        g[0] = g[1] = g[2] = g[3] = -1.0f;
}

/* 21. set_geom moves the hit area (scale applied), zone_geom stays unscaled. */
static void t_geom_move(void)
{
    int zi, id;
    float g0[4], g1[4], ox, oy, nx, ny;
    const XBOX_IOS_TOUCH_ZONE *z;
    XBOX_INPUT_STATE s;

    reset();
    zi = zone_by_label("A");
    if (zi < 0)
        return;
    id = zone(zi)->id;
    geom_of(zi, g0);
    ox = g0[0];
    oy = g0[1];

    /* The old centre presses A; a spot in the middle of the screen is bare. */
    fdown(1, ox, oy);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not pressed at its preset centre");
    fup(1);
    nx = 0.50f;
    ny = 0.55f;
    require_bare(nx, ny);

    xbox_ios_touch_set_scale(1.25f);
    zi = zone_by_id(id);
    CHECK(zi >= 0 && xbox_ios_touch_zone_set_geom(zi, nx, ny, g0[2], g0[3]) == 1, "set_geom refused");
    z = zone(zi);
    CHECK(z && near4(z->cx, nx) && near4(z->cy, ny), "table centre %.4f,%.4f want %.4f,%.4f",
          z ? z->cx : -1.0f, z ? z->cy : -1.0f, nx, ny);
    CHECK(z && near4(z->rx, g0[2] * 1.25f) && near4(z->ry, g0[3] * 1.25f),
          "table radii %.4f,%.4f want %.4f,%.4f (base * 1.25)", z ? z->rx : -1.0f, z ? z->ry : -1.0f,
          g0[2] * 1.25f, g0[3] * 1.25f);
    geom_of(zi, g1);
    CHECK(near4(g1[0], nx) && near4(g1[1], ny) && near4(g1[2], g0[2]) && near4(g1[3], g0[3]),
          "zone_geom returned scaled or wrong values %.4f,%.4f,%.4f,%.4f", g1[0], g1[1], g1[2], g1[3]);

    /* Old point misses, new point hits, the edge follows the scaled radius. */
    fdown(1, ox, oy);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0, "A still pressed at its OLD centre");
    fup(1);
    fdown(1, nx, ny);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not pressed at its NEW centre");
    fup(1);
    fdown(1, nx + g0[2] * 1.25f * 0.9f, ny);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not pressed at 0.9 scaled radii");
    fup(1);
    fdown(1, nx + g0[2] * 1.25f * 1.1f, ny);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0, "A pressed at 1.1 scaled radii");
    fup(1);

    /* The override survives a size change and a layout round trip. */
    xbox_ios_touch_set_scale(1.0f);
    xbox_ios_touch_set_layout(1);
    xbox_ios_touch_set_layout(0);
    zi = zone_by_id(id);
    z = zone(zi);
    CHECK(z && near4(z->cx, nx) && near4(z->cy, ny) && near4(z->rx, g0[2]),
          "override lost over scale / layout switches");
    reset();
}

/* 21b. Clamps, bad indices, NaN. */
static void t_geom_clamp(void)
{
    int zi, si;
    float g[4], nan = NAN;

    reset();
    CHECK(xbox_ios_touch_zone_geom(-1, NULL, NULL, NULL, NULL) == 0, "geom(-1) accepted");
    CHECK(xbox_ios_touch_zone_geom(zcount(), NULL, NULL, NULL, NULL) == 0, "geom(n) accepted");
    CHECK(xbox_ios_touch_zone_set_geom(-1, 0.5f, 0.5f, 0.05f, 0.1f) == 0, "set_geom(-1) accepted");
    CHECK(xbox_ios_touch_zone_set_geom(zcount() + 5, 0.5f, 0.5f, 0.05f, 0.1f) == 0, "set_geom(n+5) accepted");
    CHECK(xbox_ios_touch_zone_geom(0, NULL, NULL, NULL, NULL) == 1, "geom with NULL outs refused");

    zi = zone_by_label("A");
    if (zi < 0)
        return;
    /* Too big: radii capped, centre pulled inside. */
    xbox_ios_touch_zone_set_geom(zi, -5.0f, 9.0f, 1.0f, 2.0f);
    geom_of(zi, g);
    CHECK(near4(g[2], 0.30f) && near4(g[3], 0.45f), "big radii -> %.4f,%.4f want 0.30,0.45", g[2], g[3]);
    CHECK(near4(g[0], 0.30f) && near4(g[1], 0.55f), "centre -> %.4f,%.4f want 0.30,0.55", g[0], g[1]);
    /* Too small. */
    xbox_ios_touch_zone_set_geom(zi, 0.5f, 0.5f, 0.0f, -1.0f);
    geom_of(zi, g);
    CHECK(near4(g[2], 0.02f) && near4(g[3], 0.04f), "tiny radii -> %.4f,%.4f want 0.02,0.04", g[2], g[3]);
    /* Far corner: the whole zone stays inside 0..1. */
    xbox_ios_touch_zone_set_geom(zi, 2.0f, 2.0f, 0.05f, 0.1f);
    geom_of(zi, g);
    CHECK(near4(g[0], 0.95f) && near4(g[1], 0.90f), "corner -> %.4f,%.4f want 0.95,0.90", g[0], g[1]);
    /* NaN does not poison the table. */
    xbox_ios_touch_zone_set_geom(zi, nan, nan, nan, nan);
    geom_of(zi, g);
    CHECK(isfinite(g[0]) && isfinite(g[1]) && isfinite(g[2]) && isfinite(g[3]) &&
          g[0] >= g[2] && g[0] <= 1.0f - g[2] && g[1] >= g[3] && g[1] <= 1.0f - g[3],
          "NaN gave %.4f,%.4f,%.4f,%.4f", g[0], g[1], g[2], g[3]);

    /* The steering strip: half-width up to 0.45, height capped where the
     * table caps it (0.19), still on screen. */
    xbox_ios_touch_set_layout(1);
    si = zone_by_kind(XBOX_IOS_ZONE_STEER);
    if (si >= 0) {
        xbox_ios_touch_zone_set_geom(si, 0.0f, 1.0f, 0.9f, 0.9f);
        geom_of(si, g);
        CHECK(near4(g[2], 0.45f) && near4(g[3], 0.19f), "strip radii -> %.4f,%.4f want 0.45,0.19", g[2], g[3]);
        CHECK(near4(g[0], 0.45f) && near4(g[1], 0.81f), "strip centre -> %.4f,%.4f want 0.45,0.81", g[0], g[1]);
    }
    reset();
}

/* 21c. Reset restores the preset; layouts are independent (id 7 is RT in PAD
 * and GAS in DRIVE). */
static void t_geom_layouts(void)
{
    int zp, zd;
    float pad0[4], drv0[4], g[4];
    unsigned gen;

    reset();
    zp = zone_by_id(7);
    geom_of(zp, pad0);
    xbox_ios_touch_set_layout(1);
    zd = zone_by_id(7);
    geom_of(zd, drv0);
    CHECK(zp >= 0 && zd >= 0, "no zone with id 7");
    CHECK(!near4(pad0[0], drv0[0]) || !near4(pad0[1], drv0[1]), "test setup: id 7 is at the same place in both");

    /* Edit DRIVE 7: PAD 7 does not move. */
    xbox_ios_touch_zone_set_geom(zd, 0.60f, 0.60f, 0.05f, 0.10f);
    geom_of(zd, g);
    CHECK(near4(g[0], 0.60f) && near4(g[1], 0.60f), "DRIVE edit not applied");
    xbox_ios_touch_set_layout(0);
    zp = zone_by_id(7);
    geom_of(zp, g);
    CHECK(near4(g[0], pad0[0]) && near4(g[1], pad0[1]) && near4(g[2], pad0[2]) && near4(g[3], pad0[3]),
          "PAD 7 changed by a DRIVE edit: %.4f,%.4f", g[0], g[1]);

    /* Edit PAD 7, then reset DRIVE: PAD keeps its edit, DRIVE is back. */
    xbox_ios_touch_zone_set_geom(zp, 0.30f, 0.30f, 0.05f, 0.10f);
    gen = xbox_ios_touch_layout_gen();
    xbox_ios_touch_layout_reset(1);
    CHECK(xbox_ios_touch_layout_gen() != gen, "layout_reset did not bump the generation");
    geom_of(zp, g);
    CHECK(near4(g[0], 0.30f) && near4(g[1], 0.30f), "PAD edit lost by a DRIVE reset");
    xbox_ios_touch_set_layout(1);
    zd = zone_by_id(7);
    geom_of(zd, g);
    CHECK(near4(g[0], drv0[0]) && near4(g[1], drv0[1]) && near4(g[2], drv0[2]) && near4(g[3], drv0[3]),
          "DRIVE 7 not back at the preset after layout_reset(1)");
    /* The hit area is back too. */
    CHECK(zone_hit(drv0[0], drv0[1]) == zd, "preset centre does not hit the restored zone");
    CHECK(zone_hit(0.60f, 0.60f) != zd, "the edited spot still hits the reset zone");

    /* Reset of the layout that is not active works as well. */
    xbox_ios_touch_set_layout(0);
    xbox_ios_touch_layout_reset(0);
    zp = zone_by_id(7);
    geom_of(zp, g);
    CHECK(near4(g[0], pad0[0]) && near4(g[1], pad0[1]), "PAD 7 not back after layout_reset(0)");
    reset();
}

/* 21d. layout_gen: bumps on a change and a reset, not on reads or no-ops. */
static void t_geom_gen(void)
{
    int zi;
    float g[4];
    unsigned g0, g1;

    reset();
    zi = zone_by_label("B");
    if (zi < 0)
        return;
    geom_of(zi, g);
    g0 = xbox_ios_touch_layout_gen();
    (void)xbox_ios_touch_zone_geom(zi, NULL, NULL, NULL, NULL);
    CHECK(xbox_ios_touch_layout_gen() == g0, "zone_geom bumped the generation");
    xbox_ios_touch_zone_set_geom(zi, g[0], g[1], g[2], g[3]);   /* unchanged */
    CHECK(xbox_ios_touch_layout_gen() == g0, "a no-op set_geom bumped the generation");
    {
        char buf[512];
        (void)xbox_ios_touch_layout_format(buf, sizeof buf);
        CHECK(strstr(buf, "\nv1 ") == NULL, "a no-op set_geom created an override");
    }
    xbox_ios_touch_zone_set_geom(zi, g[0] - 0.05f, g[1] - 0.05f, g[2], g[3]);
    g1 = xbox_ios_touch_layout_gen();
    CHECK(g1 != g0, "set_geom did not bump the generation");
    xbox_ios_touch_zone_set_geom(zi, g[0] - 0.05f, g[1] - 0.05f, g[2], g[3]);
    CHECK(xbox_ios_touch_layout_gen() == g1, "repeating the same set_geom bumped again");
    xbox_ios_touch_layout_reset(0);
    CHECK(xbox_ios_touch_layout_gen() != g1, "layout_reset did not bump the generation");
    g0 = xbox_ios_touch_layout_gen();
    CHECK(xbox_ios_touch_layout_parse("v1 pad 3 0.5 0.5 0.05 0.1\n") == 1, "parse of one line");
    CHECK(xbox_ios_touch_layout_gen() != g0, "parse did not bump the generation");
    reset();
}

/* 21e. format -> parse reproduces the geometry, and the text itself. */
static void t_geom_roundtrip(void)
{
    static const int ids_pad[3] = { 8, 3, 1 };
    static const int ids_drv[3] = { 7, 12, 4 };
    char t1[4096], t2[4096];
    float want_pad[3][4], want_drv[3][4], g[4];
    float pre_pad[3][4], pre_drv[3][4];
    int k, zi, n1, n2, applied;

    reset();
    n1 = xbox_ios_touch_layout_format(t1, sizeof t1);
    CHECK(n1 > 0 && t1[0] == '#' && (size_t)n1 == strlen(t1), "empty format: header comment only, got %d", n1);
    CHECK(strstr(t1, "\nv1 ") == NULL, "an empty layout has override lines");

    for (k = 0; k < 3; k++) {
        zi = zone_by_id(ids_pad[k]);
        geom_of(zi, pre_pad[k]);
        xbox_ios_touch_zone_set_geom(zi, 0.30f + 0.1234567f * (float)k, 0.40f + 0.0777777f * (float)k,
                                     0.04f + 0.0111111f * (float)k, 0.09f + 0.0222222f * (float)k);
        geom_of(zi, want_pad[k]);
    }
    xbox_ios_touch_set_layout(1);
    for (k = 0; k < 3; k++) {
        zi = zone_by_id(ids_drv[k]);
        geom_of(zi, pre_drv[k]);
        xbox_ios_touch_zone_set_geom(zi, 0.35f + 0.0987654f * (float)k, 0.45f + 0.0666666f * (float)k,
                                     0.05f + 0.0123456f * (float)k, 0.10f + 0.0212121f * (float)k);
        geom_of(zi, want_drv[k]);
    }
    n1 = xbox_ios_touch_layout_format(t1, sizeof t1);
    CHECK(n1 > 0 && (size_t)n1 == strlen(t1), "format length %d vs strlen %zu", n1, strlen(t1));
    {
        int lines = 0;
        const char *p = t1;
        while ((p = strstr(p, "\nv1 ")) != NULL) {
            lines++;
            p++;
        }
        CHECK(lines == 6, "%d override lines, want 6", lines);
        CHECK(strstr(t1, "v1 pad 1 ") && strstr(t1, "v1 drive 12 "), "expected lines missing");
        CHECK(strstr(t1, "v1 pad 1 ") < strstr(t1, "v1 pad 3 ") && strstr(t1, "v1 pad 8 ") < strstr(t1, "v1 drive 4 "),
              "lines are not PAD first, ids ascending");
    }

    /* Wipe everything, check it is gone, parse it back. */
    xbox_ios_touch_layout_reset(0);
    xbox_ios_touch_layout_reset(1);
    zi = zone_by_id(ids_drv[0]);
    geom_of(zi, g);
    CHECK(near4(g[0], pre_drv[0][0]) && near4(g[1], pre_drv[0][1]), "setup: reset did not clear");
    applied = xbox_ios_touch_layout_parse(t1);
    CHECK(applied == 6, "parse applied %d lines, want 6", applied);
    for (k = 0; k < 3; k++) {
        zi = zone_by_id(ids_drv[k]);
        geom_of(zi, g);
        CHECK(near4(g[0], want_drv[k][0]) && near4(g[1], want_drv[k][1]) &&
              near4(g[2], want_drv[k][2]) && near4(g[3], want_drv[k][3]),
              "DRIVE id %d: %.4f,%.4f,%.4f,%.4f want %.4f,%.4f,%.4f,%.4f", ids_drv[k], g[0], g[1], g[2], g[3],
              want_drv[k][0], want_drv[k][1], want_drv[k][2], want_drv[k][3]);
    }
    xbox_ios_touch_set_layout(0);
    for (k = 0; k < 3; k++) {
        zi = zone_by_id(ids_pad[k]);
        geom_of(zi, g);
        CHECK(near4(g[0], want_pad[k][0]) && near4(g[1], want_pad[k][1]) &&
              near4(g[2], want_pad[k][2]) && near4(g[3], want_pad[k][3]),
              "PAD id %d: %.4f,%.4f,%.4f,%.4f want %.4f,%.4f,%.4f,%.4f", ids_pad[k], g[0], g[1], g[2], g[3],
              want_pad[k][0], want_pad[k][1], want_pad[k][2], want_pad[k][3]);
    }
    n2 = xbox_ios_touch_layout_format(t2, sizeof t2);
    CHECK(n2 == n1 && strcmp(t1, t2) == 0, "format after parse differs:\n%s---\n%s", t1, t2);

    /* parse REPLACES: a text with one line leaves exactly that one. */
    CHECK(xbox_ios_touch_layout_parse("v1 pad 3 0.5 0.5 0.05 0.1\n") == 1, "single-line parse");
    n2 = xbox_ios_touch_layout_format(t2, sizeof t2);
    CHECK(strstr(t2, "v1 pad 3 ") && !strstr(t2, "v1 pad 8 ") && !strstr(t2, "v1 drive"),
          "parse did not replace the old overrides:\n%s", t2);
    zi = zone_by_id(ids_pad[0]);
    geom_of(zi, g);
    CHECK(near4(g[0], pre_pad[0][0]) && near4(g[1], pre_pad[0][1]), "an override outlived a replacing parse");

    /* Format into small buffers: no overflow, NUL, snprintf length. */
    {
        char tiny[8];
        char guard[2][16];
        int full;
        xbox_ios_touch_layout_parse(t1);
        full = xbox_ios_touch_layout_format(NULL, 0);
        CHECK(full == n1, "format(NULL,0) = %d, want %d", full, n1);
        memset(tiny, 'x', sizeof tiny);
        n2 = xbox_ios_touch_layout_format(tiny, 0);
        CHECK(n2 == n1 && tiny[0] == 'x', "format(buf,0) wrote or returned %d", n2);
        n2 = xbox_ios_touch_layout_format(tiny, sizeof tiny);
        CHECK(n2 == n1 && tiny[sizeof tiny - 1] == 0 && strlen(tiny) == sizeof tiny - 1 &&
              strncmp(tiny, t1, sizeof tiny - 1) == 0, "truncated format: len %d, text \"%s\"", n2, tiny);
        memset(guard, 0x7E, sizeof guard);
        n2 = xbox_ios_touch_layout_format(guard[0], 1);
        CHECK(n2 == n1 && guard[0][0] == 0 && guard[0][1] == 0x7E, "format(buf,1) not just a NUL");
    }
    reset();
}

/* 21f. parse ignores garbage, unknown ids / layouts / versions, non-finite
 * numbers, accepts CRLF and clamps. */
static void t_geom_parse(void)
{
    static const char text[] =
        "# a comment line\n"
        "garbage\n"
        "\n"
        "v1 pad 3 0.5 0.5 0.05 0.1\n"            /* ok */
        "v1 pad 99 0.5 0.5 0.05 0.1\n"           /* unknown id */
        "v1 xbox 3 0.5 0.5 0.05 0.1\n"           /* unknown layout */
        "v2 pad 4 0.5 0.5 0.05 0.1\n"            /* unknown version */
        "v1 drive 12 2.0 -3 9 9\n"               /* ok, clamped */
        "v1 pad 4 nan 0.5 0.05 0.1\n"            /* not finite */
        "v1 pad 5 0.5 inf 0.05 0.1\n"            /* not finite */
        "v1 pad 6 0.5 0.5\n"                     /* short */
        "v1 drive 4 abc 0.5 0.05 0.1\n"          /* not a number */
        "  v1 pad 5 0.3 0.3 0.04 0.09\r\n"       /* ok: CRLF, leading blanks */
        "v1 pad 11 0.5 0.5 0.05 0.1";            /* ok: no final newline */
    int applied, zi;
    float g[4];
    char out[1024];

    reset();
    applied = xbox_ios_touch_layout_parse(text);
    CHECK(applied == 4, "parse applied %d lines, want 4", applied);
    zi = zone_by_id(3);
    geom_of(zi, g);
    CHECK(near4(g[0], 0.5f) && near4(g[1], 0.5f) && near4(g[2], 0.05f) && near4(g[3], 0.1f), "pad 3 wrong");
    zi = zone_by_id(5);
    geom_of(zi, g);
    CHECK(near4(g[0], 0.3f) && near4(g[1], 0.3f) && near4(g[2], 0.04f) && near4(g[3], 0.09f), "pad 5 (CRLF) wrong");
    zi = zone_by_id(4);
    geom_of(zi, g);
    {
        float preset_cx = zone(zi)->cx;
        CHECK(near4(g[0], preset_cx), "pad 4 moved by a line that was garbage");
    }
    xbox_ios_touch_set_layout(1);
    zi = zone_by_id(12);
    if (zi >= 0) {
        geom_of(zi, g);
        CHECK(near4(g[2], 0.45f) && near4(g[3], 0.19f) && near4(g[0], 0.55f) && near4(g[1], 0.19f),
              "drive 12 not clamped: %.4f,%.4f,%.4f,%.4f", g[0], g[1], g[2], g[3]);
    } else {
        record_fail(__LINE__, "no zone with id 12 in DRIVE");
    }
    xbox_ios_touch_set_layout(0);

    /* Our own header line is a comment. */
    xbox_ios_touch_layout_format(out, sizeof out);
    CHECK(xbox_ios_touch_layout_parse(out) == 4, "the formatted text does not parse back to 4 lines");

    /* NULL changes nothing; "" clears everything. */
    CHECK(xbox_ios_touch_layout_parse(NULL) == 0, "parse(NULL) != 0");
    xbox_ios_touch_layout_format(out, sizeof out);
    CHECK(strstr(out, "v1 pad 3 ") != NULL, "parse(NULL) dropped the overrides");
    CHECK(xbox_ios_touch_layout_parse("") == 0, "parse(\"\") != 0");
    xbox_ios_touch_layout_format(out, sizeof out);
    CHECK(strstr(out, "\nv1 ") == NULL, "parse(\"\") left overrides");
    zi = zone_by_id(3);
    geom_of(zi, g);
    CHECK(near4(g[0], zone(zi)->cx), "zone 3 table and geom disagree after a clear");

    /* A line longer than the parser's buffer is garbage, not a crash. */
    {
        char *big = malloc(100100);
        if (big) {
            memset(big, 'a', 99990);
            memcpy(big + 99990, "\nv1 pad 3 0.5 0.5 0.05 0.1\n", 28);
            CHECK(xbox_ios_touch_layout_parse(big) == 1, "long garbage line broke the parser");
            free(big);
        }
    }
    reset();
}

/* 21g. An overridden zone stays on screen at the largest size; the strip keeps
 * its width; the classic DRIVE table takes the DRIVE overrides by id. */
static void t_geom_on_screen(void)
{
    int zi, si;
    float g[4], classic7[4];
    const XBOX_IOS_TOUCH_ZONE *z;

    reset();
    /* The classic DRIVE preset of id 7, read now (never hard-coded). */
    xbox_ios_touch_set_layout(1);
    xbox_ios_touch_set_drive_classic(1);
    geom_of(zone_by_id(7), classic7);
    xbox_ios_touch_set_drive_classic(0);
    xbox_ios_touch_set_layout(0);
    zi = zone_by_label("A");
    if (zi < 0)
        return;
    geom_of(zi, g);
    xbox_ios_touch_zone_set_geom(zi, 0.0f, 1.0f, g[2], g[3]);   /* corner, clamped on base extents */
    xbox_ios_touch_set_scale(1.5f);
    z = zone(zi);
    CHECK(z && z->cx - z->rx >= -1e-6f && z->cx + z->rx <= 1.0f + 1e-6f &&
          z->cy - z->ry >= -1e-6f && z->cy + z->ry <= 1.0f + 1e-6f,
          "overridden zone leaves the screen at 1.5x: c %.4f,%.4f r %.4f,%.4f",
          z ? z->cx : -1.0f, z ? z->cy : -1.0f, z ? z->rx : -1.0f, z ? z->ry : -1.0f);
    CHECK(z && near4(z->rx, g[2] * 1.5f), "radius at 1.5x is %.4f", z ? z->rx : -1.0f);
    geom_of(zi, g);
    CHECK(near4(g[0], g[2]) && near4(g[1], 1.0f - g[3]), "base centre was moved by the scale");

    /* Strip: its width does not follow the size setting, its centre is its own. */
    xbox_ios_touch_set_scale(1.0f);
    xbox_ios_touch_set_layout(1);
    si = zone_by_kind(XBOX_IOS_ZONE_STEER);
    if (si >= 0) {
        xbox_ios_touch_zone_set_geom(si, 0.45f, 0.50f, 0.25f, 0.15f);
        xbox_ios_touch_set_scale(1.5f);
        z = zone(zone_by_kind(XBOX_IOS_ZONE_STEER));
        CHECK(z && near4(z->rx, 0.25f) && near4(z->cx, 0.45f) && near4(z->cy, 0.50f) && near4(z->ry, 0.19f),
              "strip at 1.5x: c %.4f,%.4f r %.4f,%.4f", z ? z->cx : -1.0f, z ? z->cy : -1.0f,
              z ? z->rx : -1.0f, z ? z->ry : -1.0f);
        xbox_ios_touch_set_scale(1.0f);
        /* A finger on the new strip steers; the old preset spot no longer does. */
        {
            XBOX_INPUT_STATE s;
            xbox_ios_touch_set_steer_mode(0);        /* absolute: the offset from the centre */
            fdown(1, 0.45f + 0.20f, 0.50f);
            s = rd();
            CHECK(s.Gamepad.sThumbLX > 5000, "strip finger right of centre: LX=%d", s.Gamepad.sThumbLX);
            fup(1);
        }
    }

    /* Classic DRIVE: ids 1..11 take the same overrides. */
    reset();
    xbox_ios_touch_set_layout(1);
    zi = zone_by_id(7);
    xbox_ios_touch_zone_set_geom(zi, 0.60f, 0.60f, 0.05f, 0.10f);
    xbox_ios_touch_set_drive_classic(1);
    zi = zone_by_id(7);
    geom_of(zi, g);
    CHECK(zi >= 0 && near4(g[0], 0.60f) && near4(g[1], 0.60f), "classic DRIVE ignores the DRIVE override");
    xbox_ios_touch_layout_reset(1);
    geom_of(zone_by_id(7), g);
    CHECK(near4(g[0], classic7[0]) && near4(g[1], classic7[1]) && near4(g[2], classic7[2]),
          "classic preset not back after reset: %.4f,%.4f", g[0], g[1]);
    reset();
}


/* ------------------------------------------------------------------ */
/* Screen adaptation: safe area, aspect, tablet size                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int n;
    XBOX_IOS_TOUCH_ZONE z[16];
} Snap;

static void snap_take(Snap *sn)
{
    int n = zcount(), i;
    sn->n = n > 16 ? 16 : n;
    for (i = 0; i < sn->n; i++)
        sn->z[i] = *zone(i);
}

static int snap_same(const Snap *sn)
{
    int n = zcount(), i;
    if (n != sn->n)
        return 0;
    for (i = 0; i < n; i++)
        if (memcmp(zone(i), &sn->z[i], sizeof sn->z[i]) != 0)
            return 0;
    return 1;
}

#define PHONE_A (852.0f / 393.0f)

/* The rigid per-half shift, worked out here from a snapshot of the table
 * without insets: left group cx < 0.5, right group the rest (zones wider than
 * the band excluded). Checks every zone of the active table against it, plus
 * radii / cy unchanged and rigidity (pairwise centre distances inside a group
 * equal those of the no-inset table). Returns the shifts via *shl / *shr. */
static void expect_shifted(const Snap *base, float L, float R, const char *what, float *shl, float *shr)
{
    float minl = 2.0f, maxr = -1.0f, sl = 0.0f, sr = 0.0f;
    int i, j;

    for (i = 0; i < base->n; i++) {
        const XBOX_IOS_TOUCH_ZONE *b = &base->z[i];
        if (b->cx < 0.5f) {
            if (b->cx - b->rx < minl) minl = b->cx - b->rx;
        } else if (b->cx + b->rx > maxr) {
            maxr = b->cx + b->rx;
        }
    }
    if (minl < L) sl = L - minl;
    if (maxr > 1.0f - R) sr = maxr - (1.0f - R);
    CHECK(zcount() == base->n, "%s: zone count %d vs %d", what, zcount(), base->n);
    for (i = 0; i < base->n && i < zcount(); i++) {
        const XBOX_IOS_TOUCH_ZONE *z = zone(i), *b = &base->z[i];
        float want = b->cx + (b->cx < 0.5f ? sl : -sr);
        CHECK(z->id == b->id, "%s: order changed at %d", what, i);
        CHECK(near4(z->cx, want), "%s zone %d: cx %.4f, want %.4f", what, b->id, z->cx, want);
        CHECK(z->rx == b->rx && z->ry == b->ry && z->cy == b->cy,
              "%s zone %d: radii or y changed (rx %.4f vs %.4f)", what, b->id, z->rx, b->rx);
        CHECK(z->cx - z->rx >= L - 1e-5f && z->cx + z->rx <= 1.0f - R + 1e-5f,
              "%s zone %d: leaves the safe band", what, b->id);
        for (j = i + 1; j < base->n && j < zcount(); j++) {
            const XBOX_IOS_TOUCH_ZONE *y = zone(j), *c = &base->z[j];
            if ((b->cx < 0.5f) == (c->cx < 0.5f))
                CHECK(fabsf((y->cx - z->cx) - (c->cx - b->cx)) < 1e-5f,
                      "%s: zones %d and %d moved relative to each other", what, b->id, c->id);
        }
    }
    if (shl) *shl = sl;
    if (shr) *shr = sr;
}

/* 22. Safe area: a rigid shift per half (left group right, right group left)
 * until its outermost edge is inside the inset; radii, y and spacing unchanged;
 * both layouts; clamps; disabled and zero insets give today's table. */
static void t_safe_map(void)
{
    static const float L = 0.07f, R = 0.03f;
    Snap base;
    int lay;
    float l, r;

    for (lay = 0; lay < 2; lay++) {
        reset();
        xbox_ios_touch_set_layout(lay);
        snap_take(&base);
        CHECK(base.n > 0, "layout %d: no zones", lay);
        xbox_ios_touch_set_safe_area(L, R);
        expect_shifted(&base, L, R, lay ? "DRIVE" : "PAD", NULL, NULL);
        xbox_ios_touch_get_safe_area(&l, &r);
        CHECK(near4(l, L) && near4(r, R), "get_safe_area %.4f %.4f", l, r);

        /* The setting off: today's table, insets reported as zero. */
        xbox_ios_touch_set_safe_enabled(0);
        CHECK(snap_same(&base), "layout %d: safe area off does not give today's table", lay);
        xbox_ios_touch_get_safe_area(&l, &r);
        CHECK(l == 0.0f && r == 0.0f, "get_safe_area %.4f %.4f while off", l, r);
        xbox_ios_touch_set_safe_enabled(1);
        CHECK(!snap_same(&base), "layout %d: back on does not shift again", lay);
        /* Zero insets: bit-identical to today. */
        xbox_ios_touch_set_safe_area(0.0f, 0.0f);
        CHECK(snap_same(&base), "layout %d: 0 insets differ from today's table", lay);
    }

    /* Clamps: each side 0..0.25, NaN = 0. */
    reset();
    xbox_ios_touch_set_safe_area(0.9f, -3.0f);
    xbox_ios_touch_get_safe_area(&l, &r);
    CHECK(near4(l, 0.25f) && r == 0.0f, "clamp: %.4f %.4f, want 0.25 0", l, r);
    xbox_ios_touch_set_safe_area(NAN, 0.4f);
    xbox_ios_touch_get_safe_area(&l, &r);
    CHECK(l == 0.0f && near4(r, 0.25f), "clamp NaN: %.4f %.4f, want 0 0.25", l, r);

    /* Order with the other stages: override, size, aspect, tablet, then the shift. */
    reset();
    {
        int a = zone_by_label("A");
        float g[4];
        if (a >= 0) {
            geom_of(a, g);
            xbox_ios_touch_zone_set_geom(a, 0.50f, 0.50f, g[2], g[3]);
            xbox_ios_touch_set_scale(1.2f);
            xbox_ios_touch_set_aspect(1024.0f / 768.0f);
            xbox_ios_touch_set_tablet_scale(0.8f);
            snap_take(&base);                           /* everything but the insets */
            CHECK(near4(zone(zone_by_label("A"))->cx, 0.50f), "combined: override moved");
            xbox_ios_touch_set_safe_area(0.06f, 0.04f);
            expect_shifted(&base, 0.06f, 0.04f, "combined", NULL, NULL);
            geom_of(zone_by_label("A"), g);
            CHECK(near4(g[0], 0.50f) && near4(g[1], 0.50f), "zone_geom is not the base value");
        }
    }
    reset();
}

/* 22b. Hit-test and drawing use the shifted table; the strip keeps its width and gain. */
static void t_safe_hit(void)
{
    XBOX_INPUT_STATE s;
    int a, d, st;
    const XBOX_IOS_TOUCH_ZONE *z;
    float bx, by;

    reset();
    xbox_ios_touch_set_steer_mode(0);        /* the strip part is about the absolute gain */
    a = zone_by_label("A");
    if (a < 0)
        return;
    bx = zone(a)->cx;
    by = zone(a)->cy;
    xbox_ios_touch_set_safe_area(0.20f, 0.20f);
    z = zone(zone_by_label("A"));
    CHECK(z && z->cx < bx - 0.05f, "A centre %.4f did not move left of %.4f", z ? z->cx : -1.0f, bx);
    /* The old spot is bare now (a stick touch), the new centre presses A. */
    CHECK(zone_hit(bx, by) != zone_by_label("A"), "the unshifted centre still hits A");
    fdown(1, bx, by);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0, "A pressed at its unshifted spot");
    fup(1);
    fdown(2, z ? z->cx : 0.0f, z ? z->cy : 0.0f);
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not pressed at its shifted centre");
    CHECK(xbox_ios_touch_zone_pressed(zone_by_label("A")), "zone_pressed false at the shifted centre");
    fup(2);

    /* D-pad quadrants follow the shifted centre. */
    d = zone_by_kind(XBOX_IOS_ZONE_DPAD);
    if (d >= 0) {
        z = zone(d);
        fdown(3, z->cx + 0.6f * z->rx, z->cy);
        s = rd();
        CHECK(s.Gamepad.wButtons == XBOX_GAMEPAD_DPAD_RIGHT, "shifted d-pad right: 0x%x", s.Gamepad.wButtons);
        fup(3);
    }

    /* The strip: rx unchanged, moves with its half (same shift as BACK), same gain. */
    xbox_ios_touch_set_layout(1);
    xbox_ios_touch_set_safe_area(0.0f, 0.0f);
    st = zone_by_kind(XBOX_IOS_ZONE_STEER);
    if (st >= 0) {
        float rx0 = zone(st)->rx, cx0 = zone(st)->cx, back0 = zone(zone_by_flag(XBOX_GAMEPAD_BACK))->cx;
        float shift;
        xbox_ios_touch_set_safe_area(0.10f, 0.10f);
        z = zone(zone_by_kind(XBOX_IOS_ZONE_STEER));
        shift = zone(zone_by_flag(XBOX_GAMEPAD_BACK))->cx - back0;
        CHECK(shift > 0.05f, "BACK did not shift right (%.4f)", shift);
        CHECK(z && near4(z->rx, rx0) && near4(z->cx, cx0 + shift),
              "strip: c %.4f r %.4f (rx must stay %.4f, cx %.4f)", z ? z->cx : -1.0f, z ? z->rx : -1.0f, rx0, cx0 + shift);
        fdown(4, z->cx + z->rx, z->cy);
        s = rd();
        CHECK(s.Gamepad.sThumbLX > 30000, "strip edge: LX=%d, want full lock", s.Gamepad.sThumbLX);
        fup(4);
        /* Gain unchanged: the same finger offset gives the same value. */
        z = zone(zone_by_kind(XBOX_IOS_ZONE_STEER));
        fdown(5, z->cx + 0.5f * z->rx, z->cy);
        s = rd();
        xbox_ios_touch_set_safe_area(0.0f, 0.0f);
        z = zone(zone_by_kind(XBOX_IOS_ZONE_STEER));
        fup(5);
        fdown(6, z->cx + 0.5f * z->rx, z->cy);
        {
            XBOX_INPUT_STATE s0 = rd();
            CHECK(s.Gamepad.sThumbLX == s0.Gamepad.sThumbLX, "strip gain changed with the insets (%d vs %d)",
                  s.Gamepad.sThumbLX, s0.Gamepad.sThumbLX);
        }
        fup(6);
    }
    /* A zone against the right edge: the right half moves in until it is inside. */
    reset();
    {
        int ia = zone_by_label("A");
        if (ia >= 0) {
            float g[4];
            geom_of(ia, g);
            xbox_ios_touch_zone_set_geom(ia, 1.0f, 0.5f, g[2], g[3]);   /* base: against the right edge */
            xbox_ios_touch_set_safe_area(0.0f, 0.20f);
            z = zone(zone_by_label("A"));
            CHECK(z && near4(z->cx, 0.80f - g[2]) && near4(z->rx, g[2]), "edge zone: cx %.4f rx %.4f, want %.4f %.4f",
                  z ? z->cx : -1.0f, z ? z->rx : -1.0f, 0.80f - g[2], g[2]);
        }
    }
    /* A strip wider than the band shrinks to it and is centred. */
    reset();
    xbox_ios_touch_set_layout(1);
    st = zone_by_kind(XBOX_IOS_ZONE_STEER);
    if (st >= 0) {
        xbox_ios_touch_zone_set_geom(st, 0.5f, 0.79f, 0.40f, 0.15f);
        xbox_ios_touch_set_safe_area(0.25f, 0.25f);                      /* band half-width 0.25 */
        z = zone(zone_by_kind(XBOX_IOS_ZONE_STEER));
        CHECK(z && near4(z->rx, 0.25f) && near4(z->cx, 0.50f), "wide strip: c %.4f r %.4f, want 0.5 0.25",
              z ? z->cx : -1.0f, z ? z->rx : -1.0f);
    }
    reset();
}

/* Signed gap in points between zones i and j on a W x H screen: > 0 the
 * distance between the shapes, <= 0 minus the deepest sampled penetration.
 * Ellipses and the strip's rectangle are sampled; good to about a point. */
static void shape_pt(const XBOX_IOS_TOUCH_ZONE *z, float t, float *x, float *y, float W, float H)
{
    float a = t * 6.2831853f;
    if (z->kind == XBOX_IOS_ZONE_STEER) {            /* rectangle boundary, t in 0..1 */
        float u = t * 4.0f;
        float cx = z->cx * W, cy = z->cy * H, hx = z->rx * W, hy = z->ry * H;
        int side = (int)u > 3 ? 3 : (int)u;
        float f = u - (float)side;
        *x = side == 0 ? cx - hx + 2 * hx * f : side == 2 ? cx + hx - 2 * hx * f : side == 1 ? cx + hx : cx - hx;
        *y = side == 1 ? cy - hy + 2 * hy * f : side == 3 ? cy + hy - 2 * hy * f : side == 0 ? cy - hy : cy + hy;
    } else {
        *x = z->cx * W + z->rx * W * cosf(a);
        *y = z->cy * H + z->ry * H * sinf(a);
    }
}

static float zone_gap_pt(int i, int j, float W, float H)
{
    const XBOX_IOS_TOUCH_ZONE *zi = zone(i), *zj = zone(j);
    int a, b, steps = 720, hit = 0;
    float best = 1e9f, depth = 0.0f;

    for (a = 0; a < steps; a++) {
        float xa, ya;
        shape_pt(zi, (float)a / (float)steps, &xa, &ya, W, H);
        for (b = 0; b < steps; b += 2) {
            float xb, yb, d;
            shape_pt(zj, (float)b / (float)steps, &xb, &yb, W, H);
            d = sqrtf((xa - xb) * (xa - xb) + (ya - yb) * (ya - yb));
            if (d < best)
                best = d;
        }
    }
    for (a = -30; a <= 30; a++)
        for (b = -30; b <= 30; b++) {
            float x = zi->cx + zi->rx * (float)a / 30.0f, y = zi->cy + zi->ry * (float)b / 30.0f;
            if (in_shape(zi, x, y) && in_shape(zj, x, y)) {
                int c;
                float dmin = 1e9f;
                hit = 1;
                for (c = 0; c < steps; c += 2) {
                    float xb, yb, d;
                    shape_pt(zj, (float)c / (float)steps, &xb, &yb, W, H);
                    d = sqrtf((x * W - xb) * (x * W - xb) + (y * H - yb) * (y * H - yb));
                    if (d < dmin)
                        dmin = d;
                }
                if (dmin > depth)
                    depth = dmin;
            }
        }
    return hit ? -depth : best;
}

/* 22d. With the Face ID insets (59 pt each side of 852) and Size 1.0 no two
 * zones of PAD, DRIVE or classic DRIVE overlap. Reports the smallest gap
 * (TOUCHTEST_GEOMETRY=1 prints it and every pair under 8 pt). */
static void t_safe_overlap(void)
{
    const float W = 852.0f, H = 393.0f, inset = 59.0f / 852.0f;
    static const char *names[3] = { "PAD", "DRIVE", "DRIVE classic" };
    int print = getenv("TOUCHTEST_GEOMETRY") != NULL;
    int lay;

    for (lay = 0; lay < 3; lay++) {
        int n, i, j, wi = -1, wj = -1;
        float worst = 1e9f, w0 = 1e9f, shl, shr;
        Snap snap;

        reset();
        xbox_ios_touch_set_layout(lay > 0);
        xbox_ios_touch_set_drive_classic(lay == 2);
        xbox_ios_touch_set_scale(1.0f);
        snap_take(&snap);                                /* reference: today's table, no insets */
        n = zcount();
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++) {
                float g = zone_gap_pt(i, j, W, H);
                if (g < w0)
                    w0 = g;
            }
        xbox_ios_touch_set_safe_area(inset, inset);
        expect_shifted(&snap, inset, inset, names[lay], &shl, &shr);   /* rigid, in the band */
        n = zcount();
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++) {
                float g = zone_gap_pt(i, j, W, H);
                if (g < worst) {
                    worst = g;
                    wi = i;
                    wj = j;
                }
                if (print && g < 8.0f)
                    printf("   gap  %-13s id %2d (%s) / id %2d (%s): %.1f pt\n", names[lay], zone(i)->id,
                           xbox_ios_touch_zone_label(i) ? xbox_ios_touch_zone_label(i) : "-", zone(j)->id,
                           xbox_ios_touch_zone_label(j) ? xbox_ios_touch_zone_label(j) : "-", g);
                CHECK(g > 0.0f, "%s: zones id %d (%s) and id %d (%s) overlap by %.1f pt", names[lay],
                      zone(i)->id, xbox_ios_touch_zone_label(i) ? xbox_ios_touch_zone_label(i) : "-",
                      zone(j)->id, xbox_ios_touch_zone_label(j) ? xbox_ios_touch_zone_label(j) : "-", -g);
            }
        if (print && lay == 1) {                          /* the strip with the insets, and its nearest neighbour */
            int si = zone_find_kind(XBOX_IOS_ZONE_STEER), nb = -1;
            float ming = 1e9f, ming0;
            XBOX_IOS_TOUCH_ZONE zsv, zs0;
            const XBOX_IOS_TOUCH_ZONE *zs = si >= 0 ? &zsv : NULL;
            if (si >= 0)
                zsv = *zone(si);                          /* the table is rebuilt below */
            for (j = 0; zs && j < n; j++)
                if (j != si && zone_gap_pt(si, j, W, H) < ming) {
                    ming = zone_gap_pt(si, j, W, H);
                    nb = j;
                }
            xbox_ios_touch_set_safe_area(0.0f, 0.0f);
            if (si >= 0)
                zs0 = *zone(si);
            ming0 = 1e9f;
            for (j = 0; zs && j < zcount(); j++)
                if (j != si && zone_gap_pt(si, j, W, H) < ming0)
                    ming0 = zone_gap_pt(si, j, W, H);
            if (zs)
                printf("   STRIP with insets: x %.1f-%.1f pt; nearest zone id %d at %.1f pt (%.1f pt without insets: x %.1f-%.1f pt)\n",
                       (zs->cx - zs->rx) * W, (zs->cx + zs->rx) * W, nb >= 0 ? zone(nb)->id : -1, ming, ming0,
                       (zs0.cx - zs0.rx) * W, (zs0.cx + zs0.rx) * W);
            xbox_ios_touch_set_safe_area(inset, inset);
        }
        if (print && wi >= 0)
            printf("   %-13s shift left +%.1f pt, right -%.1f pt; smallest gap %.1f pt (id %d / id %d), %.1f pt without insets\n",
                   names[lay], shl * W, shr * W, worst, zone(wi)->id, zone(wj)->id, w0);
        for (i = 0; i < n; i++) {
            const XBOX_IOS_TOUCH_ZONE *z = zone(i);
            CHECK(z->cx - z->rx >= inset - 1e-5f && z->cx + z->rx <= 1.0f - inset + 1e-5f,
                  "%s: zone id %d leaves the safe band", names[lay], z->id);
        }
        (void)wj;
    }
    reset();
}

/* 22c. Layout editor in screen space: the per-zone shift and its inverse
 * round-trip, and zone_geom keeps returning base values. */
static void t_safe_editor(void)
{
    static const float L = 0.08f, R = 0.05f;
    int a, a2, i;
    float g[4], g2[4], sx, nsx, back;
    const XBOX_IOS_TOUCH_ZONE *z;

    reset();
    a = zone_by_label("A");
    if (a < 0)
        return;
    geom_of(a, g);
    xbox_ios_touch_set_safe_area(L, R);
    geom_of(zone_by_label("A"), g2);
    CHECK(near4(g[0], g2[0]) && near4(g[1], g2[1]) && near4(g[2], g2[2]) && near4(g[3], g2[3]),
          "zone_geom changed with the insets");
    for (i = 0; i < zcount(); i++) {
        float bx, by, br, bq;
        float s2;
        (void)xbox_ios_touch_zone_geom(i, &bx, &by, &br, &bq);
        s2 = xbox_ios_touch_base_to_screen_x(i, bx);
        CHECK(near4(s2, zone(i)->cx), "zone %d: base_to_screen %.4f, table cx %.4f", zone(i)->id, s2, zone(i)->cx);
        CHECK(near4(xbox_ios_touch_screen_to_base_x(i, s2), bx), "zone %d: round trip gives %.5f, base %.5f",
              zone(i)->id, xbox_ios_touch_screen_to_base_x(i, s2), bx);
    }
    CHECK(xbox_ios_touch_base_to_screen_x(-1, 0.3f) == 0.3f && xbox_ios_touch_screen_to_base_x(99, 0.3f) == 0.3f,
          "a bad index must return the value unchanged");
    a2 = zone_by_label("A");
    z = zone(a2);
    sx = z->cx;
    /* A drag of -0.10 of the width on screen moves the zone by exactly that. */
    nsx = sx - 0.10f;
    back = xbox_ios_touch_screen_to_base_x(a2, nsx);
    xbox_ios_touch_zone_set_geom(a2, back, g[1], g[2], g[3]);
    z = zone(zone_by_label("A"));
    CHECK(z && near4(z->cx, nsx), "after the drag the zone is at %.4f, want %.4f", z ? z->cx : -1.0f, nsx);
    geom_of(zone_by_label("A"), g2);
    CHECK(near4(g2[0], back), "stored base cx %.4f, want %.4f", g2[0], back);
    /* The override survives a change of the insets. */
    xbox_ios_touch_set_safe_area(0.0f, 0.0f);
    z = zone(zone_by_label("A"));
    CHECK(z && near4(z->cx, back), "without insets the zone is at %.4f, want base %.4f", z ? z->cx : -1.0f, back);
    /* Format text carries base values. */
    {
        char buf[512], want[64];
        (void)xbox_ios_touch_layout_format(buf, sizeof buf);
        snprintf(want, sizeof want, " 8 %.4f", back);
        CHECK(strstr(buf, want) != NULL, "layout text lacks the base value (%s)", want);
    }
    reset();
}

/* 23. Aspect: the phone's table is untouched, other aspects make the round
 * controls round in pixels, pedals and the strip keep their shapes. */
static void t_aspect_round(void)
{
    static const float A43 = 1024.0f / 768.0f;
    Snap base;
    int i, lay;

    reset();
    snap_take(&base);
    /* Within 10 % of 852/393 (1.95 .. 2.385), unknown, portrait: nothing. */
    xbox_ios_touch_set_aspect(PHONE_A);
    CHECK(snap_same(&base), "phone aspect changed the table");
    xbox_ios_touch_set_aspect(2.0f);
    CHECK(snap_same(&base), "aspect 2.0 changed the table");
    xbox_ios_touch_set_aspect(2.35f);
    CHECK(snap_same(&base), "aspect 2.35 changed the table");
    xbox_ios_touch_set_aspect(0.5f);
    CHECK(snap_same(&base), "a portrait aspect changed the table");
    xbox_ios_touch_set_aspect(NAN);
    CHECK(snap_same(&base), "NaN aspect changed the table");
    xbox_ios_touch_set_tablet_scale(0.8f);               /* only with a different aspect */
    CHECK(snap_same(&base), "the tablet factor applied on the phone aspect");
    xbox_ios_touch_set_tablet_scale(1.0f);

    for (lay = 0; lay < 2; lay++) {
        reset();
        xbox_ios_touch_set_layout(lay);
        snap_take(&base);
        xbox_ios_touch_set_aspect(A43);
        CHECK(!snap_same(&base), "layout %d: 4:3 left the table as is", lay);
        for (i = 0; i < base.n && i < zcount(); i++) {
            const XBOX_IOS_TOUCH_ZONE *z = zone(i), *b = &base.z[i];
            float px = z->rx * 1024.0f, py = z->ry * 768.0f;         /* radii in pixels */
            float phone = (b->ry * 393.0f) / (b->rx * 852.0f);       /* h/w of the phone's shape */
            int round = (b->kind == XBOX_IOS_ZONE_FLAG || b->kind == XBOX_IOS_ZONE_DPAD ||
                         (b->kind == XBOX_IOS_ZONE_ANALOG && b->value != XBOX_BUTTON_LTRIGGER &&
                          b->value != XBOX_BUTTON_RTRIGGER));
            CHECK(z->id == b->id && z->cx == b->cx && z->cy == b->cy, "layout %d zone %d: moved", lay, b->id);
            if (round) {
                CHECK(z->rx == b->rx, "layout %d zone %d: rx changed", lay, b->id);
                CHECK(fabsf(py / px - phone) < 1e-3f, "layout %d zone %d: shape %.4f, phone %.4f",
                      lay, b->id, py / px, phone);
                if (fabsf(phone - 1.0f) < 0.03f && b->kind != XBOX_IOS_ZONE_STEER)
                    CHECK(fabsf(py / px - 1.0f) < 0.03f, "layout %d zone %d: not round in pixels (%.3f)",
                          lay, b->id, py / px);
            } else {
                CHECK(z->rx == b->rx && z->ry == b->ry, "layout %d zone %d (pedal/trigger/strip): shape changed",
                      lay, b->id);
            }
        }
        xbox_ios_touch_set_round(0);
        CHECK(snap_same(&base), "layout %d: round off does not give today's table", lay);
        xbox_ios_touch_set_round(1);
        xbox_ios_touch_set_aspect(PHONE_A);
        CHECK(snap_same(&base), "layout %d: back to the phone aspect does not restore the table", lay);
    }

    /* Hit test in pixels on 4:3: 0.9 R presses A, 1.1 R does not. */
    reset();
    xbox_ios_touch_set_aspect(A43);
    {
        int a = zone_by_label("A");
        if (a >= 0) {
            const XBOX_IOS_TOUCH_ZONE *z = zone(a);
            float R = z->rx * 1024.0f, cx = z->cx, cy = z->cy;
            float dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            int k;
            for (k = 0; k < 4; k++) {
                float xi = cx + dirs[k][0] * 0.9f * R / 1024.0f, yi = cy + dirs[k][1] * 0.9f * R / 768.0f;
                float xo = cx + dirs[k][0] * 1.1f * R / 1024.0f, yo = cy + dirs[k][1] * 1.1f * R / 768.0f;
                CHECK(zone_hit(xi, yi) == a, "dir %d: 0.9 R misses A (hit %d)", k, zone_hit(xi, yi));
                CHECK(zone_hit(xo, yo) != a, "dir %d: 1.1 R still hits A", k);
            }
        }
    }
    reset();
}

/* 23b. Tablet size factor: on top of Size, only with a different aspect, clamped. */
static void t_tablet_scale(void)
{
    static const float A43 = 1024.0f / 768.0f;
    Snap base, ar;
    int i, lay;

    for (lay = 0; lay < 2; lay++) {
        reset();
        xbox_ios_touch_set_layout(lay);
        xbox_ios_touch_set_aspect(A43);
        snap_take(&ar);                                  /* aspect shape, factor 1.0 */
        xbox_ios_touch_set_aspect(0.0f);
        snap_take(&base);
        xbox_ios_touch_set_aspect(A43);
        xbox_ios_touch_set_tablet_scale(0.8f);
        for (i = 0; i < ar.n && i < zcount(); i++) {
            const XBOX_IOS_TOUCH_ZONE *z = zone(i), *b = &ar.z[i];
            if (b->kind == XBOX_IOS_ZONE_STEER) {
                CHECK(z->rx == b->rx && near4(z->ry, b->ry * 0.8f),
                      "strip: r %.4f,%.4f want %.4f,%.4f", z->rx, z->ry, b->rx, b->ry * 0.8f);
            } else {
                CHECK(near4(z->rx, b->rx * 0.8f) && near4(z->ry, b->ry * 0.8f),
                      "layout %d zone %d: r %.4f,%.4f want %.4f,%.4f", lay, b->id, z->rx, z->ry,
                      b->rx * 0.8f, b->ry * 0.8f);
            }
        }
        /* With the Size setting: multiplied, not replaced. */
        xbox_ios_touch_set_scale(1.25f);
        {
            int a = zone_by_id(7);
            const XBOX_IOS_TOUCH_ZONE *z = a >= 0 ? zone(a) : NULL;
            if (z && z->kind != XBOX_IOS_ZONE_STEER) {
                int j = -1, m;
                for (m = 0; m < ar.n; m++)
                    if (ar.z[m].id == 7)
                        j = m;
                CHECK(j >= 0 && near4(z->rx, ar.z[j].rx * 1.25f * 0.8f),
                      "size x tablet: rx %.4f want %.4f", z->rx, j >= 0 ? ar.z[j].rx * 1.25f * 0.8f : -1.0f);
            }
        }
        xbox_ios_touch_set_scale(1.0f);
        xbox_ios_touch_set_aspect(0.0f);
        CHECK(snap_same(&base), "layout %d: unknown aspect does not restore the table", lay);
    }

    /* Clamps 0.6 .. 1.2, NaN = 1.0. */
    reset();
    xbox_ios_touch_set_aspect(A43);
    snap_take(&base);
    xbox_ios_touch_set_tablet_scale(0.1f);
    CHECK(near4(zone(zone_by_label("A"))->rx, base.z[zone_by_label("A")].rx * 0.6f), "tablet 0.1 not clamped to 0.6");
    xbox_ios_touch_set_tablet_scale(9.0f);
    CHECK(near4(zone(zone_by_label("A"))->rx, base.z[zone_by_label("A")].rx * 1.2f), "tablet 9 not clamped to 1.2");
    xbox_ios_touch_set_tablet_scale(NAN);
    CHECK(snap_same(&base), "tablet NaN is not 1.0");
    reset();
}

/* 23c. Every adaptation setter is a no-op for a value already in force: no
 * rebuild, no generation bump, no finger dropped. A real change bumps once and
 * still keeps the finger. */
static void t_adapt_noop(void)
{
    static const float A43 = 1024.0f / 768.0f;
    int a;
    unsigned g0, g1;
    XBOX_INPUT_STATE s;

    reset();
    a = zone_by_label("A");
    if (a < 0)
        return;
    fdown(1, zone(a)->cx, zone(a)->cy);
    g0 = xbox_ios_touch_layout_gen();
    xbox_ios_touch_set_safe_area(0.0f, 0.0f);
    xbox_ios_touch_set_safe_enabled(1);
    xbox_ios_touch_set_aspect(0.0f);
    xbox_ios_touch_set_aspect(PHONE_A);                  /* both inactive */
    xbox_ios_touch_set_aspect(2.2f);
    xbox_ios_touch_set_round(1);
    xbox_ios_touch_set_round(0);                         /* nothing to re-shape on a phone aspect */
    xbox_ios_touch_set_round(1);
    xbox_ios_touch_set_tablet_scale(1.0f);
    xbox_ios_touch_set_tablet_scale(0.8f);               /* inactive aspect: no table change */
    xbox_ios_touch_set_tablet_scale(1.0f);
    xbox_ios_touch_set_safe_enabled(0);                  /* no insets: nothing to map */
    xbox_ios_touch_set_safe_enabled(1);
    CHECK(xbox_ios_touch_layout_gen() == g0, "a no-op setter bumped the generation");
    CHECK(xbox_ios_touch_zone_pressed(a), "a no-op setter dropped the held finger");

    xbox_ios_touch_set_safe_area(0.1f, 0.05f);
    g1 = xbox_ios_touch_layout_gen();
    CHECK(g1 != g0, "set_safe_area did not bump the generation");
    xbox_ios_touch_set_safe_area(0.1f, 0.05f);
    CHECK(xbox_ios_touch_layout_gen() == g1, "repeating set_safe_area bumped again");
    xbox_ios_touch_set_aspect(A43);
    g0 = xbox_ios_touch_layout_gen();
    CHECK(g0 != g1, "set_aspect(4:3) did not bump the generation");
    xbox_ios_touch_set_aspect(A43);
    xbox_ios_touch_set_aspect(A43 + 0.00001f);
    CHECK(xbox_ios_touch_layout_gen() == g0, "repeating set_aspect bumped again");
    xbox_ios_touch_set_tablet_scale(0.8f);
    CHECK(xbox_ios_touch_layout_gen() != g0, "set_tablet_scale did not bump the generation");
    CHECK(xbox_ios_touch_zone_pressed(a), "a real change dropped the held finger");
    s = rd();
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255, "A not held after the adaptation changes");
    fup(1);
    reset();
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Environment defaults first, in children, before this process touches
     * the layer. */
    env_case("ENV   RECOMP_TOUCH=0: init reports off, state 0, panel can enable", "off");
    env_case("ENV   NFSU2_TOUCH_LAYOUT=drive selects the DRIVE zones", "drive");
    env_case("ENV   NFSU2_TOUCH_SCALE=9 clamps to 1.5", "scale");
    env_case("ENV   default DRIVE: analog pedals and the strip are on", "analog1");
    env_case("ENV   NFSU2_ANALOG_PEDALS=0: DRIVE pedals digital 255", "analog0");
    env_case("ENV   NFSU2_STEER=0: no strip in DRIVE", "steer0");
    env_case("ENV   NFSU2_STEER_SENS=9 clamps to 2 (relative travel)", "sens2");
    env_case("ENV   NFSU2_STEER_MODE=absolute + SENS=9: old strip gain", "sens2abs");
    env_case("ENV   default steer mode is relative, curve 1.35", "mode_default");
    env_case("ENV   NFSU2_STEER_MODE=sideways falls back to relative", "mode_junk");
    env_case("ENV   NFSU2_STEER_MODE=ABSOLUTE (any case); panel can switch", "mode_abs");
    env_case("ENV   NFSU2_STEER_CURVE=9 clamps to 2.0; panel can change", "curve9");
    env_case("ENV   NFSU2_STEER_CURVE=abc clamps to 1.0", "curve_bad");
    env_case("ENV   NFSU2_DRIVE_CLASSIC=1: the old DRIVE table", "classic");

    unsetenv("RECOMP_TOUCH");
    unsetenv("NFSU2_TOUCH_LAYOUT");
    unsetenv("NFSU2_TOUCH_SCALE");
    unsetenv("NFSU2_ANALOG_PEDALS");
    unsetenv("NFSU2_STEER");
    unsetenv("NFSU2_STEER_SENS");
    unsetenv("NFSU2_STEER_MODE");
    unsetenv("NFSU2_STEER_CURVE");
    unsetenv("NFSU2_DRIVE_CLASSIC");

    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "touch_test: SDL_Init failed: %s\n", SDL_GetError());
        return 99;
    }
    /* The way xinput_device.c does it: init on the first poll. */
    if (xbox_ios_touch_init() != 1) {
        fprintf(stderr, "touch_test: xbox_ios_touch_init() did not report a live layer\n");
        return 98;
    }
    reset();

    run_case("STATE packet number and zeroing contract",           t_state_contract, NULL);
    run_case(" 1   PAD: each analog zone centre presses its button", t_pad_analog, NULL);
    run_case(" 1b  PAD: a held zone stays latched when the finger drifts", t_zone_latched, NULL);
    run_case(" 1c  PAD: two zones at once lift independently",     t_pad_multitouch, NULL);
    run_case(" 2   BACK / START set their wButtons flag",           t_flags, NULL);
    run_case(" 3   D-pad: cardinals, diagonals, neutral centre",    t_dpad, NULL);
    run_case(" 4a  Left stick: directions, deadzone, scale, clamp", t_left_stick, NULL);
    run_case(" 4b  Right stick: directions, independent of left",   t_right_stick, NULL);
    run_case(" 4c  Stick travel follows the window aspect",         t_stick_aspect, NULL);
    run_case(" 5   Stick handover to a waiting finger",             t_stick_handover, NULL);
    run_case(" 5b  Stray up / move events are harmless",            t_up_unknown_finger, NULL);
    run_case(" 6   Touch off: state 0, fingers dropped and ignored", t_touch_off, NULL);
    run_case(" 6b  Touch off: no press hook, tilt cleared",         t_touch_off_side_effects, NULL);
    run_case(" 7   DRIVE layout: GAS = RT, BRAKE = LT",             t_layout_drive, NULL);
    run_case(" 7b  Layout / scale switch clears held fingers",      t_layout_clears_fingers, NULL);
    run_case(" 8   Scale: zones grow/shrink about their centres, clamped", t_scale, NULL);
    run_case(" 9   Tilt: drives left X, a left-stick finger overrides", t_tilt, NULL);
    run_case("10   Press hook: once per zone press, not for sticks", t_press_hook, NULL);
    run_case("11   init() is idempotent (no double watch)",          t_init_idempotent, NULL);
    run_case("12   Finger limit: five tracked, sixth ignored",      t_finger_limit, NULL);
    run_case("13   Focus loss / background clear all fingers",      t_lifecycle, NULL);
    run_case("14   Overlay queries: stick(), zone_label(), zone_pressed()", t_overlay_queries, NULL);
    run_case("15   zone_style(): faces, shoulders, triggers / pedals, menu", t_zone_style, NULL);
    run_case("15d  zone_caption() / zone_icon(): PAD, DRIVE, classic DRIVE, out of range", t_caption_icon, NULL);
    run_case("15b  dpad_bits(): arms held, matches the state, cleared on lift", t_dpad_bits, NULL);
    run_case("15c  layout_gen(): moves on rebuild only", t_layout_gen, NULL);
    run_case("16   Analog pedals: ~90 at the bottom, 255 from the 40% mark up, DRIVE only", t_pedal_analog, NULL);
    run_case("16b  Analog pedals switch: digital 255, live with a finger held", t_pedal_digital, NULL);
    run_case("16c  Two fingers on one pedal: the higher value", t_pedal_two_fingers, NULL);
    run_case("17   Steering strip: deflection, deadzone, clamp, Y 0, latch, release", t_steer_strip, NULL);
    run_case("17b  Steering strip sensitivity 0.5..2, live", t_steer_sens, NULL);
    run_case("17c  Steering strip: rectangle hit test, width fixed, on screen", t_steer_shape, NULL);
    run_case("17d  Steering strip off: zone gone, dynamic stick back", t_steer_disabled, NULL);
    run_case("17e  Strip outranks the dynamic stick; two strip fingers", t_steer_priority, NULL);
    run_case("17f  Tilt is suppressed while the strip (or a stick) steers", t_tilt_vs_strip, NULL);
    run_case("17g  Relative steering: touch-down = straight, +travel = full lock, curve, deadzone, latch, release", t_steer_rel, NULL);
    run_case("17h  Relative steering sensitivity: travel 0.075 / sens, clamped 0.5..2, live", t_steer_rel_sens, NULL);
    run_case("17i  Steering curve: 1.0 linear, 2.0 squared, clamps, absolute ignores it, live mode switch", t_steer_curve, NULL);
    run_case("17j  Relative steering ignores strip size / place; setters keep fingers; two strip fingers", t_steer_rel_indep, NULL);
    run_case("18   Activity hook: every tracked finger down, not moves / repeats / off", t_activity_hook, NULL);
    run_case("19   DRIVE geometry on 852x393: no overlap, off the island, sizes", t_drive_geometry, NULL);
    run_case("20   NFSU2_DRIVE_CLASSIC / set_drive_classic: the old table", t_drive_classic, NULL);
    run_case("21   Layout editor: set_geom moves the hit area (scale applied)", t_geom_move, NULL);
    run_case("21b  Layout editor: clamps, bad indices, NaN, strip limits", t_geom_clamp, NULL);
    run_case("21c  Layout editor: overrides are per layout; reset restores the preset", t_geom_layouts, NULL);
    run_case("21d  Layout editor: layout_gen bumps on change / reset / parse only", t_geom_gen, NULL);
    run_case("21e  Layout editor: format -> parse round trip, replace, small buffers", t_geom_roundtrip, NULL);
    run_case("21f  Layout editor: parse tolerates garbage, CRLF, NULL, empty", t_geom_parse, NULL);
    run_case("21g  Layout editor: on screen at 1.5x, strip width, classic DRIVE", t_geom_on_screen, NULL);
    run_case("22   Safe area: rigid shift per half, off / zero = today, clamps, order", t_safe_map, NULL);
    run_case("22b  Safe area: hit test and d-pad use the shifted table, strip gain kept, edge cases", t_safe_hit, NULL);
    run_case("22d  Safe area 59/852 each side at 852x393, Size 1.0: no overlaps, groups rigid (PAD, DRIVE, classic)", t_safe_overlap, NULL);
    run_case("22c  Safe area: editor per-zone shift round-trips, zone_geom stays base", t_safe_editor, NULL);
    run_case("23   Aspect: phone unchanged, 4:3 round in pixels, pedals / strip keep shape", t_aspect_round, NULL);
    run_case("23b  Tablet factor: on top of Size, strip height only, clamped", t_tablet_scale, NULL);
    run_case("23c  Adaptation setters: no-op leaves gen and fingers alone", t_adapt_noop, NULL);

    reset();
    SDL_Quit();
    printf("touch_test: %d passed, %d failed%s", g_pass, g_fail, "");
    if (g_known)
        printf(", %d known", g_known);
    printf("\n");
    return g_fail;
}
