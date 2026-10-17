/* state_test.c -- nfsu2_in_race() (NFSU1 words: gameflow 0x283434, race
 * controller pointer 0x265B18 -> RCSTATE) on a fake guest memory that spans
 * the contiguous window (0x80000000..), and the debounce of
 * nfsu2_auto_layout_step(). Built and run by run_state_test.sh. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "nfsu2_state.h"

ptrdiff_t g_xbox_mem_offset;

static int fails, checks;
static uint8_t *mem;

#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define VA_GF  0x283434u
#define VA_PTR 0x265B18u
#define RC_OBJ 0x81B10E40u          /* a contiguous-window address, as in the game */

static void put(uint32_t va, uint32_t v) { memcpy(mem + va, &v, 4); }
/* gameflow word, RC pointer, RCSTATE stored at [ptr] (when ptr is mapped) */
static void setp(uint32_t gf, uint32_t ptr, uint32_t rc)
{
    put(VA_GF, gf);
    put(VA_PTR, ptr);
    if (ptr == RC_OBJ)
        put(ptr, rc);
}
static void set(uint32_t gf, uint32_t rc) { setp(gf, RC_OBJ, rc); }

int main(void)
{
    int st, i;
    unsigned a, b, c;

    mem = mmap(NULL, 0x84000000u, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;

    /* Outside RACING the gameflow word alone decides: 0, whatever the RC. */
    set(3, 0);   CHECK(nfsu2_in_race() == 0);   /* menus */
    set(1, 0);   CHECK(nfsu2_in_race() == 0);   /* loading frontend */
    set(4, 0);   CHECK(nfsu2_in_race() == 0);   /* loading track common */
    set(5, 0);   CHECK(nfsu2_in_race() == 0);   /* loading track specific */
    set(7, 4);   CHECK(nfsu2_in_race() == 0);   /* unloading: gf wins */
    set(9, 4);   CHECK(nfsu2_in_race() == 0);
    set(0, 4);   CHECK(nfsu2_in_race() == 0);
    setp(3, 0, 0);          CHECK(nfsu2_in_race() == 0);   /* menus, no RC object */
    setp(3, 0xDEADBEEFu, 0); CHECK(nfsu2_in_race() == 0);  /* stale pointer ignored */

    /* RACING: the RCSTATE decides. */
    set(6, 1);   CHECK(nfsu2_in_race() == -1);  /* LOADING */
    set(6, 2);   CHECK(nfsu2_in_race() == -1);  /* CAMERA_FLYBY */
    set(6, 3);   CHECK(nfsu2_in_race() == -1);  /* PRERACE / countdown */
    set(6, 4);   CHECK(nfsu2_in_race() == 1);   /* RACE_IN_PROGRESS: driving */
    set(6, 7);   CHECK(nfsu2_in_race() == 0);   /* PAUSE */
    set(6, 8);   CHECK(nfsu2_in_race() == 0);   /* POSTRACE: results */
    set(6, 0);   CHECK(nfsu2_in_race() == -1);  /* NONE */
    set(6, 5);   CHECK(nfsu2_in_race() == -1);  /* INSTANT_REPLAY */
    set(6, 6);   CHECK(nfsu2_in_race() == -1);  /* AUTO_REPLAY */
    set(6, 0xFFFFFFFFu); CHECK(nfsu2_in_race() == -1);  /* garbage RCSTATE */
    set(6, 0x1234);      CHECK(nfsu2_in_race() == -1);

    /* Pointer safety: never dereferenced unless inside the two RAM windows. */
    setp(6, 0, 4);           CHECK(nfsu2_in_race() == -1);  /* no object yet */
    setp(6, 0xDEADBEEFu, 4); CHECK(nfsu2_in_race() == -1);  /* outside every window */
    setp(6, 0xFFFFFFFCu, 4); CHECK(nfsu2_in_race() == -1);
    setp(6, 0x84000000u, 4); CHECK(nfsu2_in_race() == -1);  /* just past the window */
    setp(6, 0x80000000u, 4); CHECK(nfsu2_in_race() == -1);  /* null page of the window */
    setp(6, 0x00000800u, 4); CHECK(nfsu2_in_race() == -1);  /* low page */

    set(77, 4);  CHECK(nfsu2_in_race() == -1);  /* garbage gameflow */
    set(10, 7);  CHECK(nfsu2_in_race() == -1);

    set(6, 4);
    CHECK(nfsu2_state_words(&a, &b, &c) == 1 && a == 6 && b == 4 && c == 1);
    setp(6, 0, 0);
    CHECK(nfsu2_state_words(&a, &b, &c) == 1 && a == 6 && b == 0xFFFFFFFFu && c == 0);
    CHECK(nfsu2_state_words(NULL, NULL, NULL) == 1);

    g_xbox_mem_offset = 0;
    CHECK(nfsu2_in_race() == -1);                 /* no guest memory */
    CHECK(nfsu2_state_words(&a, &b, &c) == 0 && a == 0 && b == 0 && c == 0);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;

    /* Debounce: same 0/1 answer twice in a row. */
    st = 0;
    CHECK(nfsu2_auto_layout_step(&st, 1) == -1);  /* first sight: wait */
    CHECK(nfsu2_auto_layout_step(&st, 1) == 1);   /* second: Drive */
    CHECK(nfsu2_auto_layout_step(&st, 1) == 1);   /* steady: repeats */
    CHECK(nfsu2_auto_layout_step(&st, 0) == -1);  /* flip: wait again */
    CHECK(nfsu2_auto_layout_step(&st, 0) == 0);   /* Pad */
    CHECK(nfsu2_auto_layout_step(&st, -1) == -1); /* unknown: no switch */
    CHECK(nfsu2_auto_layout_step(&st, 0) == -1);  /* -1 reset the streak */
    CHECK(nfsu2_auto_layout_step(&st, 0) == 0);
    CHECK(nfsu2_auto_layout_step(&st, 1) == -1);  /* single blip */
    CHECK(nfsu2_auto_layout_step(&st, 0) == -1);  /* blip gone: restarts */
    CHECK(nfsu2_auto_layout_step(&st, 0) == 0);
    CHECK(nfsu2_auto_layout_step(&st, 1) == -1);
    CHECK(nfsu2_auto_layout_step(&st, -1) == -1);
    CHECK(nfsu2_auto_layout_step(&st, 1) == -1);  /* 1,-1,1 is not 2 in a row */
    CHECK(nfsu2_auto_layout_step(&st, 1) == 1);
    CHECK(nfsu2_auto_layout_step(&st, 7) == -1);  /* junk is unknown (1st) */
    st = 0;
    for (i = 0; i < 5; i++)
        CHECK(nfsu2_auto_layout_step(&st, -1) == -1);

    /* Sustained unknown falls back to Pad on the 6th, and stays Pad. */
    st = 0;
    nfsu2_auto_layout_step(&st, 1);
    CHECK(nfsu2_auto_layout_step(&st, 1) == 1);            /* driving */
    for (i = 0; i < 5; i++)
        CHECK(nfsu2_auto_layout_step(&st, -1) == -1);      /* 5 unknown: no switch */
    CHECK(nfsu2_auto_layout_step(&st, -1) == 0);           /* 6th: Pad */
    CHECK(nfsu2_auto_layout_step(&st, 99) == 0);           /* junk counts, stays Pad */
    for (i = 0; i < 20; i++)
        CHECK(nfsu2_auto_layout_step(&st, -1) == 0);
    CHECK(nfsu2_auto_layout_step(&st, 1) == -1);           /* back to driving: debounce */
    CHECK(nfsu2_auto_layout_step(&st, 1) == 1);

    /* An unknown run broken by a valid reading restarts its count. */
    st = 0;
    for (i = 0; i < 5; i++)
        CHECK(nfsu2_auto_layout_step(&st, -1) == -1);
    CHECK(nfsu2_auto_layout_step(&st, 1) == -1);           /* resets the counter */
    for (i = 0; i < 5; i++)
        CHECK(nfsu2_auto_layout_step(&st, -1) == -1);      /* only 5 again */
    CHECK(nfsu2_auto_layout_step(&st, -1) == 0);

    /* Flyby after loading (Pad up): ~12 unknown polls stay Pad (no Drive
     * ever), then two 1s switch to Drive. The caller ignores 0 while on Pad. */
    st = 0;
    CHECK(nfsu2_auto_layout_step(&st, 0) == -1);
    CHECK(nfsu2_auto_layout_step(&st, 0) == 0);
    for (i = 0; i < 12; i++)
        CHECK(nfsu2_auto_layout_step(&st, -1) != 1);
    CHECK(nfsu2_auto_layout_step(&st, 1) == -1);
    CHECK(nfsu2_auto_layout_step(&st, 1) == 1);

    /* A whole session: menus, loading, countdown, drive, pause, resume. */
    {
        static const struct { uint32_t gf, rc; int want; } seq[] = {
            {3,0,-1}, {3,0, 0}, {4,0, 0}, {5,0, 0},
            {6,1,-1}, {6,3,-1}, {6,3,-1}, {6,4,-1}, {6,4, 1}, {6,4, 1},
            {6,7,-1}, {6,7, 0}, {6,4,-1}, {6,4, 1}, {6,8,-1}, {6,8, 0}, {6,1,-1}, {3,0,-1}, {3,0, 0},
        };
        st = 0;
        for (i = 0; i < (int)(sizeof seq / sizeof seq[0]); i++) {
            set(seq[i].gf, seq[i].rc);
            CHECK(nfsu2_auto_layout_step(&st, nfsu2_in_race()) == seq[i].want);
        }
    }

    munmap(mem, 0x84000000u);
    printf("state_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
