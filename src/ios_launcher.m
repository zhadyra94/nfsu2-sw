/*
 * ios_launcher.m - the screen before boot
 *
 * ios_launcher_run() is called once from main() (iOS) on the UIKit main
 * thread, after cfg_load and before the defaults, SDL_CreateWindow and every
 * other thread, only when NFSU2_LAUNCHER=1 or no game data was found (v29:
 * off by default, the gear panel has the same rows). It shows a full-screen UIWindow with
 *   - whether game data was found (nfsu2_game_data_status, main.c),
 *   - the Display / Audio / Controls settings that otherwise only apply on
 *     the next launch,
 *   - a switch for this screen itself (NFSU2_LAUNCHER),
 *   - a Play button,
 * and returns when Play is tapped. Every change goes through nfsu2_cfg_set
 * (table + file, no setenv); main() then exports the changed keys with
 * nfsu2_cfg_export_changed(). Same keys, values and defaults as the in-game
 * panel (ios_touch_overlay.m s_items).
 *
 * The run loop is spun in short slices here instead of returning to SDL: no
 * timers, no threads, nothing left behind when the window is released.
 * Returns 1 after Play, 0 when the screen could not be shown (the caller
 * goes straight on to the game).
 */

#import <UIKit/UIKit.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* main.c */
extern const char *nfsu2_cfg_get(const char *key);
extern int         nfsu2_cfg_set(const char *key, const char *val);
extern int         nfsu2_game_data_status(char *desc, size_t n);   /* 0 none 1 folder 2 image */
extern const char *nfsu2_build_label(void);

int ios_launcher_run(void);
int ios_point_res_wanted(char *desc, size_t n);

/* ── Settings table (a copy of the overlay panel's rows for these keys) ── */

enum { LK_HEAD, LK_SWITCH, LK_SLIDER, LK_SEG };

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
} LItem;

static const LItem s_litems[] = {
    { .kind = LK_HEAD, .title = "Display" },
    { .kind = LK_SEG, .title = "Render scale", .key = "RECOMP_GL_SCALE",
      .labels = "1x|1.5x|2x|3x", .values = "1|1.5|2|3", .def_val = "2" },
    { .kind = LK_SWITCH, .title = "Widescreen 16:9", .key = "RECOMP_WIDESCREEN",
      .on = "1", .off = "0", .def_on = 1 },
    { .kind = LK_SWITCH, .title = "60 fps races",
      .key = "RECOMP_PRESENT_INTERVAL", .on = "1", .off = "0", .def_on = 1 },
    { .kind = LK_SLIDER, .title = "Brightness", .key = "NFSU2_GAMMA",
      .lo = 0.5f, .hi = 2.0f, .def = 1.0f, .decimals = 2 },
    { .kind = LK_SWITCH, .title = "Car reflections", .key = "RECOMP_VK_CUBE",
      .on = "1", .off = "0", .def_on = 1 },
    { .kind = LK_SWITCH, .title = "Fill screen with movies", .key = "NFSU2_MOVIE_CROP",
      .on = "1", .off = "0", .def_on = 1 },
    { .kind = LK_SWITCH, .title = "Mipmaps", .key = "RECOMP_VK_MIPS",
      .on = "1", .off = "0", .def_on = 1 },
    { .kind = LK_SEG, .title = "Texture filtering", .key = "RECOMP_VK_ANISO",
      .labels = "Off|2x|4x|8x|16x", .values = "1|2|4|8|16", .def_val = "4" },

    { .kind = LK_HEAD, .title = "Audio" },
    { .kind = LK_SLIDER, .title = "Volume", .key = "RECOMP_AUDIO_VOLUME",
      .lo = 0.0f, .hi = 100.0f, .def = 100.0f, .decimals = 0 },
    /* on = "" leaves SDL's default category (Playback + MixWithOthers). */
    { .kind = LK_SWITCH, .title = "Mix with other apps' audio", .key = "SDL_AUDIO_CATEGORY",
      .on = "", .off = "playback", .def_on = 1 },

    { .kind = LK_HEAD, .title = "Controls" },
    { .kind = LK_SEG, .title = "Touch layout", .key = "NFSU2_TOUCH_LAYOUT",
      .labels = "Pad|Drive", .values = "pad|drive", .def_val = "pad" },

    { .kind = LK_HEAD, .title = "Launch" },
    { .kind = LK_SWITCH, .title = "Show this screen at launch", .key = "NFSU2_LAUNCHER",
      .on = "1", .off = "0", .def_on = 0 },
};
#define LITEM_COUNT ((int)(sizeof s_litems / sizeof s_litems[0]))

