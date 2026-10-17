# NFSU1 (Xbox Need for Speed: Underground) on the NFSU2 shell

Branch `nfsu1`, forked from the NFSU2 iOS branch at v29 (`37ee3ed`). The
runtime, renderer and iOS shell are NFSU2's; only the title-specific parts
changed. `CLAUDE.md` still describes NFSU2.

## Disc and lift

- NTSC-U disc, `default.xbe` title ID 0x45410047, version 1, SHA-256
  `cdd562ff…d878a`, entry point 0x00164356, XDK 5558, no XNET/XONLINE.
- Lift: `config-u1/lift_u1.sh` (disasm with `config-u1/seed_functions.json`,
  func_id, abi_analysis, recomp with `--exclude-manual src/recomp_manual.c`
  and `--mmio-sections DSOUND,XPP`). 12904 functions, 52 files.
- `tools/disasm/functions.py`: an immediate in a library section (D3D,
  DSOUND, XPP) no longer starts a function in `.text`. D3D's NV2A register
  constants equal some `.text` addresses; one cut `sub_001009C0` (a jump
  table switch) in three, and its stubbed tail left esi/edi corrupt: a crash
  at the first track load.
- Five seeds (ICALL targets after `ret`/`ret 8` or padding); see the file.
- Three more seeds (u1-4, lift release `u1-gen3`): 0xC3F4D, 0xC3F92,
  0xC3F9A. `sub_000C3EA0` (a front-end message handler) is cut at a false
  function start (0xC3F1C); the code after that piece's `ret` belonged to no
  function, so the lifter stubbed its three jump targets
  (`recomp_stubs_unresolved.c`). Opening Options > EA Trax sent a message down
  the 0xC3F4D branch: the stub only moved esp, the handler returned with esi
  and edi wrong, and the listener loop in `sub_000DA280` (list at 0x2BCB98)
  read a garbage slot: crash at guest VA 0xE796FF7C. The seeds add exactly
  three functions; every other function body is unchanged.
- 77 stubs remain. Most are data decoded as code (mid-instruction, padding,
  jump tables, addresses outside the image); a few are real code. A
  `-DRECOMP_ABI_CHECK` build logs any lifted call that returns with
  ebx/esi/edi or esp wrong (`[ABI]` lines). Touring the front end with it
  (every main-menu entry, all Options submenus but Trailers, Customize Ride,
  car and track select) showed only boot-time CRT/SEH helpers and the EA
  Trax path. Build: a second build dir with `-DCMAKE_C_FLAGS=-DRECOMP_ABI_CHECK`
  (gen at -O1 builds in about a minute).

## Overrides (`src/recomp_manual.c`)

NFSU2 twins found by masked-opcode match:

| NFSU1 | NFSU2 | What |
|---|---|---|
| 0x1B3640 | 0x2A7EE0, 0x2A9450 | CRT memmove (one copy in NFSU1) |
| 0x1E4CA1 | 0x32EB65 | DirectSound DSP command post |
| 0x1EA5B2 | 0x33518D | AC'97 channel reset (table 0x1EB200) |
| 0x1D9BB0 | 0x2F1D80 | D3D vblank handler at DISPATCH |
| 0x1DA120 | 0x2F22F0 | D3D PGRAPH handler at DISPATCH |
| 0x1D0340 | (0x2E8F20) | BlockOnTime: hands the executor the GPU semaphore (device at [0x1DE9E8], +0x30). Found by structure; without it boot hung in BlockUntilIdle |
| 0x1CF380, 0x1CF570 | 0x2EB4B0, 0x2EB6A0 | Swap wrappers: `RECOMP_PRESENT_INTERVAL=1` rewrites D3D's present interval at 0x1DE94C (2 or 0x80000002 -> every vblank). The game keeps 0x80000002 in menus and races |
| 0x1410D0 | (0x1890C0) | Main-loop time step. Under interval 1 hands the 1/60 dt floor's excess back through the remainder at 0x2CA430 (`RECOMP_DT_FLOOR=game` turns it off; `RECOMP_DT_LOG=1` logs per second) |

