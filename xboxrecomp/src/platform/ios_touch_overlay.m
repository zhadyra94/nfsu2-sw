/* ios_touch_overlay.m -- visible on-screen controls over the SDL window, and
 * the in-game settings menu (gear button).
 * Pure UIKit/QuartzCore: a transparent view whose layer holds one small CALayer
 * subtree per touch zone (built from the same table the touch layer
 * hit-tests, xinput_ios.c; the DRIVE steering strip has a knob, the pedals an
 * analog fill) plus two floating stick layers. Nothing is drawn
 * with drawRect: layers are rebuilt when the zone table or the bounds change
 * and updated by index when a pressed state changes. The overlay itself
 * never takes touches except the gear button (hitTest: returns nil for
 * everything else, SDL keeps them). The settings panel is a
 * separate scroll view on the window, so the opacity setting does not fade
 * the menu itself. Every change goes through nfsu2_cfg_set (main.c), which
 * saves nfsu2_env.txt. */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
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
extern float xbox_ios_touch_zone_value(int idx);     /* pedal level 0..1, strip steer -1..1 */
extern const char *xbox_ios_touch_zone_label(int idx);
extern int xbox_ios_touch_zone_style(int idx);       /* ZS_* below */
extern unsigned xbox_ios_touch_dpad_bits(void);      /* up 1, down 2, left 4, right 8 */
extern unsigned xbox_ios_touch_layout_gen(void);     /* bumped on every zone-table rebuild */
extern void xbox_ios_touch_set_enabled(int on);
extern void xbox_ios_touch_set_layout(int drive);
extern void xbox_ios_touch_set_scale(float k);
extern void xbox_ios_touch_set_press_hook(void (*hook)(int zone_idx));
extern void xbox_ios_touch_set_activity_hook(void (*hook)(void));   /* any finger down */
extern void xbox_ios_touch_set_analog_pedals(int on);
extern void xbox_ios_touch_set_steer(int on);
extern void xbox_ios_touch_set_steer_sens(float k);
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
static void overlay_wake(void);

/* ── Look: colours, text and icon images, path helpers ─────────── */

/* Style numbers: a plain-int mirror of XBOX_IOS_STYLE_* in xinput_ios.h
 * (that header cannot be included here, see above). Keep the two in step. */
enum {
    ZS_FACE_A = 0, ZS_FACE_B, ZS_FACE_X, ZS_FACE_Y,
    ZS_SHOULDER_BLACK, ZS_SHOULDER_WHITE, ZS_TRIGGER_L, ZS_TRIGGER_R,
    ZS_PEDAL_GAS, ZS_PEDAL_BRAKE, ZS_DPAD, ZS_MENU_BACK, ZS_MENU_START,
    ZS_STEER, ZS_OTHER
};

static CGFloat s_cscale = 2.0;       /* screen scale, refreshed at every rebuild */

static UIColor *rgba(unsigned hex, CGFloat a)
{
    return [UIColor colorWithRed:((hex >> 16) & 0xFF) / 255.0
                           green:((hex >> 8) & 0xFF) / 255.0
                            blue:(hex & 0xFF) / 255.0
                           alpha:a];
}

/* CGColorRef/CGPathRef are read from a parameter that the caller keeps alive
 * for the whole call, then handed to a layer property that retains/copies. */
static void set_bg(CALayer *l, UIColor *c)
{
    CGColorRef cg = c.CGColor;
    l.backgroundColor = cg;
}

static void set_stroke(CAShapeLayer *l, UIColor *c)
{
    CGColorRef cg = c.CGColor;
    l.strokeColor = cg;
}

static void set_fill(CAShapeLayer *l, UIColor *c)
{
    CGColorRef cg = c.CGColor;
    l.fillColor = cg;
}

static void set_path(CAShapeLayer *l, UIBezierPath *p)
{
    CGPathRef cg = p.CGPath;
    l.path = cg;
}

static void set_shadow(CALayer *l, UIBezierPath *p, UIColor *c)
{
    CGPathRef sp = p.CGPath;
    CGColorRef sc = c.CGColor;
    l.shadowPath = sp;
    l.shadowColor = sc;
}

static void set_border(CALayer *l, UIColor *c, CGFloat width)
{
    CGColorRef bc = c.CGColor;
    l.borderColor = bc;
    l.borderWidth = width;
}

static void set_gradient(CAGradientLayer *g, UIColor *top, UIColor *bot)
{
    CGColorRef a = top.CGColor;
    CGColorRef b = bot.CGColor;
    g.colors = [NSArray arrayWithObjects:(__bridge id)a, (__bridge id)b, nil];
    g.startPoint = CGPointMake(0.5, 0.0);
    g.endPoint = CGPointMake(0.5, 1.0);
}

static UIGraphicsImageRendererFormat *img_format(void)
{
    UIGraphicsImageRendererFormat *f = [UIGraphicsImageRendererFormat preferredFormat];
    f.opaque = NO;
    f.scale = s_cscale;
    return f;
}

/* Bold system-font text with a soft dark shadow, rendered once into a bitmap
 * (a label never costs a text pass per frame). The font shrinks until the
 * text is no wider than maxw (0 = no limit). */
static UIImage *text_image(NSString *s, CGFloat fs, CGFloat maxw, UIColor *color)
{
    UIFont *font = [UIFont boldSystemFontOfSize:fs];
    NSDictionary *attrs = @{ NSFontAttributeName: font,
                             NSForegroundColorAttributeName: color };
    CGSize sz = [s sizeWithAttributes:attrs];
    if (maxw > 0.0 && sz.width > maxw && sz.width > 1.0) {
        fs = floor(fs * maxw / sz.width);
        if (fs < 6.0)
            fs = 6.0;
        font = [UIFont boldSystemFontOfSize:fs];
        attrs = @{ NSFontAttributeName: font,
                   NSForegroundColorAttributeName: color };
        sz = [s sizeWithAttributes:attrs];
    }
    NSDictionary *shadow = @{ NSFontAttributeName: font,
                              NSForegroundColorAttributeName: [UIColor colorWithWhite:0.0 alpha:0.55] };
    CGFloat pad = 2.0;
    CGSize canvas = CGSizeMake(ceil(sz.width) + 2.0 * pad, ceil(sz.height) + 2.0 * pad);
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:canvas
                                                                        format:img_format()];
    return [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
        (void)ctx;
        [s drawAtPoint:CGPointMake(pad, pad + 1.0) withAttributes:shadow];
        [s drawAtPoint:CGPointMake(pad, pad) withAttributes:attrs];
    }];
}