/* Switch state from the stored string; an odd value falls back on its first
 * character (0/n/f = off), then on the default (same rule as the panel). */
static int litem_is_on(const LItem *it)
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

static float litem_slider_value(const LItem *it)
{
    const char *v = nfsu2_cfg_get(it->key);
    float f = (v && *v) ? (float)atof(v) : it->def;
    return f < it->lo ? it->lo : f > it->hi ? it->hi : f;
}

static NSString *ln_ns(const char *c)
{
    return [NSString stringWithUTF8String:c ? c : ""];
}

static void ln_format_slider(const LItem *it, float v, char *buf, size_t n)
{
    snprintf(buf, n, "%.*f", it->decimals, (double)v);
}

/* ── Look ── */

static UIColor *ln_blue(void)  { return [UIColor colorWithRed:0.55 green:0.75 blue:1.0 alpha:1.0]; }
static UIColor *ln_green(void) { return [UIColor colorWithRed:0.35 green:0.85 blue:0.50 alpha:1.0]; }
static UIColor *ln_amber(void) { return [UIColor colorWithRed:1.0 green:0.72 blue:0.20 alpha:1.0]; }
static UIColor *ln_card(void)  { return [UIColor colorWithWhite:0.14 alpha:1.0]; }

static UILabel *ln_label(NSString *text, CGFloat size, UIColor *color, BOOL bold)
{
    UILabel *l = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 10, 10)];
    l.text = text;
    l.textColor = color;
    l.font = bold ? [UIFont boldSystemFontOfSize:size] : [UIFont systemFontOfSize:size];
    return l;
}

/* Wrapped label at (x, y) with width w; returns the y below it. */
static CGFloat ln_place(UIView *parent, UILabel *l, CGFloat x, CGFloat y, CGFloat w)
{
    l.numberOfLines = 0;
    CGSize s = [l sizeThatFits:CGSizeMake(w, CGFLOAT_MAX)];
    l.frame = CGRectMake(x, y, w, ceil(s.height));
    [parent addSubview:l];
    return y + ceil(s.height);
}

/* ── The view controller: also the target of every control ── */

#define LN_BAR_H 64.0
#define LN_MAX_W 560.0

@interface Nfsu2LauncherVC : UIViewController
@property (nonatomic, assign) BOOL played;
@property (nonatomic, assign) int dataKind;
@property (nonatomic, strong) NSString *dataText;
@property (nonatomic, strong) UIScrollView *scroll;
@property (nonatomic, strong) UIView *bar;
@property (nonatomic, strong) UIButton *playButton;
@property (nonatomic, assign) CGFloat builtWidth;
- (void)switchChanged:(UISwitch *)sw;
- (void)sliderMoved:(UISlider *)sl;
- (void)sliderDone:(UISlider *)sl;
- (void)segChanged:(UISegmentedControl *)sg;
- (void)playTapped;
- (void)buildContent:(CGFloat)w;
@end

static CGFloat ln_hairline(UIView *card, CGFloat cy, CGFloat cw)
{
    UIView *h = [[UIView alloc] initWithFrame:CGRectMake(14, cy, cw - 14, 0.5)];
    h.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.12];
    [card addSubview:h];
    return cy;
}

