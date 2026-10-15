/*
 * ios_vblank.m -- display vsync timestamps for the guest vblank (v27).
 *
 * A CADisplayLink on its OWN thread with its own run loop (never the main
 * thread: UIKit stalls there must not move the samples). Each callback pushes
 * the link's vsync time to the kernel in the kernel's own clock.
 *
 * Clock pairing: kernel_vblank_clock_us() is CLOCK_MONOTONIC, which on Darwin
 * keeps counting while the device sleeps; CADisplayLink.timestamp and
 * CACurrentMediaTime() are mach_absolute_time based and do not. So every
 * callback reads both back to back and converts by the difference; the raw
 * link timestamp is never pushed.
 *
 * The link stops by itself in the background; the kernel's free-run fallback
 * covers that. NFSU2_VBLANK_LOCK=0 (checked in main.c and in the kernel)
 * switches the whole thing off.
 *
 * Self-contained on purpose: the CI fail-fast step compiles this file with
 * no -I flags.
 */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <stdint.h>
#include <stdio.h>

extern void kernel_vblank_link_sample(uint64_t host_us);
extern long long kernel_vblank_clock_us(void);

@interface NFSU2VBlankLink : NSObject
- (void)threadMain:(id)unused;
- (void)tick:(CADisplayLink *)link;
@end

static NFSU2VBlankLink *s_vb_link;
static NSThread *s_vb_thread;

@implementation NFSU2VBlankLink

- (void)tick:(CADisplayLink *)link
{
    /* Back to back: the pair is simultaneous to about a microsecond. */
    long long k_us = kernel_vblank_clock_us();
    double media = CACurrentMediaTime();
    double ts = link.timestamp;                 /* the vsync, in media time */
    long long vsync_us = k_us - (long long)((media - ts) * 1e6);

    if (vsync_us > 0)
        kernel_vblank_link_sample((uint64_t)vsync_us);
}

- (void)threadMain:(id)unused
{
    (void)unused;
    @autoreleasepool {
        NSRunLoop *rl = [NSRunLoop currentRunLoop];
        CADisplayLink *dl = [CADisplayLink displayLinkWithTarget:self
                                                        selector:@selector(tick:)];
        if (@available(iOS 15.0, *)) {
            dl.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
        } else {
            dl.preferredFramesPerSecond = 60;
        }
        [dl addToRunLoop:rl forMode:NSRunLoopCommonModes];
        fprintf(stderr, "[vblank] display link thread running (60 Hz requested)\n");
        for (;;) {
            @autoreleasepool {
                if (![rl runMode:NSDefaultRunLoopMode
                      beforeDate:[NSDate dateWithTimeIntervalSinceNow:1.0]])
                    [NSThread sleepForTimeInterval:0.05];   /* no source: no spin */
            }
        }
    }
}

@end

/* Called once from main.c's iOS path. Safe to call before the kernel's timer
 * thread runs: the kernel keeps the newest samples in a ring. */
void ios_vblank_link_start(void)
{
    if (s_vb_thread)
        return;
    s_vb_link = [[NFSU2VBlankLink alloc] init];
    s_vb_thread = [[NSThread alloc] initWithTarget:s_vb_link
                                          selector:@selector(threadMain:)
                                            object:nil];
    s_vb_thread.name = @"nfsu2-vblank-link";
    s_vb_thread.qualityOfService = NSQualityOfServiceUserInteractive;
    [s_vb_thread start];
}