/* A white SF Symbol rendered into a bitmap; nil when the symbol is missing. */
static UIImage *symbol_image(NSString *name, CGFloat pt)
{
    UIImageSymbolConfiguration *cfg =
        [UIImageSymbolConfiguration configurationWithPointSize:pt
                                                        weight:UIImageSymbolWeightSemibold];
    UIImage *img = [UIImage systemImageNamed:name withConfiguration:cfg];
    if (!img)
        return nil;
    UIImage *tinted = [img imageWithTintColor:[UIColor whiteColor]
                                renderingMode:UIImageRenderingModeAlwaysOriginal];
    CGSize sz = tinted.size;
    if (sz.width < 1.0 || sz.height < 1.0)
        return nil;
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:sz
                                                                        format:img_format()];
    return [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
        (void)ctx;
        [tinted drawInRect:CGRectMake(0.0, 0.0, sz.width, sz.height)];
    }];
}

/* Closed polygon with every corner rounded by a quad curve. */
static UIBezierPath *rounded_polygon(const CGPoint *p, int n, CGFloat rad)
{
    UIBezierPath *path = [UIBezierPath bezierPath];
    int i;
    for (i = 0; i < n; i++) {
        CGPoint prev = p[(i + n - 1) % n];
        CGPoint cur = p[i];
        CGPoint next = p[(i + 1) % n];
        CGFloat dx1 = prev.x - cur.x, dy1 = prev.y - cur.y;
        CGFloat dx2 = next.x - cur.x, dy2 = next.y - cur.y;
        CGFloat l1 = sqrt(dx1 * dx1 + dy1 * dy1);
        CGFloat l2 = sqrt(dx2 * dx2 + dy2 * dy2);
        CGPoint a, b;
        if (l1 < 0.001 || l2 < 0.001)
            continue;
        a = CGPointMake(cur.x + dx1 / l1 * rad, cur.y + dy1 / l1 * rad);
        b = CGPointMake(cur.x + dx2 / l2 * rad, cur.y + dy2 / l2 * rad);
        if ([path isEmpty])
            [path moveToPoint:a];
        else
            [path addLineToPoint:a];
        [path addQuadCurveToPoint:b controlPoint:cur];
    }
    [path closePath];
    return path;
}

/* The largest rounded rectangle (half extents u*A*fx by u*B*fy, corner radius
 * frac * shorter side) whose corners still sit inside the hit-test ellipse
 * with radii A, B: what you can see is what you can touch. */
static void fit_rounded(CGFloat A, CGFloat B, CGFloat fx, CGFloat fy, CGFloat frac,
                        CGFloat *ow, CGFloat *oh, CGFloat *orad)
{
    CGFloat u, hw = A * fx, hh = B * fy, r = 0.0;
    for (u = 1.0; u > 0.45; u -= 0.02) {
        int ok = 1, k;
        hw = u * A * fx;
        hh = u * B * fy;
        r = frac * 2.0 * fmin(hw, hh);
        for (k = 0; k <= 4; k++) {
            CGFloat ang = (CGFloat)k * (M_PI_2 / 4.0);
            CGFloat px = hw - r + r * cos(ang);
            CGFloat py = hh - r + r * sin(ang);
            CGFloat q = (px / A) * (px / A) + (py / B) * (py / B);
            if (q > 0.99)
                ok = 0;
        }
        if (ok)
            break;
    }
    *ow = 2.0 * hw;
    *oh = 2.0 * hh;
    *orad = r;
}

/* ── One control: a small CALayer subtree per touch zone ───────── */

@interface Nfsu2Ctl : NSObject
@property (nonatomic, assign) int zoneIdx;            /* index in the zone table */
@property (nonatomic, assign) int style;              /* ZS_* */
@property (nonatomic, assign) CGSize size;
@property (nonatomic, strong) CALayer *root;          /* positioned, scaled when pressed */
@property (nonatomic, strong) CAShapeLayer *ring;
@property (nonatomic, strong) CALayer *pressFill;     /* opacity 0 / 1 */
@property (nonatomic, strong) CALayer *levelClip;     /* pedals: analog fill level */
@property (nonatomic, strong) CALayer *levelFill;
@property (nonatomic, strong) NSArray *arms;          /* d-pad: up, down, left, right */
@property (nonatomic, strong) UIColor *ringIdle;
@property (nonatomic, strong) UIColor *ringHot;
@property (nonatomic, assign) int pressed;            /* cached; -1 = unknown */
@property (nonatomic, assign) unsigned dbits;         /* cached d-pad bits */
@property (nonatomic, assign) CGFloat level;          /* 0..1, pedals */
@property (nonatomic, strong) CALayer *knob;          /* steering strip: the knob */
@property (nonatomic, assign) CGFloat travel;         /* strip: knob travel each way, pt */
@property (nonatomic, assign) CGFloat steerPos;       /* strip: -1..1 */
@property (nonatomic, assign) CGFloat value;          /* last analog value applied; -2 = none yet */
@end

@implementation Nfsu2Ctl
- (void)setLevel:(CGFloat)v
{
    CGFloat w = self.size.width, h = self.size.height;
    v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
    _level = v;
    if (!self.levelFill)
        return;
    self.levelClip.hidden = (v <= 0.001) ? YES : NO;
    self.levelFill.frame = CGRectMake(0.0, h * (1.0 - v), w, h * v);
}

/* The strip's knob: v = -1..1 across the track. Callers wrap this in a
 * CATransaction (no actions while following a finger, short at release). */
- (void)setSteerPos:(CGFloat)v
{
    v = v < -1.0 ? -1.0 : v > 1.0 ? 1.0 : v;
    _steerPos = v;
    if (!self.knob)
        return;
    self.knob.position = CGPointMake(self.size.width / 2.0 + v * self.travel,
                                     self.size.height / 2.0);
}

/* Follow the zone's analog value (pedal level or strip steer). */
- (void)setAnalogValue:(CGFloat)v
{
    self.value = v;
    if (self.style == ZS_STEER)
        [self setSteerPos:v];
    else
        [self setLevel:v];
}