static CGFloat ln_switch_row(Nfsu2LauncherVC *vc, UIView *card, int idx, CGFloat cy, CGFloat cw)
{
    const LItem *it = &s_litems[idx];
    UILabel *l = ln_label(ln_ns(it->title), 15, [UIColor whiteColor], NO);
    l.frame = CGRectMake(14, cy, cw - 28 - 59, 48);
    l.adjustsFontSizeToFitWidth = YES;
    l.minimumScaleFactor = 0.75;
    [card addSubview:l];
    UISwitch *sw = [[UISwitch alloc] initWithFrame:CGRectMake(cw - 14 - 51, cy + 8.5, 51, 31)];
    sw.on = litem_is_on(it) ? YES : NO;
    sw.tag = idx + 1;
    sw.accessibilityLabel = ln_ns(it->title);
    [sw addTarget:vc action:@selector(switchChanged:) forControlEvents:UIControlEventValueChanged];
    [card addSubview:sw];
    return cy + 48;
}

static CGFloat ln_slider_row(Nfsu2LauncherVC *vc, UIView *card, int idx, CGFloat cy, CGFloat cw)
{
    const LItem *it = &s_litems[idx];
    char buf[24];
    float v = litem_slider_value(it);
    ln_format_slider(it, v, buf, sizeof buf);

    UILabel *l = ln_label(ln_ns(it->title), 15, [UIColor whiteColor], NO);
    l.frame = CGRectMake(14, cy + 8, cw - 28 - 70, 22);
    [card addSubview:l];
    UILabel *vl = ln_label(ln_ns(buf), 15, [UIColor lightGrayColor], NO);
    vl.frame = CGRectMake(cw - 14 - 70, cy + 8, 70, 22);
    vl.textAlignment = NSTextAlignmentRight;
    vl.tag = 1000 + idx;
    [card addSubview:vl];
    UISlider *sl = [[UISlider alloc] initWithFrame:CGRectMake(14, cy + 32, cw - 28, 44)];
    sl.minimumValue = it->lo;
    sl.maximumValue = it->hi;
    sl.value = v;
    sl.continuous = YES;
    sl.tag = idx + 1;
    sl.accessibilityLabel = ln_ns(it->title);
    [sl addTarget:vc action:@selector(sliderMoved:) forControlEvents:UIControlEventValueChanged];
    /* Save on release: one file write per drag, not per pixel. */
    [sl addTarget:vc action:@selector(sliderDone:)
        forControlEvents:(UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                          UIControlEventTouchCancel)];
    [card addSubview:sl];
    return cy + 80;
}

static CGFloat ln_seg_row(Nfsu2LauncherVC *vc, UIView *card, int idx, CGFloat cy, CGFloat cw)
{
    const LItem *it = &s_litems[idx];
    NSArray *labels = [ln_ns(it->labels) componentsSeparatedByString:@"|"];
    NSArray *vals = [ln_ns(it->values) componentsSeparatedByString:@"|"];
    NSString *cur = ln_ns(nfsu2_cfg_get(it->key));
    NSString *def = ln_ns(it->def_val);
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

    UILabel *l = ln_label(ln_ns(it->title), 15, [UIColor whiteColor], NO);
    l.frame = CGRectMake(14, cy + 8, cw - 28, 22);
    [card addSubview:l];
    UISegmentedControl *sg = [[UISegmentedControl alloc] initWithItems:labels];
    sg.frame = CGRectMake(14, cy + 34, cw - 28, 44);
    sg.selectedSegmentIndex = sel;
    sg.tag = idx + 1;
    sg.accessibilityLabel = ln_ns(it->title);
    [sg addTarget:vc action:@selector(segChanged:) forControlEvents:UIControlEventValueChanged];
    [card addSubview:sg];
    return cy + 90;
}

@implementation Nfsu2LauncherVC

- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return UIInterfaceOrientationMaskLandscape;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    char desc[1200];
    self.dataKind = nfsu2_game_data_status(desc, sizeof desc);
    self.dataText = ln_ns(desc);

    self.view.backgroundColor = [UIColor colorWithWhite:0.05 alpha:1.0];
    self.view.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;

    self.scroll = [[UIScrollView alloc] initWithFrame:CGRectZero];
    [self.view addSubview:self.scroll];

    self.bar = [[UIView alloc] initWithFrame:CGRectZero];
    self.bar.backgroundColor = [UIColor colorWithWhite:0.05 alpha:1.0];
    [self.view addSubview:self.bar];

    /* Always enabled: the game's own message explains missing data. */
    UIButton *play = [UIButton buttonWithType:UIButtonTypeSystem];
    [play setTitle:@"Play" forState:UIControlStateNormal];
    [play setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    play.titleLabel.font = [UIFont boldSystemFontOfSize:20];
    play.backgroundColor = [UIColor colorWithRed:0.0 green:0.48 blue:1.0 alpha:1.0];
    play.layer.cornerRadius = 14;
    play.accessibilityLabel = @"Play";
    [play addTarget:self action:@selector(playTapped)
        forControlEvents:UIControlEventTouchUpInside];
    [self.bar addSubview:play];
    self.playButton = play;
}

- (void)viewDidLayoutSubviews
{
    [super viewDidLayoutSubviews];
    CGRect b = self.view.bounds;
    UIEdgeInsets sa = self.view.safeAreaInsets;
    CGFloat bw = b.size.width - sa.left - sa.right;
    CGFloat barTop = b.size.height - sa.bottom - LN_BAR_H;
    CGFloat pw = bw - 32;

    if (bw < 100 || barTop <= sa.top)
        return;
    self.scroll.frame = CGRectMake(sa.left, sa.top, bw, barTop - sa.top);
    self.bar.frame = CGRectMake(sa.left, barTop, bw, LN_BAR_H);
    if (pw > LN_MAX_W)
        pw = LN_MAX_W;
    self.playButton.frame = CGRectMake(floor((bw - pw) / 2), 6, pw, 52);
    /* Rebuilt only when the width changes (rotation): layout runs often and
     * a rebuild would drop a touch in progress. */
    if (fabs(bw - self.builtWidth) > 0.5) {
        self.builtWidth = bw;
        [self buildContent:bw];
    }
}

- (void)buildContent:(CGFloat)w
{
    UIScrollView *sv = self.scroll;
    NSArray *old = [sv.subviews copy];
    for (UIView *v in old)
        [v removeFromSuperview];

    CGFloat cw = w - 32;
    if (cw > LN_MAX_W)
        cw = LN_MAX_W;
    CGFloat x0 = floor((w - cw) / 2);
    CGFloat y = 16;

    y = ln_place(sv, ln_label(@"Need for Speed: Underground 2", 26, [UIColor whiteColor], YES),
                 x0, y, cw);
    y += 2;
    y = ln_place(sv, ln_label([NSString stringWithFormat:@"Static recompilation port · build %s",
                                                         nfsu2_build_label()],
                              12, [UIColor lightGrayColor], NO),
                 x0, y, cw);
    y += 18;

    /* Game data card. */
    y = ln_place(sv, ln_label(@"GAME DATA", 12, ln_blue(), YES), x0 + 2, y, cw - 4);
    y += 6;
    {
        UIView *card = [[UIView alloc] initWithFrame:CGRectMake(x0, y, cw, 10)];
        card.backgroundColor = ln_card();
        card.layer.cornerRadius = 12;
        CGFloat cy = 12;
        if (self.dataKind != 0) {
            cy = ln_place(card, ln_label([@"✓ " stringByAppendingString:self.dataText],
                                         15, ln_green(), YES), 14, cy, cw - 28);
        } else {
            cy = ln_place(card, ln_label(@"⚠︎ No game data found", 15, ln_amber(), YES),
                          14, cy, cw - 28);
            cy += 6;
            cy = ln_place(card, ln_label(@"Copy NFSU2.iso (or the extracted disc folder) into "
                                         @"Files > On My iPhone/iPad > NFSU2, then reopen the app.",
                                         13, [UIColor lightGrayColor], NO), 14, cy, cw - 28);
        }
        cy += 12;
        card.frame = CGRectMake(x0, y, cw, cy);
        [sv addSubview:card];
        y += cy + 20;
    }

    /* Settings, one card per heading. */
    UIView *card = nil;
    CGFloat cardTop = 0, cy = 0;
    for (int i = 0; i < LITEM_COUNT; i++) {
        const LItem *it = &s_litems[i];
        if (it->kind == LK_HEAD) {
            if (card) {
                card.frame = CGRectMake(x0, cardTop, cw, cy);
                y = cardTop + cy + 20;
            }
            y = ln_place(sv, ln_label([ln_ns(it->title) uppercaseString], 12, ln_blue(), YES),
                         x0 + 2, y, cw - 4);
            y += 6;
            card = [[UIView alloc] initWithFrame:CGRectMake(x0, y, cw, 10)];
            card.backgroundColor = ln_card();
            card.layer.cornerRadius = 12;
            card.clipsToBounds = YES;
            [sv addSubview:card];
            cardTop = y;
            cy = 0;
            continue;
        }
        if (!card)
            continue;
        if (cy > 0)
            ln_hairline(card, cy, cw);
        if (it->kind == LK_SWITCH)
            cy = ln_switch_row(self, card, i, cy, cw);
        else if (it->kind == LK_SLIDER)
            cy = ln_slider_row(self, card, i, cy, cw);
        else
            cy = ln_seg_row(self, card, i, cy, cw);
    }
    if (card) {
        card.frame = CGRectMake(x0, cardTop, cw, cy);
        y = cardTop + cy + 20;
    }
    sv.contentSize = CGSizeMake(w, y + 8);
    sv.contentOffset = CGPointZero;
}

