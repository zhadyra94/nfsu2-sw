/**
 * NFSU1 manual overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "platform/cpu_report.h"

/* The generated register model (g_eax/g_esp are thread-local there) and the
 * XBOX_PTR/MEM32 accessors; the register names below are its macros. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── CRT memmove/memcpy ───────────────────────────────────────
 *
 * NFSU1 links ONE copy of the MSVC CRT memmove (0x001B3640; memcpy is the
 * same body). Its tail and backward-copy paths dispatch through UnwindDown-
 * style vectors indexed with a *negative* register -- the table address in
 * the instruction is the end of the table -- which the lifter's forward
 * table reader cannot follow, so those jumps were lowered to indirect tail
 * calls into the middle of the function and failed to resolve: every copy
 * whose length was not a multiple of 4, and every overlapping backward copy,
 * silently lost its tail.
 *
 * Replaced by the host memmove. cdecl (dst, src, count), returns dst, and
 * esi/edi are preserved, as the original restores them. The generated body
 * is skipped by tools.recomp --exclude-manual reading this file. */
static void crt_memmove(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    eax = dst;
    esp += 4;   /* return address; cdecl, the caller pops the arguments */
}

void sub_001B3640(void) { crt_memmove(); }

/* ── DirectSound DSP command post (0x001E4CA1) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_001E4CA1_gen(void);

void sub_001E4CA1(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_001E4CA1_gen();
}

/* ── DirectSound AC'97 channel reset (0x001EA5B2) ────────────
 *
 * thiscall, ecx = channel object. Sets RR (bit 1) in the channel's NABM
 * control byte, then waits for the controller to clear it -- except MSVC
 * hoisted the load out of the loop, so the original reads the byte once and
 * spins on the register copy forever unless the bit is already clear at that
 * one read:
 *
 *     mov  byte [eax+0xFEC0010B], 2
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * On hardware the reset has completed by then. The Windows runtime answers it
 * with a write trap on the NABM page; there is no fault handling on POSIX or
 * Switch, so this is the same function with the reset completing at once.
 * Everything else -- the lock taken through sub_001E28B0/sub_001E28D2, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * sub_001EA3BC re-arm -- is as the original does it. */
void sub_001EA5B2(void)
{
    uint32_t ebp_saved = g_ebp, frame, chan, nabm;

    PUSH32(esp, ebp_saved);
    frame = esp;
    g_ebp = frame;
    g_seh_ebp = frame;
    esp -= 8;
    MEM32(frame - 4) = 0;
    PUSH32(esp, esi);
    esi = ecx;
    chan = esi;

    ecx = frame - 8;
    PUSH32(esp, 0x001EA5C6u); RECOMP_ABI_CALL(0x001E28B0u, sub_001E28B0);

    nabm = MEM32(MEM32(chan) * 4 + 0x1EB200u);
    MEM8(nabm + 0xFEC0010Bu) = 2;
    MEM8(nabm + 0xFEC0010Bu) &= (uint8_t)~2u;   /* reset complete */
    eax = nabm;
    SET_LO8(ecx, 0);

    MEM32(nabm + 0xFEC00100u) = MEM32(chan + 0x1C);
    if (MEM32(chan) == 1)
        MEM32(0xFEC0017Cu) = MEM32(chan + 0x28);

    PUSH32(esp, 1);
    PUSH32(esp, 1);
    ecx = chan;
    PUSH32(esp, MEM8(chan + 0x24));
    PUSH32(esp, MEM8(chan + 0x25));
    eax = MEM8(chan + 0x25);
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x001EA619u); RECOMP_ABI_CALL(0x001EA3BCu, sub_001EA3BC);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x001EA621u); RECOMP_ABI_CALL(0x001E28D2u, sub_001E28D2);

    POP32(esp, esi);
    esp = frame;
    POP32(esp, ebp_saved);
    g_ebp = ebp_saved;
    esp += 4;
}

/* ── D3D fence wait, BlockOnTime (0x001D0340) ────────────────
 *
 * Found by structure (no masked twin of NFSU2's sub_002E8F20: this is XDK
 * 5558's D3D). stdcall (fence, flags), ret 8. The device sits at 0x001DE9F0,
 * reached through the global at 0x001DE9E8:
 *   +0x2C  write sequence; sub_001D0280 (InsertFence) bumps it by 2 and posts
 *          BACK_END_WRITE_SEMAPHORE_RELEASE (method 0x1D70) carrying it
 *   +0x30  pointer to the semaphore word the GPU releases
 * and the wait at 0x001D0470 spins until *(+0x30) reaches the fence. Nothing
 * writes the word unless the pushbuffer executor knows its address, so the
 * first wait (BlockUntilIdle in the device bring-up, sub_001D0650) hung.
 * Hand the executor the semaphore once, then run the original wait. */