/* Callers wrap this in a CATransaction (short duration, or no actions). */
- (void)applyPressed:(int)p dpad:(unsigned)bits
{
    self.pressed = p;
    self.dbits = bits;
    if (self.style == ZS_STEER) {
        self.pressFill.opacity = p ? 1.0f : 0.0f;
        self.knob.opacity = p ? 1.0f : 0.6f;     /* no shrink: the track stays put */
    } else if (self.style == ZS_DPAD) {
        NSUInteger k;
        for (k = 0; k < self.arms.count; k++) {
            CALayer *a = [self.arms objectAtIndex:k];
            a.opacity = (bits & (1u << k)) ? 1.0f : 0.0f;
        }
    } else {
        self.pressFill.opacity = p ? 1.0f : 0.0f;
        self.root.transform = p ? CATransform3DMakeScale(0.92, 0.92, 1.0)
                                : CATransform3DIdentity;
    }
    set_stroke(self.ring, p ? self.ringHot : self.ringIdle);
}
@end

/* Builds the layers of zone idx. Geometry comes from the runtime zone table
 * (normalized centre and radii against the overlay size, in points). */
static Nfsu2Ctl *build_ctl(int idx, const XBOX_IOS_TOUCH_ZONE_MIRROR *z, CGFloat W, CGFloat H)
{
    int style = xbox_ios_touch_zone_style(idx);
    CGFloat A = z->rx * W, B = z->ry * H;
    CGFloat w, h, r = 0.0;
    BOOL cross = (style == ZS_DPAD);
    BOOL pedal = (style == ZS_PEDAL_GAS || style == ZS_PEDAL_BRAKE);
    BOOL trigger = (style == ZS_TRIGGER_L || style == ZS_TRIGGER_R);
    BOOL menu = (style == ZS_MENU_BACK || style == ZS_MENU_START);
    BOOL face = (style <= ZS_FACE_Y);
    BOOL shoulder = (style == ZS_SHOULDER_BLACK || style == ZS_SHOULDER_WHITE);
    BOOL steer = (style == ZS_STEER);
    /* DRIVE names its buttons by function ("E-BRAKE", "NOS", "SHIFT"): a label
     * of three or more characters does not fit a circle, so that face button
     * is drawn as a pill. */
    const char *lbl0 = xbox_ios_touch_zone_label(idx);
    BOOL wide = (face && lbl0 && strlen(lbl0) > 2);
    BOOL circle = ((face && !wide) || shoulder);
    UIColor *accent = nil;
    UIColor *white = [UIColor colorWithWhite:1.0 alpha:0.92];
    CGPoint pts[12];
    CGFloat prad = 0.0;

    switch (style) {
    case ZS_FACE_A:     case ZS_PEDAL_GAS:   accent = rgba(0x5DC21E, 1.0); break;
    case ZS_FACE_B:     case ZS_PEDAL_BRAKE: accent = rgba(0xE0313A, 1.0); break;
    case ZS_FACE_X:                          accent = rgba(0x2D7BF4, 1.0); break;
    case ZS_FACE_Y:                          accent = rgba(0xF2C21B, 1.0); break;
    default: break;
    }

    if (cross) {
        w = 2.0 * fmin(A, B);
        h = w;
    } else if (circle) {
        w = 2.0 * fmin(A, B);
        h = w;
        r = w / 2.0;
    } else if (pedal) {
        fit_rounded(A, B, 1.0, 1.0, 0.25, &w, &h, &r);
    } else if (steer) {
        /* The zone is a rectangle: a pill 60 % of its height, full width. */
        w = 2.0 * A;
        h = 2.0 * B * 0.6;
        r = h / 2.0;
    } else if (wide) {
        fit_rounded(A, B, 1.0, 0.80, 0.5, &w, &h, &r);
    } else if (trigger) {
        fit_rounded(A, B, 0.72, 1.0, 0.5, &w, &h, &r);
    } else {
        fit_rounded(A, B, 1.0, 0.62, 0.5, &w, &h, &r);
    }

    CALayer *root = [CALayer layer];
    root.bounds = CGRectMake(0.0, 0.0, w, h);
    root.position = CGPointMake(z->cx * W, z->cy * H);
    root.contentsScale = s_cscale;
    CGRect R = root.bounds;

    /* Cross outline (d-pad), also the ring path. */
    if (cross) {
        CGFloat c0 = w / 2.0, L = 0.93 * c0, t = 0.32 * c0;
        pts[0]  = CGPointMake(c0 - t, c0 - L);  pts[1]  = CGPointMake(c0 + t, c0 - L);
        pts[2]  = CGPointMake(c0 + t, c0 - t);  pts[3]  = CGPointMake(c0 + L, c0 - t);
        pts[4]  = CGPointMake(c0 + L, c0 + t);  pts[5]  = CGPointMake(c0 + t, c0 + t);
        pts[6]  = CGPointMake(c0 + t, c0 + L);  pts[7]  = CGPointMake(c0 - t, c0 + L);
        pts[8]  = CGPointMake(c0 - t, c0 + t);  pts[9]  = CGPointMake(c0 - L, c0 + t);
        pts[10] = CGPointMake(c0 - L, c0 - t);  pts[11] = CGPointMake(c0 - t, c0 - t);
        prad = 0.12 * c0;
    }

    /* Body: glassy dark gradient. */
    CAGradientLayer *body = [CAGradientLayer layer];
    body.frame = R;
    body.contentsScale = s_cscale;
    if (shoulder)
        set_gradient(body, [UIColor colorWithWhite:0.22 alpha:0.50],
                           [UIColor colorWithWhite:0.08 alpha:0.34]);
    else
        set_gradient(body, [UIColor colorWithWhite:0.0 alpha:0.46],
                           [UIColor colorWithWhite:0.0 alpha:0.30]);
    if (cross) {
        CAShapeLayer *mask = [CAShapeLayer layer];
        mask.frame = R;
        set_path(mask, rounded_polygon(pts, 12, prad));
        body.mask = mask;
    } else {
        body.cornerRadius = r;
        body.masksToBounds = YES;
    }
    [root addSublayer:body];

    /* Pressed fill (faces use their colour). */
    Nfsu2Ctl *c = [[Nfsu2Ctl alloc] init];
    if (!cross) {
        CALayer *pf = [CALayer layer];
        pf.frame = R;
        pf.cornerRadius = r;
        if (face)
            set_bg(pf, [accent colorWithAlphaComponent:0.35]);
        else if (steer)
            set_bg(pf, [UIColor colorWithWhite:1.0 alpha:0.10]);
        else if (pedal)
            set_bg(pf, [UIColor colorWithWhite:1.0 alpha:0.22]);
        else
            set_bg(pf, [UIColor colorWithWhite:1.0 alpha:0.30]);
        pf.opacity = 0.0f;
        [root addSublayer:pf];
        c.pressFill = pf;
    }

    /* Pedal: analog fill level from the bottom (0 for now). */
    if (pedal) {
        CALayer *clip = [CALayer layer];
        clip.frame = R;
        clip.cornerRadius = r;
        clip.masksToBounds = YES;
        clip.hidden = YES;
        CALayer *lf = [CALayer layer];
        lf.frame = CGRectMake(0.0, h, w, 0.0);
        set_bg(lf, [accent colorWithAlphaComponent:0.35]);
        [clip addSublayer:lf];
        [root addSublayer:clip];
        c.levelClip = clip;
        c.levelFill = lf;

        /* Grip lines in the lower part. */
        CAShapeLayer *grips = [CAShapeLayer layer];
        UIBezierPath *gp = [UIBezierPath bezierPath];
        int k;
        grips.frame = R;
        grips.contentsScale = s_cscale;
        for (k = 0; k < 5; k++) {
            CGFloat gy = h * (0.56 + 0.085 * (CGFloat)k);
            [gp moveToPoint:CGPointMake(w * 0.24, gy)];
            [gp addLineToPoint:CGPointMake(w * 0.76, gy)];
        }
        set_path(grips, gp);
        grips.fillColor = NULL;
        set_stroke(grips, [UIColor colorWithWhite:1.0 alpha:0.18]);
        grips.lineWidth = 1.5;
        grips.lineCap = kCALineCapRound;
        [root addSublayer:grips];
    }

    /* Faint inner highlight along the top. */
    {
        CAShapeLayer *hi = [CAShapeLayer layer];
        UIBezierPath *hp = [UIBezierPath bezierPath];
        hi.frame = R;
        hi.contentsScale = s_cscale;
        if (cross) {
            CGFloat c0 = w / 2.0, L = 0.93 * c0, t = 0.32 * c0;
            [hp moveToPoint:CGPointMake(c0 - t + 4.0, c0 - L + 3.5)];
            [hp addLineToPoint:CGPointMake(c0 + t - 4.0, c0 - L + 3.5)];
        } else if (circle || (w - 2.0 * r) < 6.0) {
            CGFloat hr = fmax(r - 3.5, 2.0);
            CGFloat a0 = M_PI * 1.22, a1 = M_PI * 1.78;
            int k;
            for (k = 0; k <= 12; k++) {
                CGFloat a = a0 + (a1 - a0) * (CGFloat)k / 12.0;
                CGPoint pt = CGPointMake(w / 2.0 + hr * cos(a), r + hr * sin(a));
                if (k == 0)
                    [hp moveToPoint:pt];
                else
                    [hp addLineToPoint:pt];
            }
        } else {
            [hp moveToPoint:CGPointMake(r, 3.5)];
            [hp addLineToPoint:CGPointMake(w - r, 3.5)];
        }
        set_path(hi, hp);
        hi.fillColor = NULL;
        set_stroke(hi, [UIColor colorWithWhite:1.0 alpha:0.20]);
        hi.lineWidth = 1.0;
        hi.lineCap = kCALineCapRound;
        [root addSublayer:hi];
    }

    /* Ring. */
    CAShapeLayer *ring = [CAShapeLayer layer];
    {
        CGRect ri = CGRectInset(R, 0.75, 0.75);
        UIBezierPath *rp;
        if (cross)
            rp = rounded_polygon(pts, 12, prad);
        else if (circle)
            rp = [UIBezierPath bezierPathWithOvalInRect:ri];
        else
            rp = [UIBezierPath bezierPathWithRoundedRect:ri
                                            cornerRadius:fmax(r - 0.75, 0.0)];
        ring.frame = R;
        ring.contentsScale = s_cscale;
        set_path(ring, rp);
        ring.fillColor = NULL;
        ring.lineWidth = 1.5;
        ring.lineJoin = kCALineJoinRound;
        if (face) {
            c.ringIdle = [accent colorWithAlphaComponent:0.55];
            c.ringHot = [accent colorWithAlphaComponent:0.95];
        } else if (pedal) {
            c.ringIdle = [accent colorWithAlphaComponent:0.75];
            c.ringHot = [accent colorWithAlphaComponent:1.0];
        } else if (shoulder) {
            c.ringIdle = [UIColor colorWithWhite:1.0 alpha:0.38];
            c.ringHot = [UIColor colorWithWhite:1.0 alpha:0.90];
        } else {
            c.ringIdle = [UIColor colorWithWhite:1.0 alpha:0.45];
            c.ringHot = [UIColor colorWithWhite:1.0 alpha:0.90];
        }
        set_stroke(ring, c.ringIdle);
        [root addSublayer:ring];
    }

    /* Steering strip: centre mark, a chevron at each end and the knob. The
     * knob follows xbox_ios_touch_zone_value(); it rests at the centre. */
    if (steer) {
        CAShapeLayer *marks = [CAShapeLayer layer];
        UIBezierPath *mp = [UIBezierPath bezierPath];
        CGFloat cy0 = h / 2.0, ch = h * 0.16, cx0 = r * 0.85, kd = h * 0.78;
        CALayer *knob = [CALayer layer];
        CAGradientLayer *kg = [CAGradientLayer layer];
        UIColor *black = [UIColor blackColor];

        [mp moveToPoint:CGPointMake(w / 2.0, h * 0.22)];
        [mp addLineToPoint:CGPointMake(w / 2.0, h * 0.78)];
        [mp moveToPoint:CGPointMake(cx0 + ch * 0.6, cy0 - ch)];
        [mp addLineToPoint:CGPointMake(cx0, cy0)];
        [mp addLineToPoint:CGPointMake(cx0 + ch * 0.6, cy0 + ch)];
        [mp moveToPoint:CGPointMake(w - cx0 - ch * 0.6, cy0 - ch)];
        [mp addLineToPoint:CGPointMake(w - cx0, cy0)];
        [mp addLineToPoint:CGPointMake(w - cx0 - ch * 0.6, cy0 + ch)];
        marks.frame = R;
        marks.contentsScale = s_cscale;
        set_path(marks, mp);
        marks.fillColor = NULL;
        set_stroke(marks, [UIColor colorWithWhite:1.0 alpha:0.28]);
        marks.lineWidth = 2.0;
        marks.lineCap = kCALineCapRound;
        marks.lineJoin = kCALineJoinRound;
        [root addSublayer:marks];

        knob.bounds = CGRectMake(0.0, 0.0, kd, kd);
        knob.position = CGPointMake(w / 2.0, h / 2.0);
        set_shadow(knob, [UIBezierPath bezierPathWithOvalInRect:knob.bounds], black);
        knob.shadowOpacity = 0.35f;
        knob.shadowRadius = 4.0;
        knob.shadowOffset = CGSizeMake(0.0, 2.0);
        knob.opacity = 0.6f;
        kg.frame = knob.bounds;
        kg.contentsScale = s_cscale;
        kg.cornerRadius = kd / 2.0;
        kg.masksToBounds = YES;
        set_border(kg, [UIColor colorWithWhite:1.0 alpha:0.85], 1.5);
        set_gradient(kg, [UIColor colorWithWhite:1.0 alpha:0.70],
                         [UIColor colorWithWhite:1.0 alpha:0.38]);
        [knob addSublayer:kg];
        [root addSublayer:knob];
        c.knob = knob;
        c.travel = fmax((w - kd) / 2.0 - 4.0, 1.0);
    }

    /* D-pad: four arm highlights, lit by xbox_ios_touch_dpad_bits(). */
    if (cross) {
        CGFloat c0 = w / 2.0, L = 0.93 * c0, t = 0.32 * c0, ar = 0.12 * c0;
        CGRect ar_rect[4];
        NSMutableArray *arms = [NSMutableArray arrayWithCapacity:4];
        int k;
        ar_rect[0] = CGRectMake(c0 - t, c0 - L, 2.0 * t, L - t);   /* up */
        ar_rect[1] = CGRectMake(c0 - t, c0 + t, 2.0 * t, L - t);   /* down */
        ar_rect[2] = CGRectMake(c0 - L, c0 - t, L - t, 2.0 * t);   /* left */
        ar_rect[3] = CGRectMake(c0 + t, c0 - t, L - t, 2.0 * t);   /* right */
        for (k = 0; k < 4; k++) {
            CAShapeLayer *arm = [CAShapeLayer layer];
            arm.frame = R;
            arm.contentsScale = s_cscale;
            set_path(arm, [UIBezierPath bezierPathWithRoundedRect:ar_rect[k]
                                                     cornerRadius:ar]);
            set_fill(arm, [UIColor colorWithWhite:1.0 alpha:0.34]);
            arm.opacity = 0.0f;
            [root addSublayer:arm];
            [arms addObject:arm];
        }
        c.arms = arms;
    }

    /* Label or icon, rendered once. */
    {
        const char *lbl = lbl0;
        UIImage *li NS_VALID_UNTIL_END_OF_SCOPE = nil;
        CGFloat ly = h / 2.0;
        if (menu) {
            BOOL back = (style == ZS_MENU_BACK);
            li = symbol_image(back ? @"rectangle.on.rectangle" : @"line.3.horizontal", h * 0.46);
            if (!li)
                li = text_image(back ? @"BACK" : @"START", h * 0.34, w * 0.86, white);
        } else if (lbl) {
            NSString *s = [NSString stringWithUTF8String:lbl];
            if (wide)
                li = text_image(s, h * 0.40, w * 0.84, accent);
            else if (face)
                li = text_image(s, w * 0.46, w * 0.70, accent);
            else if (shoulder)
                li = text_image(s, w * 0.30, w * 0.72, white);
            else if (trigger)
                li = text_image(s, fmin(h * 0.22, w * 0.42), w * 0.80, white);
            else if (pedal)
                li = text_image(s, h * 0.12, w * 0.78, white);
        }
        if (li) {
            CGImageRef cgi = li.CGImage;
            CALayer *ll = [CALayer layer];
            if (pedal)
                ly = h * 0.08 + li.size.height / 2.0;
            ll.bounds = CGRectMake(0.0, 0.0, li.size.width, li.size.height);
            ll.position = CGPointMake(w / 2.0, ly);
            ll.contentsScale = li.scale;
            ll.contents = (__bridge id)cgi;
            [root addSublayer:ll];
        }
    }

    c.zoneIdx = idx;
    c.style = style;
    c.size = CGSizeMake(w, h);
    c.root = root;
    c.ring = ring;
    c.pressed = -1;
    c.dbits = 0xFFFFFFFFu;
    c.value = -2.0;
    return c;
}

