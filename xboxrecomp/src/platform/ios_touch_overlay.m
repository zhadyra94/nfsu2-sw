/* ios_touch_overlay.m -- visible on-screen controls over the SDL window, and
 * the in-game settings menu (gear button).
 * Pure UIKit: a transparent view with one labeled ellipse per touch zone,
 * drawn from the same table the touch layer hit-tests (xinput_ios.c). The
 * overlay itself never takes touches except the gear button (hitTest:
 * returns nil for everything else, SDL keeps them). The settings panel is a
 * separate scroll view on the window, so the opacity setting does not fade
 * the menu itself. Every change goes through nfsu2_cfg_set (main.c), which
 * saves nfsu2_env.txt. */
#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

/* Mirror the XBOX_IOS_TOUCH_ZONE layout (xinput_ios.h) instead of including
 * it: that header pulls the Win32 vocabulary, and its `typedef int BOOL`
 * collides with ObjC's own BOOL. int/float pairs keep identical layout in
 * both worlds, so a parallel declaration is safe. */
typedef struct XBOX_IOS_TOUCH_ZONE_MIRROR {
    int   id;
    float cx, cy, rx, ry;
    int   kind, value;
} XBOX_IOS_TOUCH_ZONE_MIRROR;
extern const void *xbox_ios_touch_zones(int *count);
extern int xbox_ios_touch_stick(int side, float *bx, float *by, float *kx, float *ky);
extern int xbox_ios_touch_zone_pressed(int idx);
extern const char *xbox_ios_touch_zone_label(int idx);
extern void xbox_ios_touch_set_enabled(int on);
extern void xbox_ios_touch_set_layout(int drive);
extern void xbox_ios_touch_set_scale(float k);
extern void xbox_ios_touch_set_press_hook(void (*hook)(int zone_idx));
extern void xbox_ios_touch_set_tilt(float x, int active);
extern int nv2a_vk_fps_x10(void);

/* xinput_device.c: live switches (read there from cached atomics). */
extern void xbox_input_set_rumble(int on);
extern void xbox_input_set_keyboard(int on);

/* ios_keyboard.m */
extern void ios_keyboard_install(void);

/* Settings (Documents/nfsu2_env.txt, loaded and saved in main.c). */
extern float nfsu2_cfg_overlay_alpha(void);
extern int   nfsu2_cfg_show_fps(void);
extern const char *nfsu2_cfg_get(const char *key);
extern int   nfsu2_cfg_set(const char *key, const char *val);

@class Nfsu2GearTarget;
static Nfsu2GearTarget *s_target;
static BOOL s_user_touch_on = YES;   /* the panel's Touch controls switch */
static BOOL s_dev_hidden = NO;       /* a pad or keyboard is attached */
static void refresh_shapes(void);

@interface Nfsu2Overlay : UIView
- (void)applyVisibility:(BOOL)vis;
@property (nonatomic, assign) BOOL shapesHidden;   /* touch off, or pad/keyboard on */
@end

@implementation Nfsu2Overlay
- (id)initWithFrame:(CGRect)f
{
    self = [super initWithFrame:f];
    if (self) {
        self.backgroundColor = [UIColor clearColor];
        self.userInteractionEnabled = YES;   /* only the gear/panel eat touches */
        self.opaque = NO;
        self.contentMode = UIViewContentModeRedraw;
        self.alpha = nfsu2_cfg_overlay_alpha();
        self.shapesHidden = NO;
        self.tag = 0x4E463531;             /* 'NF51' */
    }
    return self;
}

/* Everything passes through to SDL except the gear and the settings panel. */
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e
{
    for (UIView *sub in self.subviews) {
        if (sub.userInteractionEnabled && sub.hidden == NO &&
            CGRectContainsPoint(sub.frame, p)) {
            CGPoint local = [self convertPoint:p toView:sub];
            return [sub hitTest:local withEvent:e];
        }
    }
    return nil;
}