- (void)switchChanged:(UISwitch *)sw
{
    int idx = (int)sw.tag - 1;
    if (idx < 0 || idx >= LITEM_COUNT)
        return;
    const LItem *it = &s_litems[idx];
    nfsu2_cfg_set(it->key, sw.on ? it->on : it->off);
}

- (void)sliderMoved:(UISlider *)sl
{
    int idx = (int)sl.tag - 1;
    if (idx < 0 || idx >= LITEM_COUNT)
        return;
    const LItem *it = &s_litems[idx];
    char buf[24];
    ln_format_slider(it, sl.value, buf, sizeof buf);
    UILabel *vl = (UILabel *)[self.scroll viewWithTag:1000 + idx];
    if ([vl isKindOfClass:[UILabel class]])
        vl.text = ln_ns(buf);
    /* A change without a finger (VoiceOver adjust) has no touch-up: save now. */
    if (!sl.tracking)
        nfsu2_cfg_set(it->key, buf);
}

- (void)sliderDone:(UISlider *)sl
{
    int idx = (int)sl.tag - 1;
    if (idx < 0 || idx >= LITEM_COUNT)
        return;
    const LItem *it = &s_litems[idx];
    char buf[24];
    ln_format_slider(it, sl.value, buf, sizeof buf);
    nfsu2_cfg_set(it->key, buf);
}

- (void)segChanged:(UISegmentedControl *)sg
{
    int idx = (int)sg.tag - 1;
    if (idx < 0 || idx >= LITEM_COUNT)
        return;
    const LItem *it = &s_litems[idx];
    NSArray *vals = [ln_ns(it->values) componentsSeparatedByString:@"|"];
    NSInteger k = sg.selectedSegmentIndex;
    if (k < 0 || k >= (NSInteger)vals.count)
        return;
    nfsu2_cfg_set(it->key, [[vals objectAtIndex:(NSUInteger)k] UTF8String]);
}

/* Only sets the flag: ios_launcher_run tears the window down after the run
 * loop returns, never from inside this action. */
- (void)playTapped
{
    self.played = YES;
}

@end

/* ── Entry ── */

/* The scene to attach to: the first UIWindowScene that is in the foreground,
 * else the first one, else nil (the caller uses a plain frame window). */
static UIWindowScene *ln_scene(void)
{
    UIWindowScene *first = nil;
    for (UIScene *sc in [UIApplication sharedApplication].connectedScenes) {
        if (![sc isKindOfClass:[UIWindowScene class]])
            continue;
        if (!first)
            first = (UIWindowScene *)sc;
        if (sc.activationState == UISceneActivationStateForegroundActive ||
            sc.activationState == UISceneActivationStateForegroundInactive)
            return (UIWindowScene *)sc;
    }
    return first;
}