/* ── The overlay: layer host, rebuild, per-tick update ─────────── */

@interface Nfsu2Overlay : UIView {
    CALayer *_host;               /* parent of every control and stick layer */
    NSMutableArray *_ctls;        /* Nfsu2Ctl, zone-table order */
    NSMutableArray *_sBase;       /* 2 CAShapeLayer: stick base rings */
    NSMutableArray *_sKnob;       /* 2 CALayer: stick knobs */
    BOOL _stickOn[2];
    BOOL _built;
    unsigned _gen;                /* xbox_ios_touch_layout_gen() at the build */
    CGSize _builtSize;
}
- (void)applyVisibility:(BOOL)vis;
- (BOOL)syncControls;
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
        self.alpha = nfsu2_cfg_overlay_alpha();
        self.tag = 0x4E463531;             /* 'NF51' */
        _ctls = [[NSMutableArray alloc] init];
        _sBase = [[NSMutableArray alloc] init];
        _sKnob = [[NSMutableArray alloc] init];
        _host = [CALayer layer];
        _host.frame = self.bounds;
        [self.layer addSublayer:_host];
        self.shapesHidden = NO;
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

- (void)setShapesHidden:(BOOL)h
{
    _shapesHidden = h;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _host.hidden = h;
    [CATransaction commit];
    if (!h) {
        (void)[self syncControls];
        overlay_wake();
    }
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    (void)[self syncControls];          /* rebuilds when the bounds changed */
}