- (void)drawRect:(CGRect)rect
{
    (void)rect;
    CGContextRef c = UIGraphicsGetCurrentContext();
    int n = 0;
    const XBOX_IOS_TOUCH_ZONE_MIRROR *z =
        (const XBOX_IOS_TOUCH_ZONE_MIRROR *)xbox_ios_touch_zones(&n);
    if (!c || !z || n <= 0)
        return;
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    if (self.shapesHidden)
        return;                 /* touch off / pad or keyboard on: gear+FPS stay */
    for (int i = 0; i < n; i++) {
        CGRect r = CGRectMake((z[i].cx - z[i].rx) * W,
                              (z[i].cy - z[i].ry) * H,
                              z[i].rx * 2 * W,
                              z[i].ry * 2 * H);
        int pressed = xbox_ios_touch_zone_pressed(i);
        CGContextSetRGBFillColor(c, 1, 1, 1, pressed ? 0.42 : 0.16);
        CGContextSetRGBStrokeColor(c, 1, 1, 1, pressed ? 1.0 : 0.85);
        CGContextSetLineWidth(c, 1.5);
        CGPathRef p = CGPathCreateWithEllipseInRect(r, NULL);
        CGContextAddPath(c, p);
        CGContextDrawPath(c, kCGPathFillStroke);
        CGPathRelease(p);
        const char *lbl = xbox_ios_touch_zone_label(i);
        if (lbl) {
            NSDictionary *attrs = @{
                NSFontAttributeName:
                    [UIFont boldSystemFontOfSize:H * 0.042f],
                NSForegroundColorAttributeName:
                    [UIColor colorWithWhite:1 alpha:0.95]
            };
            NSString *str = [NSString stringWithUTF8String:lbl];
            CGSize sz = [str sizeWithAttributes:attrs];
            [str drawAtPoint:CGPointMake(z[i].cx * W - sz.width / 2,
                                         z[i].cy * H - sz.height / 2)
              withAttributes:attrs];
        }
    }

    /* The sticks: dynamic (the touch-down point is the centre), so the
     * visual floats under the thumb - base ring at the down point, knob
     * at the finger, clamped to full deflection (0.12 of the width). */
    for (int side = 0; side < 2; side++) {
        float bx, by, kx, ky;
        if (!xbox_ios_touch_stick(side, &bx, &by, &kx, &ky))
            continue;
        CGFloat R = 0.12f * W;
        CGPoint base = CGPointMake(bx * W, by * H);
        CGPoint knob = CGPointMake(kx * W, ky * H);
        CGFloat vx = knob.x - base.x, vy = knob.y - base.y;
        CGFloat d = sqrt(vx * vx + vy * vy);
        if (d > R) {
            knob.x = base.x + vx / d * R;
            knob.y = base.y + vy / d * R;
        }
        CGRect ring = CGRectMake(base.x - R, base.y - R, 2 * R, 2 * R);
        CGContextSetRGBFillColor(c, 1, 1, 1, 0.10);
        CGContextSetRGBStrokeColor(c, 1, 1, 1, 0.55);
        CGContextSetLineWidth(c, 1.5);
        CGPathRef rp = CGPathCreateWithEllipseInRect(ring, NULL);
        CGContextAddPath(c, rp);
        CGContextDrawPath(c, kCGPathFillStroke);
        CGPathRelease(rp);
        CGFloat kr = R * 0.42f;
        CGRect knobR = CGRectMake(knob.x - kr, knob.y - kr, 2 * kr, 2 * kr);
        CGContextSetRGBFillColor(c, 1, 1, 1, 0.38);
        CGContextSetRGBStrokeColor(c, 1, 1, 1, 0.95);
        CGPathRef kp = CGPathCreateWithEllipseInRect(knobR, NULL);
        CGContextAddPath(c, kp);
        CGContextDrawPath(c, kCGPathFillStroke);
        CGPathRelease(kp);
    }
}

- (void)applyVisibility:(BOOL)vis
{
    /* Pad/keyboard presence: tracked apart from the panel's own switch, so a
     * pad leaving does not bring back controls the user turned off. */
    s_dev_hidden = vis ? NO : YES;
    refresh_shapes();
}
@end

/* ── FPS counter + gear + settings panel ────────────────────── */

static Nfsu2Overlay *s_ov;
static UILabel *s_fps;
static UIScrollView *s_panel;      /* on the window, above the overlay */
static UIWindow *s_win;

/* The shapes show when touch is on and no pad/keyboard has taken over. */
static void refresh_shapes(void)
{
    if (!s_ov)
        return;
    s_ov.shapesHidden = (!s_user_touch_on || s_dev_hidden) ? YES : NO;
    [s_ov setNeedsDisplay];
}

/* ── Haptics ── */

static UIImpactFeedbackGenerator *s_haptic_gen;
static int s_haptics_on = 1;          /* __atomic: panel (main) vs hook (any) */

static void haptic_fire_main(void)
{
    if (s_haptic_gen) {
        [s_haptic_gen impactOccurred];
    }
}

/* Runs on whichever thread pumps SDL events (often not main). */
static void touch_press_hook(int zone_idx)
{
    (void)zone_idx;
    if (!__atomic_load_n(&s_haptics_on, __ATOMIC_RELAXED))
        return;
    if ([NSThread isMainThread])
        haptic_fire_main();
    else
        dispatch_async(dispatch_get_main_queue(), ^{ haptic_fire_main(); });
}

/* ── Tilt steering ── */

static CMMotionManager *s_motion;
static float s_tilt_sens = 1.0f;
static int s_tilt_flip;               /* NFSU2_TILT_FLIP=1: swap the sign */

