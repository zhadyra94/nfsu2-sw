/* state_test.c -- nfsu2_in_race() on a fake guest memory, and the debounce
 * of nfsu2_auto_layout_step(). Built and run by run_state_test.sh. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nfsu2_state.h"

ptrdiff_t g_xbox_mem_offset;

static int fails, checks;
static uint8_t *mem;

#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put(uint32_t va, uint32_t v) { memcpy(mem + va, &v, 4); }
static void set(uint32_t gf, uint32_t mode, uint32_t flag)
{
    put(0x428E34, gf);
    put(0x3E5A64, mode);
    put(0x3FBCFC, flag);
}

int main(void)
{
    int st, i;
    unsigned a, b, c;

    mem = calloc(1, 0x430000);
    if (!mem) return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;

    set(3, 0, 1);  CHECK(nfsu2_in_race() == 0);   /* menus */
    set(1, 0, 1);  CHECK(nfsu2_in_race() == 0);   /* loading frontend */
    set(4, 0, 1);  CHECK(nfsu2_in_race() == 0);   /* loading region */
    set(5, 0, 1);  CHECK(nfsu2_in_race() == 0);   /* loading track */
    set(7, 4, 0);  CHECK(nfsu2_in_race() == 0);   /* unloading: gf wins */
    set(9, 0, 0);  CHECK(nfsu2_in_race() == 0);
    set(0, 0, 0);  CHECK(nfsu2_in_race() == 0);
    set(6, 3, 0);  CHECK(nfsu2_in_race() == -1);  /* flyby */
    set(6, 4, 0);  CHECK(nfsu2_in_race() == 1);   /* driving */
    set(6, 6, 1);  CHECK(nfsu2_in_race() == 0);   /* paused */
    set(6, 4, 1);  CHECK(nfsu2_in_race() == -1);  /* mixed words */
    set(6, 6, 0);  CHECK(nfsu2_in_race() == -1);
    set(6, 0, 1);  CHECK(nfsu2_in_race() == -1);
    set(77, 4, 0); CHECK(nfsu2_in_race() == -1);  /* garbage */
    set(10, 0, 1); CHECK(nfsu2_in_race() == -1);

    set(6, 4, 0);
    CHECK(nfsu2_state_words(&a, &b, &c) == 1 && a == 6 && b == 4 && c == 0);
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

    /* A whole session: menus, loading, flyby, drive, pause, resume. */
    {
        static const struct { uint32_t gf, m, f; int want; } seq[] = {
            {3,0,1,-1}, {3,0,1, 0}, {4,0,1, 0}, {5,0,1, 0},
            {6,3,0,-1}, {6,3,0,-1}, {6,4,0,-1}, {6,4,0, 1}, {6,4,0, 1},
            {6,6,1,-1}, {6,6,1, 0}, {6,4,0,-1}, {6,4,0, 1},
        };
        st = 0;
        for (i = 0; i < (int)(sizeof seq / sizeof seq[0]); i++) {
            set(seq[i].gf, seq[i].m, seq[i].f);
            CHECK(nfsu2_auto_layout_step(&st, nfsu2_in_race()) == seq[i].want);
        }
    }

    free(mem);
    printf("state_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
