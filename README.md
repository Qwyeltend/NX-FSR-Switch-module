# NX-FSR

Per-game post-processing for retail Nintendo Switch games: **contrast adaptive sharpening** and
**anti-aliasing (FXAA)**, FSR 1.0 planned. Runs on the console itself, on the game's final frame,
without extra frame buffering.

> [!WARNING]
> **Experimental.** Early version, tested on few games so far.
> It may crash games. Keep a backup of your `SaltySD` folder.

## Features

- AMD FidelityFX CAS sharpening and FXAA anti-aliasing of the final frame, separately or together,
  in one pass (anti-aliasing on edges, sharpening elsewhere)
- Off by default: games that were never set up are not touched at all
- Settings per game (title ID), applied automatically when the game starts
- Tesla / Ultrahand overlay: change settings in game, live, saved per game
- Handles games whose frame is stored bottom-up (NVN lower-left origin), with a manual override
- No added input lag: the pass runs inside the same frame, right before present (it only costs GPU time)
- Split view for comparing (effect on the left half only)
- One build for all 64-bit NVN games, no per-game code

### Planned

- Higher quality anti-aliasing (SMAA) for games with enough free memory
- Optional per-game FSR 1.0 (EASU + RCAS) where the frame can be taken before the game's own upscale

## Requirements

- Switch with Atmosphère custom firmware
- [SaltyNX](https://github.com/masagrator/SaltyNX) 2.1.4 (included in the release package)

### Limitations

- 64-bit games only (SaltyNX limitation)
- Games rendering through NVN only (not OpenGL / Vulkan)
- The effect is applied after the game composes its UI, so the UI is processed too
- About 10 MB of memory is reserved at game start for the frame copy

## How it works

The effect is built into SaltyNX Core next to its logo renderer. SaltyNX already hooks NVN
(`nvnQueuePresentTexture`), so right before a frame is presented:

1. the game's queue is fenced and a separate NVN queue waits for it,
2. the frame is copied into a texture of ours (sharpening reads neighbouring pixels),
3. a full screen triangle with the sharpening shader is drawn back into the frame,
4. the game's queue waits for our queue and presents.

### Shaders without the official SDK

NVN shaders normally come from Nintendo's offline compiler. Here they are compiled with
[UAM](https://github.com/devkitPro/uam) (the open deko3d shader compiler) and converted to NVN's
format by `tools/dksh2nvn.py`.

The layout was worked out in `lab/` by compiling SaltyNX's logo shader with UAM and comparing it with
the NVN binary of the same GLSL: the code section matches (0x30 byte prefix, shader program header,
instructions, constant buffer aligned to 256 bytes) and the NVN control section carries the same
fields as deko3d's program header. Only two bindings differ:

| | UAM / deko3d | NVN |
|---|---|---|
| uniform block, binding 0 | `c2` | `c3` |
| fragment texture, binding 0 | handle `0x1A4` | handle `0x8` |

The build patches UAM for the texture slot, declares uniform blocks at binding 1 (= NVN binding 0),
and `tools/check_nvn_asm.py` disassembles every shader with envydis and fails the build if it uses
anything else.

## Installation

1. Back up the `SaltySD` folder on your SD card.
2. Download the latest package and copy its contents to the root of the SD card.
3. Reboot.

## Configuration

Open the overlay (Tesla / Ultrahand) while a game is running:

| Setting | |
|---|---|
| Effect | on / off for this game |
| Sharpness | 0 – 100 %, 0 = off |
| Anti-aliasing | 0 – 100 %, 0 = off |
| Compare | effect on the left half only, line in the middle |
| Picture flip | Auto / Off / On, for games where the picture ends up upside down |

Changes apply immediately and are saved to `sdmc:/config/fsrSwitch/<TITLEID>.ini`, which is read
at the next start of that game. The file can also be edited by hand:

```ini
enabled=1        # 0 = off for this game
sharpness=0.50   # 0.0 (off) .. 1.0 (strong)
antialias=0.50   # 0.0 (off) .. 1.0 (strong)
split=0          # 1 = left half only (comparison)
flip=auto        # auto / on / off
```

Everything is off by default. The frame copy memory (about 10 MB) is reserved at game start only
when the effect is enabled for that game, so the first time you turn it on in the overlay the game
has to be restarted once. After that it can be changed freely while playing.
To turn the effect off for all games, create an empty `sdmc:/config/fsrSwitch/disabled.flag`.

A start-up log is written per game to `sdmc:/config/fsrSwitch/log_<TITLEID>.txt`. Frame size,
frame origin and GPU time of the pass are shown live in the overlay (SaltyNX Core can only access
the SD card while the game is starting).

## Building

Everything is built by GitHub Actions (`.github/workflows/build.yml`):

1. **shaders** — builds UAM and envydis, compiles `shaders/`, converts to NVN, checks bindings
2. **core** — SaltyNX 2.1.4 Core with the module applied by `tools/apply_patch.py` (devkitA64)
3. **overlay** — Tesla overlay (libtesla)
4. **package** — the official SaltyNX 2.1.4 release with the Core replaced, plus the overlay

```
shaders/        GLSL sources (full screen vertex shader, CAS fragment shader)
core/           FsrNVN module (added to SaltyNX Core)
overlay/        Tesla / Ultrahand overlay
common/         shared memory block used by Core and the overlay
tools/          UAM -> NVN converter, binding checker, SaltyNX patcher
lab/            research: UAM vs NVN shader comparison
package/        files added to the release package
```

## Credits

- [masagrator](https://github.com/masagrator) — SaltyNX, whose NVN hooks and logo renderer this builds on;
  FPSLocker, whose SaltyNX IPC client and libtesla copy the overlay uses
- [WerWolv](https://github.com/WerWolv/libtesla) — libtesla
- [fincs / devkitPro](https://github.com/devkitPro/uam) — UAM shader compiler
- [envytools](https://github.com/envytools/envytools) — envydis Maxwell disassembler
- AMD — FidelityFX Contrast Adaptive Sharpening (MIT)

## License

GPLv2, see [LICENSE](LICENSE). The overlay uses libtesla (GPLv2). The shader is based on AMD FidelityFX CAS (MIT).

## Disclaimer

Not affiliated with Nintendo, NVIDIA or AMD. Code injection into games can get you banned
from online services: don't use it in online games.