static void tilt_update(CMDeviceMotion *m)
{
    /* Held in landscape the phone is a wheel: turning it about the screen
     * normal moves gravity along the device's long axis (y). 30 degrees of
     * roll is full lock at sensitivity 1. */
    double gx = m.gravity.x, gy = m.gravity.y, sign, n, a;
    UIInterfaceOrientation o = s_win.windowScene.interfaceOrientation;

    if (o == UIInterfaceOrientationLandscapeLeft)
        sign = 1.0;
    else if (o == UIInterfaceOrientationLandscapeRight)
        sign = -1.0;
    else
        sign = gx > 0.0 ? 1.0 : -1.0;     /* no scene yet: go by gravity */
    if (s_tilt_flip)
        sign = -sign;
    gy = gy > 1.0 ? 1.0 : gy < -1.0 ? -1.0 : gy;
    n = sign * asin(gy) / 0.5235988 * (double)s_tilt_sens;   /* 30 degrees */
    a = fabs(n);
    if (a < 0.04) {
        n = 0.0;
    } else {
        a = (a - 0.04) / 0.96;           /* no jump at the edge of the deadzone */
        a = a > 1.0 ? 1.0 : a;
        n = n < 0.0 ? -a : a;
    }
    xbox_ios_touch_set_tilt((float)n, 1);
}

static void tilt_stop(void)
{
    if (s_motion && s_motion.deviceMotionActive)
        [s_motion stopDeviceMotionUpdates];
    xbox_ios_touch_set_tilt(0.0f, 0);
}

static void tilt_start(void)
{
    const char *f = nfsu2_cfg_get("NFSU2_TILT_FLIP");
    s_tilt_flip = (f && *f == '1');
    if (!s_motion)
        s_motion = [[CMMotionManager alloc] init];
    if (!s_motion.deviceMotionAvailable) {
        fprintf(stderr, "nfsu2: tilt: no device motion on this device\n");
        return;
    }
    if (s_motion.deviceMotionActive)
        return;
    s_motion.deviceMotionUpdateInterval = 1.0 / 60.0;
    [s_motion startDeviceMotionUpdatesToQueue:[NSOperationQueue mainQueue]
                                  withHandler:^(CMDeviceMotion *motion, NSError *error) {
        (void)error;
        if (motion)
            tilt_update(motion);
    }];
    fprintf(stderr, "nfsu2: tilt steering on\n");
}

/* ── Settings table ── */

enum { IK_HEAD, IK_SWITCH, IK_SLIDER, IK_SEG };
enum { ACT_NONE, ACT_FPS, ACT_TOUCH, ACT_LAYOUT, ACT_SCALE, ACT_ALPHA,
       ACT_HAPTICS, ACT_TILT, ACT_TILTSENS, ACT_KEYBOARD, ACT_RUMBLE };

typedef struct {
    int         kind;
    const char *title;
    const char *key;
    const char *on, *off;            /* switch: the values written */
    int         def_on;              /* switch: state when the key is unset */
    float       lo, hi, def;         /* slider */
    int         decimals;            /* slider: digits written and shown */
    const char *labels, *values;     /* segmented: '|' separated */
    const char *def_val;             /* segmented: value when unset */
    int         act;                 /* live effect, ACT_* */
} PanelItem;

