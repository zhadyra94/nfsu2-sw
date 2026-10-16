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

Not ported (NFSU2-only for now): frame
lag fence wrapper, native math leaves, culling, gamma probe, VP6 movies
(NFSU1 movies are EA `.mad`, decoded by the game's own lifted code), white
movie frames, Switch text patch, sim-step cap, Auto Pad/Drive state reader
addresses.

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
the player idle (~15 fps on lavapipe). Not tested: driving, finish,
results, career, iOS device.

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