#define NFSU1_D3D_DEVICE_PTR 0x001DE9E8u
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_001D0340_gen(void);
extern void recomp_fence_wait_begin(uint32_t sem_va, uint32_t wseq_va, uint32_t fence);
extern void recomp_fence_wait_end(void);

void sub_001D0340(void)
{
    static uint32_t registered;
    uint32_t dev = MEM32(NFSU1_D3D_DEVICE_PTR);
    uint32_t sem = dev ? MEM32(dev + 0x30) : 0;

    if (sem && sem != registered) {
        nv2a_pb_set_semaphore_target(sem);
        fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
        registered = sem;
    }
    /* The wait's poll loop sleeps on the executor's semaphore writes
     * instead of spinning (kernel_bridge.c, RECOMP_FENCE_SLEEP=0 spins).
     * +0x2C is the write sequence the loop's test is relative to. */
    recomp_fence_wait_begin(sem, dev + 0x2C, MEM32(esp + 4));
    sub_001D0340_gen();
    recomp_fence_wait_end();
}

/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * Ported from NFSU2 (sub_002F1D80 / sub_002F22F0 are masked-opcode twins of
 * the NFSU1 sub_001D9BB0 / sub_001DA120). The text below is NFSU2's.
 *
 * D3D services the GPU from its DPC (NFSU2 0x00; the NFSU1 twin is not located) and, with the interrupt
 * masked, from its own busy-waits (BlockUntilIdle and friends) on the game
 * thread: both call the vblank handler (0x001D9BB0) and the PGRAPH handler
 * (0x001DA120), which queues flips. On the Xbox the two never interleave --
 * one is a DPC, the other runs with the interrupt off. Here the DPC runs on
 * the kernel timer thread, and lifted code hands over the guest lock at any
 * function entry, so the vblank count could move between D3D computing a
 * flip's target vblank and checking it. D3D retires a flip only when the
 * count *equals* its target, so that flip stayed pending forever and
 * PersistDisplay, which waits for pending flips, never returned: the hang
 * starting a Quick Race or Career. Run at DISPATCH_LEVEL (the dispatch lock,
 * kernel_hal.c), neither can be interrupted by the other.
 *
 * Raising blocks on the lock that the timer thread holds while it waits for
 * the guest lock, so the guest lock is let go for the raise, as a kernel
 * call would; already at DISPATCH (inside the DPC) there is nothing to take. */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);

static uint8_t d3d_isr_enter(void)
{
    uint8_t old = xbox_CurrentIrql();

    if (old < 2) {
        int d = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(d);
    }
    return old;
}

static void d3d_isr_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

/* Flips retired by a handler call: D3D's retire index, at +0x1BC of the
 * block both handlers take in ecx (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler, thiscall. It ends by writing PCRTC_INTR and spinning until
 * PMC_INTR bit 24 drops. Its callers (the DPC, D3D's busy-waits) have read
 * the bits before calling it, so this is the vblank being taken: clear them
 * now, and the spin ends at once instead of holding the guest lock until the
 * runtime's ack thread comes round. */
extern void sub_001D9BB0_gen(void);

void sub_001D9BB0(void)
{
    uint32_t blk = ecx;
    /* Read only once at DISPATCH: read before, a retire by the other
     * handler (DPC thread vs a D3D busy-wait) in between was counted by
     * both, the executor's flip index ran one off for good, and every
     * FLIP_STALL then waited out its 250 ms (Carbon, console, 3.7 fps). */
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);

    xbox_Nv2aVblankTaken();
    sub_001D9BB0_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip (the flip-queue routine). The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again (tens of thousands of flips a second, until the flip
 * targets were millions of vblanks ahead); guessed from the handler's
 * PGRAPH_FIFO writes, a trap posted during a call was sometimes dropped (the
 * fence event never set). So the executor posts traps under a lock held here
 * around the call, and a call that found one reports it taken, which clears
 * it (nv2a_pb_exec.c). */
extern void sub_001DA120_gen(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);

void sub_001DA120(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);     /* at DISPATCH, as above */
    uint32_t nsource;

    nv2a_pb_trap_lock();
    nsource = nv[0x400108 / 4];
    sub_001DA120_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    d3d_isr_leave(old);
}

