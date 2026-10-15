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

Not ported (NFSU2-only for now): 60 fps time floor and swap interval, frame
lag fence wrapper, native math leaves, culling, gamma probe, VP6 movies
(NFSU1 movies are EA `.mad`, decoded by the game's own lifted code), white
movie frames, Switch text patch, sim-step cap, Auto Pad/Drive state reader
addresses.

## Defaults

`RECOMP_WIDESCREEN=0` (4:3) and `NFSU2_MOVIE_CROP=0` until checked for this
title. iOS app: bundle `me.madiyar.nfsu1`, name NFSU1, Files folder NFSU1.

## Status (2026-10-15)

Linux Vulkan on lavapipe: intro movies, title, main menu, Quick Race car
select, loading, race start with HUD and lap timer; stable for 500 s with
the player idle (~15 fps on lavapipe). Not tested: driving, finish,
results, career, iOS device.

Linux harness: `tools/run_headless.sh` with the NFSU1 game folder. The
intro movies do not skip with Start; the title screen comes at ~290 s.