/* Rebuild every control and stick layer from the zone table. Main thread. */
- (void)rebuildControls
{
    int n = 0, i, side;
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    unsigned dbits;
    const XBOX_IOS_TOUCH_ZONE_MIRROR *z;
    NSArray *old;

    _gen = xbox_ios_touch_layout_gen();
    z = (const XBOX_IOS_TOUCH_ZONE_MIRROR *)xbox_ios_touch_zones(&n);
    s_cscale = [UIScreen mainScreen].scale;
    if (s_cscale < 1.0)
        s_cscale = 1.0;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _host.frame = self.bounds;
    old = [_host.sublayers copy];
    for (CALayer *l in old)
        [l removeFromSuperlayer];
    [_ctls removeAllObjects];
    [_sBase removeAllObjects];
    [_sKnob removeAllObjects];

    dbits = xbox_ios_touch_dpad_bits();
    if (z && n > 0) {
        for (i = 0; i < n; i++) {
            Nfsu2Ctl *c = build_ctl(i, &z[i], W, H);
            [_host addSublayer:c.root];
            [_ctls addObject:c];
            [c applyPressed:xbox_ios_touch_zone_pressed(i)
                       dpad:(c.style == ZS_DPAD ? dbits : 0u)];
        }
    }

    /* Floating sticks: base ring radius 0.12 of the width (the touch layer's
     * full travel), knob 0.42 of that. Hidden until a finger drives them. */
    {
        CGFloat R = 0.12 * W, kr = R * 0.42;
        UIColor *black = [UIColor blackColor];
        for (side = 0; side < 2; side++) {
            CAShapeLayer *base = [CAShapeLayer layer];
            base.bounds = CGRectMake(0.0, 0.0, 2.0 * R, 2.0 * R);
            base.contentsScale = s_cscale;
            set_path(base, [UIBezierPath bezierPathWithOvalInRect:CGRectInset(base.bounds, 0.75, 0.75)]);
            set_fill(base, [UIColor colorWithWhite:1.0 alpha:0.05]);
            set_stroke(base, [UIColor colorWithWhite:1.0 alpha:0.35]);
            base.lineWidth = 1.5;
            base.opacity = 0.0f;

            CALayer *knob = [CALayer layer];
            knob.bounds = CGRectMake(0.0, 0.0, 2.0 * kr, 2.0 * kr);
            set_shadow(knob, [UIBezierPath bezierPathWithOvalInRect:knob.bounds], black);
            knob.shadowOpacity = 0.35f;
            knob.shadowRadius = 4.0;
            knob.shadowOffset = CGSizeMake(0.0, 2.0);
            knob.opacity = 0.0f;

            CAGradientLayer *kg = [CAGradientLayer layer];
            kg.frame = knob.bounds;
            kg.contentsScale = s_cscale;
            kg.cornerRadius = kr;
            kg.masksToBounds = YES;
            set_border(kg, [UIColor colorWithWhite:1.0 alpha:0.85], 1.5);
            set_gradient(kg, [UIColor colorWithWhite:1.0 alpha:0.70],
                             [UIColor colorWithWhite:1.0 alpha:0.38]);
            [knob addSublayer:kg];

            [_host addSublayer:base];
            [_host addSublayer:knob];
            [_sBase addObject:base];
            [_sKnob addObject:knob];
            _stickOn[side] = NO;
        }
    }
    _built = YES;
    _builtSize = self.bounds.size;
    [CATransaction commit];
}