/* ── Stubs for NFSU2 hooks that other files still reference ───
 *
 * NFSU1 has no gamma probe, so no ramp is ever captured (gen stays 0: the
 * Vulkan present path then applies none). movie_crop.c reads the last movie
 * name/row; nothing sets them here (NFSU2_MOVIE_CROP=0 is the default). */
uint8_t g_nfsu2_gamma[768];
uint32_t g_nfsu2_gamma_gen;
uint32_t nfsu2_movie_row;
char nfsu2_movie_name[64];

/* ── Swap present-interval forcing (0x001CF380 / 0x001CF570) ─
 *
 * NFSU2 twins (masked-opcode match): 0x002EB4B0 -> 0x001CF380, 0x002EB6A0 ->
 * 0x001CF570. Both read D3D's present interval at 0x1DE94C (NFSU2 0x2F76FC):
 * `cmp [0x1DE94C], 0x80000000` (D3D.asm 0x001CF43A, 0x001CF637) and
 * `mov edx, [0x1DE94C]` (0x001CF45B, 0x001CF511, 0x001CF5D5). D3D's own
 * setter (0x001D3B70..0x001D3BD1) stores 1/2/4 or 0x8000000n there; EA's
 * wrapper writes it from 0x0016D1BB and 0x0016DB08. The race cap is 2 (or
 * 0x80000002, every second vblank). Rewrite it to every-vblank before each
 * swap whenever RECOMP_PRESENT_INTERVAL=1 and leave it there: every reader
 * is inside the two swap functions. s_forced_from only feeds the log.
 *
 * Time step: NFSU1 differs from NFSU2. The main loop (sub_0004DF80, one
 * iteration per frame) measures dt (sub_00123280 clock, x0.001) and calls
 * sub_001410D0, which clamps it to [1/60, 3/60] (NTSC; [0.02, 0.06] when
 * sub_001408A0 returns 0, i.e. PAL) with the float constants at 0x1F8190,
 * 0x1F84A8 and [0x25314C] = 3.0. Those are .rdata constants shared with
 * other code, not a variable like NFSU2's 0x428EE8, so there is no floor
 * to rewrite. The clamped dt goes to [0x253164]; sub_00141200 turns it
 * into N = max(1, int(acc*60)) sim ticks of 1/60 (N at [0x253148], running
 * sum at [0x253144], the game clock: the lap timer follows it, measured
 * 310.57 s of lap time against 310.62 s of sum over the same 306.9 s).
 * Game time follows measured dt, so there is no frame-locked 30 Hz step to
 * break at 60 fps. But the 1/60 floor is a literal in the code. Present
 * intervals are a whole number of vblanks only on average: a frame that
 * takes 15 ms counts as 16.67 ms, and the clock runs fast (menus at 60
 * swaps/s on lavapipe: 1.07-1.13 against the game's own 1.0167). Fix, see
 * dt_step_wrap: hand the excess back through the game's own remainder. */
extern void sub_001CF380_gen(void);
extern void sub_001CF570_gen(void);
extern void sub_001410D0_gen(void);

#define PRESENT_SLOT    0x1DE94Cu
#define SIM_TICKS_SUM   0x253144u       /* running sum of N (sub_00141200) */

static int swap_interval_wanted(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_PRESENT_INTERVAL");
        v = (e && strcmp(e, "1") == 0);
        if (v)
            fprintf(stderr, "[FPS] RECOMP_PRESENT_INTERVAL=1: swap flips every vblank\n");
    }
    return v;
}

/* The value we replaced at the slot, so it can be put back. */
static int s_forced;
static uint32_t s_forced_from, s_forced_to;

static void force_interval(uint32_t slot)
{
    static uint32_t last;
    uint32_t cur = MEM32(slot);
    uint32_t fixed = cur;
    if (cur == 0x80000002u)
        fixed = 0x80000001u;
    else if (cur == 2u)
        fixed = 1u;
    if (fixed != cur) {
        MEM32(slot) = fixed;
        s_forced = 1;
        s_forced_from = cur;
        s_forced_to = fixed;
        if (cur != last) {
            last = cur;
            fprintf(stderr, "[FPS] swap interval 0x%08X -> every vblank\n", cur);
        }
    }
}

/* Undo force_interval where the game still holds what we wrote. A value the
 * game rewrote itself is left alone. */
static void unforce_interval(uint32_t slot)
{
    if (s_forced && MEM32(slot) == s_forced_to) {
        MEM32(slot) = s_forced_from;
        s_forced = 0;
    }
}