| 0x1D0340 (fence wait) | (0x2E8F20) | Since u1-4 its poll loop sleeps on the executor's semaphore writes instead of spinning (`RECOMP_FENCE_SLEEP=0` spins; kernel_bridge.c, same change as NFSU2 v31). Linux race: no change in fps or race clock |

Not ported (NFSU2-only for now): frame
lag fence wrapper, native math leaves, culling, gamma probe, VP6 movies
(NFSU1 movies are EA `.mad`, decoded by the game's own lifted code), white
movie frames, Switch text patch, sim-step cap.

## Defaults

`RECOMP_WIDESCREEN=0` (4:3) and `NFSU2_MOVIE_CROP=0` until checked for this
title. iOS app: bundle `me.madiyar.nfsu1`, name NFSU1, Files folder NFSU1.

App icon (u1-2): the NFSU logo from Wikimedia Commons
(`https://upload.wikimedia.org/wikipedia/commons/f/fd/NFSU.svg`, white page
rectangle made transparent, rendered with headless Chrome at 2100x600),
on a dark teal gradient (the script gives the letters a silver gradient):
`config-u1/make_icon.py LOGO.png . 0.92 18,44,40 4,9,9` writes
`ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png` and `ios/icon/*`.

## Status (2026-10-15)

Linux Vulkan on lavapipe: intro movies, title, main menu, Quick Race car
select, loading, race start with HUD and lap timer; stable for 500 s with
the player idle (~15 fps on lavapipe). Driving, finish, results, pause menu
(Resume/Restart/Quit), Career start, save and load were swept on Linux with
no crash (see Auto Pad/Drive state below). Not tested: iOS device.

Linux harness: `tools/run_headless.sh` with the NFSU1 game folder. The
intro movies do not skip with Start; the title screen comes at ~290 s.

## 60 fps

NFSU1's step differs from NFSU2: dt (rdtsc, 733.33 MHz) x1.0167, clamped to
[1/60, 3/60] by float literals (0x1F8190, 3.0 at 0x25314C), then N = max(1,
int(acc*60)) ticks of 1/60; the tick sum at 0x253144 is the game clock (the lap
timer follows it). No cutscene floor, no 1/30 variable. Below 20 fps the 3/60
cap slows the game clock (lavapipe races at 11-18 fps run at 0.6-0.85 of real
time, with or without this change). `RECOMP_DT_CAP=8` (test only) raises it.
Widescreen: the title never queries the video flags and renders 4:3, so
`RECOMP_WIDESCREEN=1` only stretches the 4:3 picture to 16:9: leave it 0.

## Yellow square after loading

The title fills freed heap blocks with 0xEE (`sub_00124310`) while the
executor can still draw the last frame that used them; DXT 0xEEEE is a flat
tan. `nv2a_vk.c` keeps the GPU copy of a cached texture whose level 0 starts
with that fill (`RECOMP_VK_FREED_GUARD`, default on). On Linux it fires at
menu -> loading and loading -> race. Same guard as NFSU2 v30.

## Auto Pad/Drive state words (`src/nfsu2_state.c`)

Not NFSU2's addresses. Gameflow state 0x283434 (3 = front end, 6 = RACING;
GameFlowBeginRacing, sub_0004F130, writes 6 through the manager at 0x283418).
The race controller (RC) object pointer is the word 0x265B18 (0 outside a
race, a 0x81xxxxxx contiguous-window address in a race); its first word is the
RCSTATE: 1 loading, 3 prerace/countdown, 4 race in progress (driving; in Drag
the RC is already 4 while the car waits), 7 pause, 8 postrace (results and the
Restart/Statistics/Leave menu). Measured on
Linux (menu -> loading -> countdown -> drive -> pause -> resume -> results ->
Restart -> Leave). The reader returns drive only for gameflow 6 and RCSTATE 4.