/* The two sticks: shown while a finger drives them, base ring at the
 * touch-down point, knob at the finger (clamped to full travel). Returns YES
 * while either is down. */
- (BOOL)syncSticks
{
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    CGFloat R = 0.12 * W;
    BOOL any = NO;
    int side;

    if (_sBase.count < 2 || _sKnob.count < 2)
        return NO;
    for (side = 0; side < 2; side++) {
        float bx, by, kx, ky;
        CALayer *base = [_sBase objectAtIndex:(NSUInteger)side];
        CALayer *knob = [_sKnob objectAtIndex:(NSUInteger)side];
        if (xbox_ios_touch_stick(side, &bx, &by, &kx, &ky)) {
            CGPoint bp = CGPointMake(bx * W, by * H);
            CGPoint kp = CGPointMake(kx * W, ky * H);
            CGFloat vx = kp.x - bp.x, vy = kp.y - bp.y;
            CGFloat d = sqrt(vx * vx + vy * vy);
            if (d > R) {
                kp.x = bp.x + vx / d * R;
                kp.y = bp.y + vy / d * R;
            }
            any = YES;
            [CATransaction begin];
            [CATransaction setDisableActions:YES];   /* follow the finger exactly */
            base.position = bp;
            knob.position = kp;
            [CATransaction commit];
            if (!_stickOn[side]) {
                _stickOn[side] = YES;
                [CATransaction begin];
                [CATransaction setAnimationDuration:0.10];
                base.opacity = 1.0f;
                knob.opacity = 1.0f;
                [CATransaction commit];
            }
        } else if (_stickOn[side]) {
            _stickOn[side] = NO;
            [CATransaction begin];
            [CATransaction setAnimationDuration:0.18];
            base.opacity = 0.0f;
            knob.opacity = 0.0f;
            [CATransaction commit];
        }
    }
    return any;
}

/* Rebuild when the zone table or the bounds changed, then bring every layer
 * in line with the touch state; only layers whose pressed state changed are
 * touched. Returns YES while anything is held (the display link keeps
 * running). Main thread. */
