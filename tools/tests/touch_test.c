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

/* Back to a known state: touch on, PAD layout, scale 1, no tilt, no hook,
 * no fingers, 16:9 window. The setters clear all fingers by contract; the
 * explicit lift keeps reset() sound even if that contract breaks. */
static void reset(void)
{
    xbox_ios_touch_set_press_hook(NULL);
    xbox_ios_touch_set_enabled(1);
    xbox_ios_touch_set_layout(0);
    xbox_ios_touch_set_scale(1.0f);
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

/* The test's own ellipse test, same convention as the layer: inclusive. */
static int zone_hit(float x, float y)
{
    int n = 0, i;
    const XBOX_IOS_TOUCH_ZONE *z = xbox_ios_touch_zones(&n);
    for (i = 0; i < n; i++) {
        float dx = (x - z[i].cx) / z[i].rx;
        float dy = (y - z[i].cy) / z[i].ry;
        if (dx * dx + dy * dy <= 1.0f)
            return i;
    }
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

/* 7. Layout: DRIVE has GAS / BRAKE; switching clears held fingers. */
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

    /* The face buttons, shoulders, BACK/START and the d-pad survive in DRIVE. */
    {
        size_t k;
        for (k = 0; k < sizeof k_pad_buttons / sizeof k_pad_buttons[0]; k++) {
            const char *l = k_pad_buttons[k].label;
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
        zone_by_flag(XBOX_GAMEPAD_BACK);
        zone_by_flag(XBOX_GAMEPAD_START);
        zone_by_kind(XBOX_IOS_ZONE_DPAD);
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
        CHECK(xbox_ios_touch_zone_style(zone_by_label("A")) == XBOX_IOS_STYLE_FACE_A,
              "DRIVE A style");
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

int main(void)
{
    /* Environment defaults first, in children, before this process touches
     * the layer. */
    env_case("ENV   RECOMP_TOUCH=0: init reports off, state 0, panel can enable", "off");
    env_case("ENV   NFSU2_TOUCH_LAYOUT=drive selects the DRIVE zones", "drive");
    env_case("ENV   NFSU2_TOUCH_SCALE=9 clamps to 1.5", "scale");

    unsetenv("RECOMP_TOUCH");
    unsetenv("NFSU2_TOUCH_LAYOUT");
    unsetenv("NFSU2_TOUCH_SCALE");

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
    run_case("15b  dpad_bits(): arms held, matches the state, cleared on lift", t_dpad_bits, NULL);
    run_case("15c  layout_gen(): moves on rebuild only", t_layout_gen, NULL);

    reset();
    SDL_Quit();
    printf("touch_test: %d passed, %d failed%s", g_pass, g_fail, "");
    if (g_known)
        printf(", %d known", g_known);
    printf("\n");
    return g_fail;
}