/* RECOMP_DT_FLOOR, only with RECOMP_PRESENT_INTERVAL=1:
 *   auto  (default) hand the floor's excess back, see dt_step_wrap
 *   game  leave the game's step alone (kill switch, the v24-like behaviour)
 * Unset or anything else is auto. NFSU2's zero, clamp and test30 do not
 * exist here: the floor is not a variable, and NFSU1's step has no
 * cutscene floor to tell apart. */
enum { DT_AUTO, DT_GAME };

static int dt_floor_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_DT_FLOOR");
        v = (e && strcmp(e, "game") == 0) ? DT_GAME : DT_AUTO;
    }
    return v;
}

static float dt_f32(uint32_t raw)
{
    float f;
    memcpy(&f, &raw, sizeof f);
    return f;
}

static uint32_t dt_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* Floor excess seen by sub_001410D0 since the last log line. */
static unsigned dt_frames, dt_floored;
static double dt_excess_s;

/* The main-loop time step, sub_001410D0 (one call per frame from 0x0004E03F,
 * cdecl, dt in seconds at [esp+4]). Normal path:
 *   raw = dt * 1.0167 + rem            rem = float[0x2CA430]
 *   t   = min(max(raw, floor), cap)    floor = 1/60 (NTSC, [0x25313C] != 0)
 *                                      or 0.02; cap = float[0x25314C] * floor
 *   rem = t - round(t*4000)/4000, float[0x253164] = quantized t
 * [0x2CA14C] != 0 is the replay branch; left alone.
 * A frame shorter than the floor is counted as a full floor, the clock gains
 * the difference. Writing the floor to 0 does not help: sub_00141200 then
 * takes N = max(1, int(acc*60)) ticks and an empty step still costs 1/60.
 * So let the floor stand and take the excess e = floor - raw back out of
 * rem. The next raw is e smaller and the game's own carry pays it off:
 * sum(ft) = 1.0167 * wall + rem_first - rem_last, exact. rem is held above
 * -3/60 so a stretch above 60 fps cannot owe more than 3 frames. */
void sub_001410D0(void)
{
    uint32_t arg = MEM32(esp + 4);
    uint32_t rem_in = MEM32(0x2CA430u);
    uint32_t replay = MEM32(0x2CA14Cu);
    float floor_s = MEM32(0x25313Cu) ? dt_f32(0x3C888889u) : dt_f32(0x3CA3D70Au);

    sub_001410D0_gen();
    if (!swap_interval_wanted() || replay)
        return;
    {
        float raw = dt_f32(arg) * dt_f32(0x3F82233Au) + dt_f32(rem_in);
        dt_frames++;
        if (raw < floor_s) {
            float e = floor_s - raw, rem = dt_f32(MEM32(0x2CA430u));
            dt_floored++;
            dt_excess_s += e;
            if (dt_floor_mode() == DT_AUTO) {
                rem -= e;
                if (rem < -3.0f / 60.0f)
                    rem = -3.0f / 60.0f;
                MEM32(0x2CA430u) = dt_bits(rem);
            }
        }
    }
}

/* Called from both swap wrappers when RECOMP_PRESENT_INTERVAL=1, before the
 * generated swap. Forces the interval and logs. RECOMP_DT_LOG=1 prints a
 * line per second: sim_s is the game's own tick sum (1/60 s per tick), fl
 * the steps whose dt the floor raised, ex_ms the time it added. */