- (BOOL)syncControls
{
    CGSize sz = self.bounds.size;
    BOOL active = NO, open = NO;
    unsigned dbits;
    NSUInteger i, n;

    if (_shapesHidden || sz.width < 1.0 || sz.height < 1.0)
        return NO;
    if (!_built || xbox_ios_touch_layout_gen() != _gen ||
        sz.width != _builtSize.width || sz.height != _builtSize.height)
        [self rebuildControls];

    dbits = xbox_ios_touch_dpad_bits();
    n = _ctls.count;
    for (i = 0; i < n; i++) {
        Nfsu2Ctl *c = [_ctls objectAtIndex:i];
        int p = xbox_ios_touch_zone_pressed(c.zoneIdx) ? 1 : 0;
        unsigned db = (c.style == ZS_DPAD) ? dbits : 0u;
        BOOL analog = (c.style == ZS_PEDAL_GAS || c.style == ZS_PEDAL_BRAKE ||
                       c.style == ZS_STEER);
        if (p)
            active = YES;
        if (p != c.pressed || db != c.dbits) {
            if (!open) {                 /* a transaction only when something changed */
                [CATransaction begin];
                [CATransaction setAnimationDuration:0.08];
                open = YES;
            }
            [c applyPressed:p dpad:db];
            if (analog && !p)
                [c setAnalogValue:0.0];   /* fill drains / knob returns to centre, eased */
        }
        if (analog && p) {
            /* Pedal level and strip knob follow the finger every tick, with
             * no implicit animation (it would trail the thumb). Only a value
             * that moved is written. */
            CGFloat v = (CGFloat)xbox_ios_touch_zone_value(c.zoneIdx);
            if (fabs(v - c.value) > 0.004) {
                [CATransaction begin];   /* nests inside the eased one, if open */
                [CATransaction setDisableActions:YES];
                [c setAnalogValue:v];
                [CATransaction commit];
            }
        }
    }
    if (open)
        [CATransaction commit];
    if ([self syncSticks])
        active = YES;
    return active;
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
}

/* ── Update loop: display link, paused while idle ──
 * Touch state changes on the SDL thread and there is no change callback, so:
 * the link (30 fps) runs only while a zone is held or a stick is driven, and
 * pauses after 3 idle ticks. It wakes through three paths, together every way
 * the touch state or the visibility can change while it is paused:
 *   1. the touch layer's activity hook: ANY finger down (zone, steering strip,
 *      dynamic stick), posted to the main thread at once;
 *   2. ios_touch_overlay_set_visible -> applyVisibility -> setShapesHidden:NO
 *      (a pad or keyboard leaves), which syncs and wakes;
 *   3. apply_live (the panel: touch on, layout, size, strip): syncs at once.
 * A finger's later moves and its lift need no wake: the finger that went down
 * woke the link and it keeps ticking while anything is held. The old 20 Hz
 * NSTimer that polled the touch state while the link slept is gone; it comes
 * back with NFSU2_OVERLAY_WATCH=1 in nfsu2_env.txt (next launch). */
static CADisplayLink *s_link;
static NSTimer *s_watch;
static int s_idle_ticks;

static void overlay_wake(void)
{
    s_idle_ticks = 0;
    if (s_link && s_link.paused)
        s_link.paused = NO;
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

/* Main thread: any finger down. Show the touch at once, wake the update loop.
 * Posted before the press hook's block, so the control is lit when the
 * haptic fires. */
static void touch_activity_main(void)
{
    if (s_ov)
        (void)[s_ov syncControls];
    overlay_wake();
}

/* Runs on whichever thread pumps SDL events (often not main). */
static void touch_activity_hook(void)
{
    if ([NSThread isMainThread])
        touch_activity_main();
    else
        dispatch_async(dispatch_get_main_queue(), ^{ touch_activity_main(); });
}

/* Main thread: the haptic for a zone press. */
static void touch_hook_main(void)
{
    if (__atomic_load_n(&s_haptics_on, __ATOMIC_RELAXED))
        haptic_fire_main();
}

/* Runs on whichever thread pumps SDL events (often not main). */
static void touch_press_hook(int zone_idx)
{
    (void)zone_idx;
    if ([NSThread isMainThread])
        touch_hook_main();
    else
        dispatch_async(dispatch_get_main_queue(), ^{ touch_hook_main(); });
}

/* ── Tilt steering ── */

static CMMotionManager *s_motion;
static float s_tilt_sens = 1.0f;
static int s_tilt_flip;               /* NFSU2_TILT_FLIP=1: swap the sign */
/* "Centre now": the roll angle (degrees, asin of gravity along the device's
 * long axis, before the orientation sign) taken as neutral. Main thread only
 * (the motion handler runs on the main queue). 0 = today's behaviour. */
static double s_tilt_centre_deg;      /* NFSU2_TILT_CENTRE, -60..60, 1 decimal */
static double s_tilt_last_deg;        /* latest sample */
static int s_tilt_have_sample;        /* s_tilt_last_deg is valid (tilt running) */

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
    s_tilt_last_deg = asin(gy) * (180.0 / M_PI);
    s_tilt_have_sample = 1;
    /* The neutral offset is in the device frame, so it survives a flip of the
     * landscape orientation (the sign multiplies the difference). */
    n = sign * (asin(gy) - s_tilt_centre_deg * (M_PI / 180.0))
        / 0.5235988 * (double)s_tilt_sens;                    /* 30 degrees */
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
    s_tilt_have_sample = 0;
    xbox_ios_touch_set_tilt(0.0f, 0);
}

/* "Centre now": the current angle becomes the neutral. Needs tilt running
 * (there is no sample otherwise); returns NO then. Saved with one decimal. */