static const PanelItem s_items[] = {
    { .kind = IK_HEAD, .title = "Controls" },
    { .kind = IK_SWITCH, .title = "Touch controls", .key = "RECOMP_TOUCH",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_TOUCH },
    { .kind = IK_SEG, .title = "Layout", .key = "NFSU2_TOUCH_LAYOUT",
      .labels = "Pad|Drive", .values = "pad|drive", .def_val = "pad",
      .act = ACT_LAYOUT },
    { .kind = IK_SLIDER, .title = "Size", .key = "NFSU2_TOUCH_SCALE",
      .lo = 0.7f, .hi = 1.5f, .def = 1.0f, .decimals = 2, .act = ACT_SCALE },
    { .kind = IK_SLIDER, .title = "Opacity", .key = "NFSU2_OVERLAY_ALPHA",
      .lo = 0.2f, .hi = 0.8f, .def = 0.55f, .decimals = 2, .act = ACT_ALPHA },
    { .kind = IK_SWITCH, .title = "Haptics", .key = "NFSU2_HAPTICS",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_HAPTICS },
    { .kind = IK_SWITCH, .title = "Tilt steering", .key = "NFSU2_TILT",
      .on = "1", .off = "0", .def_on = 0, .act = ACT_TILT },
    { .kind = IK_SLIDER, .title = "Tilt sensitivity", .key = "NFSU2_TILT_SENS",
      .lo = 0.5f, .hi = 2.0f, .def = 1.0f, .decimals = 2, .act = ACT_TILTSENS },
    { .kind = IK_SWITCH, .title = "Keyboard", .key = "NFSU2_KEYBOARD",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_KEYBOARD },
    { .kind = IK_SWITCH, .title = "Rumble", .key = "RECOMP_RUMBLE",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_RUMBLE },
    { .kind = IK_SWITCH, .title = "Show FPS", .key = "NFSU2_SHOW_FPS",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_FPS },

    { .kind = IK_HEAD, .title = "Display — applies on next launch" },
    { .kind = IK_SEG, .title = "Render scale", .key = "RECOMP_GL_SCALE",
      .labels = "1x|1.5x|2x|3x", .values = "1|1.5|2|3", .def_val = "2" },
    { .kind = IK_SWITCH, .title = "Widescreen 16:9", .key = "RECOMP_WIDESCREEN",
      .on = "1", .off = "0", .def_on = 1 },
    { .kind = IK_SWITCH, .title = "60 fps races (experimental)",
      .key = "RECOMP_PRESENT_INTERVAL", .on = "1", .off = "0", .def_on = 0 },
    { .kind = IK_SLIDER, .title = "Brightness", .key = "NFSU2_GAMMA",
      .lo = 0.5f, .hi = 2.0f, .def = 1.0f, .decimals = 2 },
    { .kind = IK_SWITCH, .title = "Car reflections", .key = "RECOMP_VK_CUBE",
      .on = "1", .off = "0", .def_on = 1 },
    { .kind = IK_SWITCH, .title = "Fill screen with movies", .key = "NFSU2_MOVIE_CROP",
      .on = "1", .off = "0", .def_on = 1 },

    { .kind = IK_HEAD, .title = "Audio — applies on next launch" },
    { .kind = IK_SLIDER, .title = "Volume", .key = "RECOMP_AUDIO_VOLUME",
      .lo = 0.0f, .hi = 100.0f, .def = 100.0f, .decimals = 0 },
    /* on = "" leaves SDL's default category (Playback + MixWithOthers). */
    { .kind = IK_SWITCH, .title = "Mix with other apps' audio", .key = "SDL_AUDIO_CATEGORY",
      .on = "", .off = "playback", .def_on = 1 },

    { .kind = IK_HEAD, .title = "Advanced — applies on next launch" },
    { .kind = IK_SWITCH, .title = "Async GPU submits",
      .key = "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS",
      .on = "0", .off = "1", .def_on = 1 },
    { .kind = IK_SWITCH, .title = "Frame lag (game and GPU overlap)",
      .key = "RECOMP_FRAME_LAG", .on = "1", .off = "0", .def_on = 1 },
    { .kind = IK_SWITCH, .title = "Eager guest lock",
      .key = "RECOMP_GIL_EAGER", .on = "1", .off = "0", .def_on = 0 },
    { .kind = IK_SWITCH, .title = "Verbose log",
      .key = "RECOMP_QUIET", .on = "0", .off = "1", .def_on = 0 },
    { .kind = IK_SWITCH, .title = "Background drain handshake",
      .key = "NFSU2_BG_HANDSHAKE", .on = "1", .off = "0", .def_on = 1 },
    { .kind = IK_SWITCH, .title = "Swapchain rebuild limiter",
      .key = "RECOMP_VK_SC_LIMIT", .on = "1", .off = "0", .def_on = 1 },
    { .kind = IK_SWITCH, .title = "High-priority GPU queue",
      .key = "NFSU2_VK_QUEUE_HIGH", .on = "1", .off = "0", .def_on = 1 },
    /* nv2a_vk.c: a pass that opens with a full clear starts with
     * LOAD_OP_CLEAR (no attachment reload on a tile GPU); iOS default on. */
    { .kind = IK_SWITCH, .title = "Clear passes on load",
      .key = "RECOMP_VK_CLEAR_LOAD", .on = "1", .off = "0", .def_on = 1 },
    /* nv2a_vk.c: texture uploads recorded before the frame instead of
     * splitting render passes; default off until measured on device. */
    { .kind = IK_SWITCH, .title = "Hoist texture uploads",
      .key = "RECOMP_VK_HOIST", .on = "1", .off = "0", .def_on = 0 },
};
#define ITEM_COUNT ((int)(sizeof s_items / sizeof s_items[0]))

/* Switch state from the stored string; an odd value falls back on its first
 * character (0/n/f = off), then on the default. */
