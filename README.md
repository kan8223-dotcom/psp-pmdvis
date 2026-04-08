# PSP PMD Visualizer (PMDVIS)

Real-time PC-98 FM sound (YM2608) playback on PSP, powered by the
Media Engine running at 333 MHz for FM synthesis while the main CPU
handles visualization.

## Features

- **PMD (.M) playback** via pmdmini (ymfm fork): FM 6ch + SSG 3ch + rhythm
- **Media Engine offload** — 333 MHz full-speed FM synthesis
- **Ring buffer** with 256 blocks for stable long playback
- **Piano roll / FMP-style visualizer** with per-channel status display
- **WAV export** with loop count and fadeout settings
- **Recursive Memory Stick scan** (SEARCH) — finds .M files anywhere on storage
- **Per-channel solo / mute** for all 11 parts (FM×6, SSG×3, ADPCM, Rhythm)
- **Auto region detection** for ○/× button layout
- **Loop cache** — 60-second PCM pre-decode for gapless playback

## Confirmed Models

| Model | Firmware |
|-------|----------|
| PSP-1000 | 6.60 PRO-B10 |
| PSP Go (N1000) | ARK-4 CXPL |

## Requirements

- PSP with Custom Firmware (6.60 PRO-B10, ARK-4, or equivalent)
- Memory Stick with enough free space for PMD files
- (Optional) YM2608 ADPCM rhythm ROM for rhythm channel playback

## Installation

1. Copy `EBOOT.PBP` to `ms0:/PSP/GAME/PMDVIS/`
2. (Optional) Place your YM2608 ADPCM ROM at
   `ms0:/PSP/GAME/PMDVIS/ym2608_adpcm_rom.bin`
   Without this file, rhythm channels are silent but the player works normally.
3. Place your PMD (.M) files anywhere on the Memory Stick.
   PMDVIS will scan recursively, or you can put them directly in
   `ms0:/PSP/GAME/PMDVIS/songs/`

## YM2608 ROM Notice

The YM2608 ADPCM rhythm ROM is copyrighted by Yamaha Corporation and is
**NOT included** in this project. Users must provide their own copy from
legitimate sources. PMDVIS works without it — rhythm channels will simply
be silent.

## Controls

### List Screen

| Button | Function |
|--------|----------|
| ○ | Play selected song |
| × | Exit application |
| ↑↓ | Move cursor |
| L / R | Page up / down |
| □ | WAV export settings |
| △ | Delete song (system dialog confirmation) |
| START | ME ON/OFF toggle (warning dialog) |
| SELECT | Search storage for .M files |

### Play Screen

| Button | Function |
|--------|----------|
| ○ | Pause / resume |
| × | Return to list |
| ←→ | Switch visualizer (Keyboard ⇔ FMP) |
| ↑↓ | Move solo cursor |
| △ | Toggle track mute |
| □ | Restore all tracks (unmute) |
| L / R | Next / previous song |
| L+R | Save screenshot |

### WAV Export Settings

| Button | Function |
|--------|----------|
| △ | Toggle loop count (1 ⇔ 2) |
| □ | Toggle fadeout ON/OFF |
| ○ | Start export |
| × | Cancel |

## Building from Source

See [docs/BUILD.md](docs/BUILD.md).

## License

GPL-3.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE) for third-party
component attributions.

## Credits

- **pmdmini / PMD sound engine**:
  - M.Kajihara (PMD original author, PC-98)
  - C60 (PMDWin Windows port — pmdwincore, pmdwin, opnaw, p86drv, ppsdrv, table, util, sjis2utf)
  - UKKY (PPZ8 PCM driver)
  - BouKiCHi (pmdmini library wrapper)
- **ymfm**: Aaron Giles (YM2608 FM/SSG synthesis, BSD-3-Clause)
- **psp-media-engine-custom-core (MECC)**: m-c/d (mcidclan)
- **pspdev community**: pspsdk and the toolchain
- **z2442, m-c/d, Nuclear Kommando**: community support

## Author

Tanaka ([@kan8223-dotcom](https://github.com/kan8223-dotcom))