static BOOL tilt_centre_now(void)
{
    char buf[24];
    double d;

    if (!s_tilt_have_sample)
        return NO;
    d = s_tilt_last_deg;
    d = d > 60.0 ? 60.0 : d < -60.0 ? -60.0 : d;
    snprintf(buf, sizeof buf, "%.1f", d);
    s_tilt_centre_deg = atof(buf);        /* what the file will say */
    nfsu2_cfg_set("NFSU2_TILT_CENTRE", buf);
    return YES;
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

enum { IK_HEAD, IK_SWITCH, IK_SLIDER, IK_SEG, IK_BUTTON };
enum { ACT_NONE, ACT_FPS, ACT_TOUCH, ACT_LAYOUT, ACT_SCALE, ACT_ALPHA,
       ACT_HAPTICS, ACT_TILT, ACT_TILTSENS, ACT_KEYBOARD, ACT_RUMBLE,
       ACT_ANALOG, ACT_STEER, ACT_STEERSENS, ACT_TILTCENTRE };

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
    /* Drive layout: pedal value from the finger's height; the steering strip
     * (a switch back to the dynamic stick) and its gain. */
    { .kind = IK_SWITCH, .title = "Analog pedals", .key = "NFSU2_ANALOG_PEDALS",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_ANALOG },
    { .kind = IK_SWITCH, .title = "Steering strip (Drive layout)", .key = "NFSU2_STEER",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_STEER },
    { .kind = IK_SLIDER, .title = "Steering sensitivity", .key = "NFSU2_STEER_SENS",
      .lo = 0.5f, .hi = 2.0f, .def = 1.0f, .decimals = 2, .act = ACT_STEERSENS },
    { .kind = IK_SWITCH, .title = "Haptics", .key = "NFSU2_HAPTICS",
      .on = "1", .off = "0", .def_on = 1, .act = ACT_HAPTICS },
    { .kind = IK_SWITCH, .title = "Tilt steering", .key = "NFSU2_TILT",
      .on = "1", .off = "0", .def_on = 0, .act = ACT_TILT },
    { .kind = IK_SLIDER, .title = "Tilt sensitivity", .key = "NFSU2_TILT_SENS",
      .lo = 0.5f, .hi = 2.0f, .def = 1.0f, .decimals = 2, .act = ACT_TILTSENS },
    /* Needs Tilt steering on: stores the current angle as straight ahead. */
    { .kind = IK_BUTTON, .title = "Centre tilt now", .key = "NFSU2_TILT_CENTRE",
      .act = ACT_TILTCENTRE },
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
        (void)[s_ov syncControls];     /* new layout generation: relays the layers */
        break;
    case ACT_SCALE:
        xbox_ios_touch_set_scale(fv);
        (void)[s_ov syncControls];
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
    case ACT_ANALOG:
        xbox_ios_touch_set_analog_pedals(on);
        break;
    case ACT_STEER:
        xbox_ios_touch_set_steer(on);      /* rebuilds the table, clears fingers */
        (void)[s_ov syncControls];         /* strip appears / goes now */
        break;
    case ACT_STEERSENS:
        xbox_ios_touch_set_steer_sens(fv);
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

/* Caption of a button row, with the stored value where there is one. */
static NSString *button_title(int act)
{
    if (act == ACT_TILTCENTRE)
        return [NSString stringWithFormat:@"Centre tilt now (offset %.1f°)",
                                          s_tilt_centre_deg];
    return @"";
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
    if (it->kind == IK_BUTTON) {
        UIButton *bt = [UIButton buttonWithType:UIButtonTypeSystem];
        bt.frame = CGRectMake(12, y + 2, w - 24, 32);
        [bt setTitle:button_title(it->act) forState:UIControlStateNormal];
        bt.titleLabel.font = [UIFont boldSystemFontOfSize:14];
        bt.tag = idx + 1;
        [bt addTarget:s_target action:@selector(buttonTapped:)
                  forControlEvents:UIControlEventTouchUpInside];
        [s_panel addSubview:bt];
        return y + 40;
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

- (void)buttonTapped:(UIButton *)bt
{
    int idx = (int)bt.tag - 1;
    if (idx < 0 || idx >= ITEM_COUNT)
        return;
    if (s_items[idx].act == ACT_TILTCENTRE) {
        if (tilt_centre_now())
            [bt setTitle:button_title(ACT_TILTCENTRE) forState:UIControlStateNormal];
        else
            [bt setTitle:@"Turn on Tilt steering first" forState:UIControlStateNormal];
    }
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
    /* Display link tick: sync the layers; pause when nothing is held. */
    BOOL active = s_ov ? [s_ov syncControls] : NO;
    if (active)
        s_idle_ticks = 0;
    else if (++s_idle_ticks >= 3 && s_link)
        s_link.paused = YES;
}

- (void)wakeWatch
{
    /* NFSU2_OVERLAY_WATCH=1 only: the old low-rate poll while the link is
     * paused. The activity hook makes it unnecessary. */
    if (s_link && s_link.paused && s_ov && [s_ov syncControls])
        overlay_wake();
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

    if (s_link)
        [s_link invalidate];
    if (s_watch)
        [s_watch invalidate];
    CADisplayLink *dl = [CADisplayLink displayLinkWithTarget:s_target
                                                    selector:@selector(stickTick)];
    dl.preferredFramesPerSecond = 30;    /* touch visuals do not need 120 */
    dl.paused = YES;                     /* runs only while something is held */
    [dl addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    s_link = dl;

    /* The 20 Hz watcher is off: touch_activity_hook wakes the link on every
     * finger down. NFSU2_OVERLAY_WATCH=1 brings it back. */
    {
        const char *wv = nfsu2_cfg_get("NFSU2_OVERLAY_WATCH");
        if (!wv || !*wv)
            wv = getenv("NFSU2_OVERLAY_WATCH");
        if (wv && *wv == '1') {
            NSTimer *wt = [NSTimer timerWithTimeInterval:0.05
                                                  target:s_target
                                                selector:@selector(wakeWatch)
                                                userInfo:nil
                                                 repeats:YES];
            wt.tolerance = 0.02;
            [[NSRunLoop mainRunLoop] addTimer:wt forMode:NSRunLoopCommonModes];
            s_watch = wt;
        }
    }
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
        xbox_ios_touch_set_activity_hook(touch_activity_hook);
        xbox_ios_touch_set_press_hook(touch_press_hook);
        /* Drive-feel settings: the layer already read its env defaults; a
         * stored value wins here. */
        v = nfsu2_cfg_get("NFSU2_ANALOG_PEDALS");
        if (v && *v)
            xbox_ios_touch_set_analog_pedals(!(*v == '0'));
        v = nfsu2_cfg_get("NFSU2_STEER_SENS");
        if (v && *v)
            xbox_ios_touch_set_steer_sens((float)atof(v));
        v = nfsu2_cfg_get("NFSU2_STEER");
        if (v && *v)
            xbox_ios_touch_set_steer(!(*v == '0'));
        v = nfsu2_cfg_get("NFSU2_TILT_CENTRE");
        if (v && *v) {
            double d = atof(v);
            s_tilt_centre_deg = d > 60.0 ? 60.0 : d < -60.0 ? -60.0 : d;
        }
        v = nfsu2_cfg_get("NFSU2_TILT");
        if (v && *v == '1')
            tilt_start();
    }
    ios_keyboard_install();

    (void)[ov syncControls];
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