static void swap_pre(uint32_t slot)
{
    static int logon = -1, started;
    static uint32_t last_slot = 0xFFFFFFFFu, last_sum;
    static unsigned swaps;
    static double last_s;

    if (!started) {
        started = 1;
        fprintf(stderr, "[dt] 60 fps time floor: %s (RECOMP_DT_FLOOR)\n",
                dt_floor_mode() == DT_GAME ? "game" : "auto");
    }
    if (logon < 0) {
        const char *e = getenv("RECOMP_DT_LOG");
        logon = (e && strcmp(e, "1") == 0);
    }
    {
        uint32_t cur = (s_forced && MEM32(slot) == s_forced_to) ? s_forced_from : MEM32(slot);
        if (cur != last_slot) {
            fprintf(stderr, "[dt] game swap interval 0x%08X (was 0x%08X)\n", cur, last_slot);
            last_slot = cur;
        }
    }
    /* Hot or in Low Power Mode: leave the game's own interval (30 fps). */
    if (xbox_power_hot_cap())
        unforce_interval(slot);
    else
        force_interval(slot);

    /* Test knob, not a feature: the step clamps dt at cap/60 s, cap =
     * float[0x25314C] = 3.0 (read only by sub_001410D0). Below 20 fps the
     * game clock then runs slower than the wall clock, which lavapipe's
     * 12-18 fps in a race hits. RECOMP_DT_CAP=8 lets the clock keep up so
     * sim_s/wall can be measured there. Unset: the game's own 3.0. */
    {
        static int cap_init;
        if (!cap_init) {
            const char *e = getenv("RECOMP_DT_CAP");
            float c = e ? (float)atof(e) : 0.0f;
            cap_init = 1;
            if (c >= 1.0f && c <= 30.0f) {
                uint32_t bits;
                memcpy(&bits, &c, sizeof bits);
                MEM32(0x25314Cu) = bits;
                fprintf(stderr, "[dt] RECOMP_DT_CAP=%g: dt clamp raised to %g/60 s (test only)\n",
                        (double)c, (double)c);
            }
        }
    }

    if (logon) {
        struct timespec ts;
        double now;
        uint32_t sum = MEM32(SIM_TICKS_SUM);
        swaps++;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        if (last_s == 0.0) {
            last_s = now;
            last_sum = sum;
        }
        if (now - last_s >= 1.0) {
            fprintf(stderr, "[dt] wall=%.3f swaps=%u steps=%u fl=%u ex_ms=%.1f sim_s=%.4f slot=0x%08X\n",
                    now - last_s, swaps, dt_frames, dt_floored, dt_excess_s * 1e3,
                    (double)(int32_t)(sum - last_sum) / 60.0, MEM32(slot));
            swaps = 0;
            dt_frames = dt_floored = 0;
            dt_excess_s = 0.0;
            last_sum = sum;
            last_s = now;
        }
    }
}

void sub_001CF380(void)
{
    if (swap_interval_wanted())
        swap_pre(PRESENT_SLOT);
    sub_001CF380_gen();
}

void sub_001CF570(void)
{
    if (swap_interval_wanted())
        swap_pre(PRESENT_SLOT);
    sub_001CF570_gen();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

#define UNIMPL_SITES 64

static struct { const char *text; uint32_t va; } s_ui_site[UNIMPL_SITES];
static int s_ui_n;                       /* sites in the table */
static int s_ui_lock;                    /* tiny spin lock: any guest thread */
static unsigned long long s_ui_repeat;   /* calls past a site's first print */
static unsigned long long s_ui_over;     /* calls from sites after the table filled */
static unsigned long long s_ui_reported; /* over-table count at the last summary */
static int s_ui_atexit;

/* One summary line, only when sites beyond the table were suppressed since
 * the last one (a hot site repeating is not news: it already printed). Called
 * at exit (NFSU2 also called it every 10 s from its swap wrappers). */
void recomp_unimpl_report(void)
{
    unsigned long long rep, over;

    while (__atomic_exchange_n(&s_ui_lock, 1, __ATOMIC_ACQUIRE))
        ;
    rep = s_ui_repeat;
    over = s_ui_over;
    if (over == s_ui_reported) {
        __atomic_store_n(&s_ui_lock, 0, __ATOMIC_RELEASE);
        return;
    }
    s_ui_reported = over;
    __atomic_store_n(&s_ui_lock, 0, __ATOMIC_RELEASE);
    fprintf(stderr, "[UNIMPL] %llu call(s) from sites beyond the %d-site table were "
                    "not printed (%llu repeat call(s) at the %d printed site(s))\n",
            over, UNIMPL_SITES, rep, s_ui_n);
    fflush(stderr);
}

void recomp_unimpl(const char *text, uint32_t va)
{
    static int trap_cached = -1;
    int i, fresh = 0;

    if (trap_cached < 0) {
        const char *trap = getenv("RECOMP_UNIMPL_TRAP");
        trap_cached = (trap && *trap && *trap != '0');
    }

    /* Each (text, va) site prints once. The table holds UNIMPL_SITES; past
     * that, calls are only counted. */
    while (__atomic_exchange_n(&s_ui_lock, 1, __ATOMIC_ACQUIRE))
        ;
    for (i = 0; i < s_ui_n; i++)
        if (s_ui_site[i].va == va && s_ui_site[i].text == text)
            break;
    if (i < s_ui_n) {
        s_ui_repeat++;
    } else if (s_ui_n < UNIMPL_SITES) {
        s_ui_site[s_ui_n].text = text;
        s_ui_site[s_ui_n].va = va;
        s_ui_n++;
        fresh = 1;
        if (!s_ui_atexit) {
            s_ui_atexit = 1;
            atexit(recomp_unimpl_report);
        }
    } else {
        s_ui_over++;
    }
    __atomic_store_n(&s_ui_lock, 0, __ATOMIC_RELEASE);

    if (fresh || trap_cached) {
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (trap_cached) abort();
}