int ios_launcher_run(void)
{
    static int s_running;
    int result = 0;

    if (s_running || ![NSThread isMainThread]) {
        fprintf(stderr, "nfsu2: launcher: not on the main thread, skipped\n");
        return 0;
    }
    s_running = 1;
    @autoreleasepool {
        Nfsu2LauncherVC *vc = [[Nfsu2LauncherVC alloc] init];
        UIWindowScene *scene = ln_scene();
        UIWindow *win = nil;

        if (scene)
            win = [[UIWindow alloc] initWithWindowScene:scene];
        else
            win = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
        if (!vc || !win) {
            fprintf(stderr, "nfsu2: launcher: no window, skipped\n");
        } else {
            fprintf(stderr, "nfsu2: launcher: window on %s\n",
                    scene ? "a window scene" : "the main screen");
            win.windowLevel = UIWindowLevelNormal + 1;
            win.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
            win.backgroundColor = [UIColor colorWithWhite:0.05 alpha:1.0];
            win.rootViewController = vc;
            [win makeKeyAndVisible];

            /* Short slices; the loop watches the flag only (runMode: can
             * return early, e.g. when UIKit switches modes while scrolling).
             * NO means the mode had no source: sleep so this cannot spin. */
            while (!vc.played) {
                @autoreleasepool {
                    BOOL ran = [[NSRunLoop mainRunLoop]
                        runMode:NSDefaultRunLoopMode
                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
                    if (!ran) {
                        struct timespec ts = { 0, 10 * 1000 * 1000 };
                        nanosleep(&ts, NULL);
                    }
                }
            }
            result = 1;
            win.hidden = YES;
            win.rootViewController = nil;
        }
        win = nil;
        vc = nil;
    }
    s_running = 0;
    return result;
}

/* ── Drawable resolution (v29) ── */

/* Called once from main() (iOS, main thread, before any thread starts and
 * before SDL_CreateWindow): 1 = create the window WITHOUT
 * SDL_WINDOW_ALLOW_HIGHDPI, so SDL gives the CAMetalLayer contentsScale 1.0
 * and the swapchain is the size in points; 0 = native pixels.
 * NFSU2_POINT_RES: auto (default) | 1 forces points | 0 forces native pixels.
 * Auto picks points on a big iPad: [UIScreen mainScreen].bounds long side
 * >= 1280 pt and short side >= 720 pt (13" iPad Pro 1376x1032; the 11" 1194x834
 * and every iPhone stay native). The long and short side are used, not
 * width and height: the bounds follow the interface orientation. desc gets
 * the text for the log line, e.g. "852x393 pt, x3". */
int ios_point_res_wanted(char *desc, size_t n)
{
    const char *v = nfsu2_cfg_get("NFSU2_POINT_RES");
    UIScreen *scr = [UIScreen mainScreen];
    CGRect b = scr.bounds;
    CGFloat lw = b.size.width > b.size.height ? b.size.width : b.size.height;
    CGFloat sh = b.size.width > b.size.height ? b.size.height : b.size.width;
    int mode = -1;                  /* -1 auto, 0 native, 1 points */
    int point;

    if (v && (*v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T' ||
              strcasecmp(v, "on") == 0))
        mode = 1;
    else if (v && (*v == '0' || *v == 'n' || *v == 'N' || *v == 'f' || *v == 'F' ||
                   strcasecmp(v, "off") == 0))
        mode = 0;
    point = mode < 0 ? (lw >= 1280.0 && sh >= 720.0) : mode;
    if (desc && n) {
        if (point)
            snprintf(desc, n, "%gx%g pt, x1; NFSU2_POINT_RES=%s", (double)lw, (double)sh,
                     mode < 0 ? "auto" : "1");
        else
            snprintf(desc, n, "%gx%g pt, x%g%s", (double)lw, (double)sh,
                     (double)scr.nativeScale, mode == 0 ? "; NFSU2_POINT_RES=0" : "");
    }
    return point;
}