static int item_is_on(const PanelItem *it)
{
    const char *v = nfsu2_cfg_get(it->key);
    if (!v)
        return it->def_on;
    if (strcmp(v, it->on) == 0)
        return 1;
    if (strcmp(v, it->off) == 0)
        return 0;
    if (*v == '0' || *v == 'n' || *v == 'N' || *v == 'f' || *v == 'F')
        return 0;
    if (*v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T')
        return 1;
    return it->def_on;
}

static float item_slider_value(const PanelItem *it)
{
    const char *v = nfsu2_cfg_get(it->key);
    float f = (v && *v) ? (float)atof(v) : it->def;
    return f < it->lo ? it->lo : f > it->hi ? it->hi : f;
}

/* Apply a setting's live effect. on / fv / sv carry the new value. */
static void apply_live(int act, int on, float fv, const char *sv)
{
    switch (act) {
    case ACT_FPS:
        s_fps.hidden = on ? NO : YES;
        break;
    case ACT_TOUCH:
        xbox_ios_touch_set_enabled(on);
        s_user_touch_on = on ? YES : NO;
        refresh_shapes();
        break;
    case ACT_LAYOUT:
        xbox_ios_touch_set_layout(sv && strcasecmp(sv, "drive") == 0);
        [s_ov setNeedsDisplay];
        break;
    case ACT_SCALE:
        xbox_ios_touch_set_scale(fv);
        [s_ov setNeedsDisplay];
        break;
    case ACT_ALPHA:
        s_ov.alpha = fv < 0.2f ? 0.2f : fv > 0.8f ? 0.8f : fv;
        break;
    case ACT_HAPTICS:
        __atomic_store_n(&s_haptics_on, on ? 1 : 0, __ATOMIC_RELAXED);
        break;
    case ACT_TILT:
        if (on)
            tilt_start();
        else
            tilt_stop();
        break;
    case ACT_TILTSENS:
        s_tilt_sens = fv;
        break;
    case ACT_KEYBOARD:
        xbox_input_set_keyboard(on);
        break;
    case ACT_RUMBLE:
        xbox_input_set_rumble(on);
        break;
    default:
        break;
    }
}

/* ── Panel building ── */

static UILabel *panel_label(NSString *text, CGRect frame, CGFloat size,
                            UIColor *color, BOOL bold)
{
    UILabel *l = [[UILabel alloc] initWithFrame:frame];
    l.text = text;
    l.textColor = color;
    l.font = bold ? [UIFont boldSystemFontOfSize:size] : [UIFont systemFontOfSize:size];
    return l;
}

static NSString *ns(const char *c)
{
    return [NSString stringWithUTF8String:c ? c : ""];
}

static void format_slider(const PanelItem *it, float v, char *buf, size_t n)
{
    snprintf(buf, n, "%.*f", it->decimals, (double)v);
}

/* Adds one row for s_items[idx] at y; returns the y below it. */
static CGFloat panel_add_item(int idx, CGFloat y, CGFloat w)
{
    const PanelItem *it = &s_items[idx];
    UIColor *white = UIColor.whiteColor;

    if (it->kind == IK_HEAD) {
        y += 8;
        [s_panel addSubview:panel_label(ns(it->title), CGRectMake(12, y, w - 24, 20), 12,
                                        [UIColor colorWithRed:0.55 green:0.75 blue:1.0 alpha:1.0],
                                        YES)];
        return y + 24;
    }
    if (it->kind == IK_SWITCH) {
        [s_panel addSubview:panel_label(ns(it->title), CGRectMake(12, y + 6, w - 24 - 59, 24), 13,
                                        white, NO)];
        UISwitch *sw = [[UISwitch alloc] initWithFrame:CGRectMake(w - 12 - 51, y + 2, 51, 31)];
        sw.on = item_is_on(it) ? YES : NO;
        sw.tag = idx + 1;
        [sw addTarget:s_target action:@selector(switchChanged:)
                    forControlEvents:UIControlEventValueChanged];
        [s_panel addSubview:sw];
        return y + 36;
    }
    if (it->kind == IK_SLIDER) {
        char buf[24];
        float v = item_slider_value(it);
        format_slider(it, v, buf, sizeof buf);
        [s_panel addSubview:panel_label(ns(it->title), CGRectMake(12, y + 2, w - 24 - 70, 20), 13,
                                        white, NO)];
        UILabel *vl = panel_label(ns(buf), CGRectMake(w - 12 - 70, y + 2, 70, 20), 13,
                                  [UIColor lightGrayColor], NO);
        vl.textAlignment = NSTextAlignmentRight;
        vl.tag = 1000 + idx;
        [s_panel addSubview:vl];
        UISlider *sl = [[UISlider alloc] initWithFrame:CGRectMake(12, y + 22, w - 24, 28)];
        sl.minimumValue = it->lo;
        sl.maximumValue = it->hi;
        sl.value = v;
        sl.continuous = YES;
        sl.tag = idx + 1;
        [sl addTarget:s_target action:@selector(sliderMoved:)
                  forControlEvents:UIControlEventValueChanged];
        /* Save and apply on release: one file write per drag, not per pixel. */
        [sl addTarget:s_target action:@selector(sliderDone:)
                  forControlEvents:(UIControlEventTouchUpInside |
                                    UIControlEventTouchUpOutside |
                                    UIControlEventTouchCancel)];
        [s_panel addSubview:sl];
        return y + 54;
    }
    /* IK_SEG */
    {
        NSArray *labels = [ns(it->labels) componentsSeparatedByString:@"|"];
        NSArray *vals = [ns(it->values) componentsSeparatedByString:@"|"];
        NSString *cur = ns(nfsu2_cfg_get(it->key));
        NSString *def = ns(it->def_val);
        NSInteger sel = -1, defsel = 0;
        NSUInteger k;

        if (cur.length == 0)
            cur = def;
        for (k = 0; k < vals.count; k++) {
            NSString *vk = [vals objectAtIndex:k];
            if ([vk caseInsensitiveCompare:cur] == NSOrderedSame)
                sel = (NSInteger)k;
            if ([vk caseInsensitiveCompare:def] == NSOrderedSame)
                defsel = (NSInteger)k;
        }
        if (sel < 0 && [cur doubleValue] > 0.0) {   /* "2.0" for "2" */
            for (k = 0; k < vals.count; k++)
                if (fabs([[vals objectAtIndex:k] doubleValue] - [cur doubleValue]) < 1e-6)
                    sel = (NSInteger)k;
        }
        if (sel < 0)
            sel = defsel;
        [s_panel addSubview:panel_label(ns(it->title), CGRectMake(12, y + 2, w - 24, 20), 13,
                                        white, NO)];
        UISegmentedControl *sg = [[UISegmentedControl alloc] initWithItems:labels];
        sg.frame = CGRectMake(12, y + 24, w - 24, 30);
        sg.selectedSegmentIndex = sel;
        sg.tag = idx + 1;
        [sg addTarget:s_target action:@selector(segChanged:)
                  forControlEvents:UIControlEventValueChanged];
        [s_panel addSubview:sg];
        return y + 60;
    }
}

/* Built again at every open: the controls always show what is stored. */
static void panel_rebuild(CGFloat w)
{
    CGFloat y = 8;
    NSArray *old = [s_panel.subviews copy];
    for (UIView *v in old)
        [v removeFromSuperview];

    [s_panel addSubview:panel_label(@"Settings", CGRectMake(12, y, w - 110, 28), 17,
                                    UIColor.whiteColor, YES)];
    UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
    done.frame = CGRectMake(w - 12 - 80, y, 80, 28);
    [done setTitle:@"Done" forState:UIControlStateNormal];
    done.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    [done addTarget:s_target action:@selector(doneTapped)
        forControlEvents:UIControlEventTouchUpInside];
    [s_panel addSubview:done];
    y += 34;

    for (int i = 0; i < ITEM_COUNT; i++)
        y = panel_add_item(i, y, w);

    y += 8;
    UILabel *foot = panel_label(
        @"Items under 'applies on next launch' take effect after you close and reopen the app.\n\n"
        @"Keyboard: arrows = D-pad, W gas, S brake, A/D steer, "
        @"Return/Space = A, Esc/Backspace = B, Shift = X, Ctrl = Y, Q/E = LB/RB, "
        @"Tab/P = Start, O or ` = Back, I/J/K/L = right stick.",
        CGRectMake(12, y, w - 24, 10), 11, [UIColor lightGrayColor], NO);
    foot.numberOfLines = 0;
    CGSize fs = [foot sizeThatFits:CGSizeMake(w - 24, CGFLOAT_MAX)];
    foot.frame = CGRectMake(12, y, w - 24, fs.height);
    [s_panel addSubview:foot];
    y += fs.height + 14;
    s_panel.contentSize = CGSizeMake(w, y);
    s_panel.contentOffset = CGPointZero;
}

/* Size and place the panel inside the safe area, below the gear, then fill it. */
static void panel_show(void)
{
    CGRect wb = s_win.bounds;
    UIEdgeInsets sa = s_win.safeAreaInsets;
    CGFloat pw = floor(wb.size.width * 0.7);
    CGFloat top = sa.top + 52;
    CGFloat maxh = wb.size.height * 0.85;
    CGFloat room = wb.size.height - top - sa.bottom - 8;
    CGFloat ph;

    panel_rebuild(pw);                  /* sets contentSize */
    ph = maxh < room ? maxh : room;
    if (s_panel.contentSize.height < ph)
        ph = s_panel.contentSize.height;
    s_panel.frame = CGRectMake(floor((wb.size.width - pw) / 2), top, pw, ph);
    [s_win bringSubviewToFront:s_panel];
    s_panel.hidden = NO;
}

static void panel_hide(void)
{
    s_panel.hidden = YES;
}

/* UIControl targets: NSObject categories can't reach our statics, so route
 * through a small helper object. */
@interface Nfsu2GearTarget : NSObject
- (void)fpsTick;
@end
@implementation Nfsu2GearTarget
- (void)switchChanged:(UISwitch *)sw
{
    int idx = (int)sw.tag - 1;
    if (idx < 0 || idx >= ITEM_COUNT)
        return;
    const PanelItem *it = &s_items[idx];
    int on = sw.on ? 1 : 0;
    nfsu2_cfg_set(it->key, on ? it->on : it->off);
    apply_live(it->act, on, 0.0f, NULL);
}

- (void)sliderMoved:(UISlider *)sl
{
    int idx = (int)sl.tag - 1;
    if (idx < 0 || idx >= ITEM_COUNT)
        return;
    const PanelItem *it = &s_items[idx];
    char buf[24];
    format_slider(it, sl.value, buf, sizeof buf);
    UILabel *vl = (UILabel *)[s_panel viewWithTag:1000 + idx];
    if ([vl isKindOfClass:[UILabel class]])
        vl.text = ns(buf);
    if (it->act == ACT_ALPHA)    /* cheap, so it follows the thumb */
        apply_live(ACT_ALPHA, 1, (float)atof(buf), NULL);
}

- (void)sliderDone:(UISlider *)sl
{
    int idx = (int)sl.tag - 1;
    if (idx < 0 || idx >= ITEM_COUNT)
        return;
    const PanelItem *it = &s_items[idx];
    char buf[24];
    format_slider(it, sl.value, buf, sizeof buf);
    nfsu2_cfg_set(it->key, buf);
    apply_live(it->act, 1, (float)atof(buf), NULL);
}

- (void)segChanged:(UISegmentedControl *)sg
{
    int idx = (int)sg.tag - 1;
    if (idx < 0 || idx >= ITEM_COUNT)
        return;
    const PanelItem *it = &s_items[idx];
    NSArray *vals = [ns(it->values) componentsSeparatedByString:@"|"];
    NSInteger k = sg.selectedSegmentIndex;
    if (k < 0 || k >= (NSInteger)vals.count)
        return;
    NSString *v = [vals objectAtIndex:(NSUInteger)k];
    nfsu2_cfg_set(it->key, [v UTF8String]);
    apply_live(it->act, 1, (float)[v doubleValue], [v UTF8String]);
}

- (void)doneTapped
{
    panel_hide();
}

- (void)gearTapped
{
    if (s_panel.hidden)
        panel_show();
    else
        panel_hide();
}

- (void)stickTick
{
    /* Poll the touch state at 30 fps: redraw only when something the
     * overlay draws actually changed (a stick moved or a zone pressed).
     * Idle frames cost two lock-protected scans - nothing. */
    static int s_sig;
    int sig = 0;
    float bx, by, kx, ky;
    if (xbox_ios_touch_stick(0, &bx, &by, &kx, &ky))
        sig |= 1 | (int)((kx * 96.0f) + 96) | ((int)((ky * 96.0f) + 96) << 8);
    if (xbox_ios_touch_stick(1, &bx, &by, &kx, &ky))
        sig |= 2 | ((int)((kx * 96.0f) + 96) << 16) | ((int)((ky * 96.0f) + 96) << 24);
    int n = 0;
    const void *zp = xbox_ios_touch_zones(&n);
    (void)zp;
    for (int i = 0; i < n; i++)
        if (xbox_ios_touch_zone_pressed(i))
            sig |= 0x10000 << (i % 15);
    if (sig != s_sig) {
        s_sig = sig;
        [s_ov setNeedsDisplay];
    }
}

- (void)fpsTick
{
    /* The thermal state is logged even with the FPS label hidden. */
    static NSInteger s_last_thermal = -1;
    NSInteger ts = (NSInteger)[NSProcessInfo processInfo].thermalState;
    if (ts != s_last_thermal) {
        s_last_thermal = ts;
        fprintf(stderr, "nfsu2: thermal state %ld\n", (long)ts);
    }
    if (!s_fps || s_fps.hidden)
        return;
    int x10 = nv2a_vk_fps_x10();
    if (ts >= (NSInteger)NSProcessInfoThermalStateSerious)
        s_fps.text = [NSString stringWithFormat:@"%d.%d fps · HOT", x10 / 10, x10 % 10];
    else
        s_fps.text = [NSString stringWithFormat:@"%d.%d fps", x10 / 10, x10 % 10];
}
@end

static UIEdgeInsets s_safe;

static CGFloat sa2left(void)  { return s_safe.left; }
static CGFloat sa2top(void)   { return s_safe.top;  }

static void build_fps_and_gear(void)
{
    s_fps = [[UILabel alloc] initWithFrame:CGRectMake(sa2left() + 10, sa2top() + 8, 170, 24)];
    s_fps.textColor = [UIColor colorWithWhite:1 alpha:0.9];
    s_fps.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightMedium];
    s_fps.hidden = nfsu2_cfg_show_fps() ? NO : YES;
    [s_ov addSubview:s_fps];

    [NSTimer scheduledTimerWithTimeInterval:0.5
                                     target:s_target
                                   selector:@selector(fpsTick)
                                   userInfo:nil
                                    repeats:YES];

    CADisplayLink *dl = [CADisplayLink displayLinkWithTarget:s_target
                                                    selector:@selector(stickTick)];
    dl.preferredFramesPerSecond = 30;    /* touch visuals do not need 120 */
    [dl addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

/* Called from main.c (iOS) after the renderer init. uiwindow is the SDL
 * UIWindow obtained via SDL_GetWindowWMInfo. */
void ios_touch_overlay_install(void *uiwindow)
{
    UIWindow *win = (__bridge UIWindow *)uiwindow;
    if (!win)
        return;
    for (UIView *sub in win.subviews)
        if ([sub isKindOfClass:[Nfsu2Overlay class]]) {
            [sub removeFromSuperview];
            break;
        }
    if (s_panel)
        [s_panel removeFromSuperview];
    s_win = win;
    s_target = [[Nfsu2GearTarget alloc] init];
    Nfsu2Overlay *ov = [[Nfsu2Overlay alloc] initWithFrame:win.bounds];
    ov.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [win addSubview:ov];
    [win bringSubviewToFront:ov];
    s_ov = ov;

    s_safe = win.safeAreaInsets;
    UIEdgeInsets sa = s_safe;
    UIButton *gear = [UIButton buttonWithType:UIButtonTypeSystem];
    gear.frame = CGRectMake(win.bounds.size.width / 2 - 22, sa.top + 4, 44, 44);
    UIImage *gi = [UIImage systemImageNamed:@"gearshape.fill"
                          withConfiguration:[UIImageSymbolConfiguration
                              configurationWithPointSize:20
                                                  weight:UIImageSymbolWeightMedium]];
    if (gi) {
        [gear setImage:gi forState:UIControlStateNormal];
    } else {
        [gear setTitle:@"..." forState:UIControlStateNormal];
        gear.titleLabel.font = [UIFont boldSystemFontOfSize:18];
    }
    gear.tintColor = [UIColor colorWithWhite:1 alpha:0.8];
    [gear addTarget:s_target action:@selector(gearTapped)
   forControlEvents:UIControlEventTouchUpInside];
    [ov addSubview:gear];

    /* The panel lives on the window, not in the overlay: the overlay's alpha
     * (the Opacity setting) must not fade the menu. Built when opened. */
    s_panel = [[UIScrollView alloc] initWithFrame:CGRectZero];
    s_panel.backgroundColor = [UIColor colorWithWhite:0.1 alpha:0.92];
    s_panel.layer.cornerRadius = 10;
    s_panel.clipsToBounds = YES;
    s_panel.hidden = YES;
    s_panel.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    [win addSubview:s_panel];

    build_fps_and_gear();

    /* Live settings from their stored values. */
    {
        const char *v;
        s_user_touch_on = item_is_on(&s_items[1]) ? YES : NO;   /* Touch controls */
        refresh_shapes();
        v = nfsu2_cfg_get("NFSU2_TILT_SENS");
        if (v && *v) {
            float f = (float)atof(v);
            s_tilt_sens = f < 0.5f ? 0.5f : f > 2.0f ? 2.0f : f;
        }
        v = nfsu2_cfg_get("NFSU2_HAPTICS");
        __atomic_store_n(&s_haptics_on, !(v && *v == '0'), __ATOMIC_RELAXED);
        s_haptic_gen = [[UIImpactFeedbackGenerator alloc]
                           initWithStyle:UIImpactFeedbackStyleLight];
        [s_haptic_gen prepare];
        xbox_ios_touch_set_press_hook(touch_press_hook);
        v = nfsu2_cfg_get("NFSU2_TILT");
        if (v && *v == '1')
            tilt_start();
    }
    ios_keyboard_install();

    [ov setNeedsDisplay];
    fprintf(stderr, "nfsu2: overlay frame %g x %g subviews %lu\n",
            (double)ov.frame.size.width, (double)ov.frame.size.height,
            (unsigned long)ov.subviews.count);
}

/* Pad/keyboard connect/disconnect. Callable from any thread. */
void ios_touch_overlay_set_visible(int v)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s_ov)
            [s_ov applyVisibility:v ? YES : NO];
    });
}
