# Building psp-pmdvis from Source

## Prerequisites

### pspdev SDK

Install the [pspdev](https://github.com/pspdev/pspdev) toolchain.
The default install path is `/usr/local/pspdev`.

```bash
# Ubuntu / WSL
sudo apt-get install build-essential cmake
# Follow https://github.com/pspdev/pspdev for full installation
```

After installation, ensure the following are set:

```bash
export PSPDEV=/usr/local/pspdev
export PATH=$PSPDEV/bin:$PATH
```

### intraFont

PMDVIS uses [intraFont](https://github.com/pspdev/intraFont) for text rendering.
This is typically included with the pspdev SDK.

### Tiny-ME (Media Engine)

The `tiny-me/` directory contains [psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core) (MECC) by m-c/d.
It is bundled in the repository — no separate setup needed.

The pre-built `tiny-me/build/libme-core.a` and `tiny-me/build/kernel/kcall.prx`
are included. If you need to rebuild:

```bash
cd tiny-me
mkdir -p build && cd build
cmake .. && make
```

## Directory Structure

```
psp-pmdvis/
  main.c              — Main source (single-file)
  Makefile             — Build configuration
  ICON0.PNG            — EBOOT icon
  deploy.sh            — Auto-deploy to PSP via PowerShell
  pmdmini/             — PMD sound engine (ymfm fork)
    src/
      pmdwin/          — PMDWin core
      ymfm/            — YM2608 FM synthesis (ymfm + OPNA wrapper)
  tiny-me/             — Media Engine custom core (MECC)
    build/
      libme-core.a     — Pre-built ME library
      kernel/
        kcall.prx      — Kernel call module
  songs/               — PMD (.M) files
  docs/                — Documentation
```

## Build

```bash
cd psp-pmdvis
make
```

This will:
1. Compile all source files with `psp-gcc` / `psp-g++` (O3 optimization)
2. Link against pspdev libraries and `libme-core.a`
3. Generate `EBOOT.PBP` with the custom icon
4. Run `deploy.sh` to copy to connected PSP (optional, fails gracefully if no PSP)

### Build flags

| Flag | Purpose |
|------|---------|
| `-O3` | Maximum optimization |
| `-G0` | No GP-relative addressing (required for large BSS) |
| `-DPSP` | PSP platform define |
| `-fno-pic` | No position-independent code |
| `-funroll-loops` | Loop unrolling for ME performance |
| `-fno-exceptions -fno-rtti` | C++ size reduction (pmdmini) |
| `-fpermissive` | Allow legacy C++ in pmdmini |

### Clean build

```bash
make clean && make
```

## Deploy

### Automatic (via deploy.sh)

`deploy.sh` detects connected PSP drives (Windows/PowerShell) and copies
`EBOOT.PBP` automatically. It also creates timestamped backups.

### Manual

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/PMDVIS/` on your PSP's Memory Stick.

## Optional: YM2608 Rhythm ROM

Place `ym2608_adpcm_rom.bin` at `ms0:/PSP/GAME/PMDVIS/` to enable
rhythm channel playback. The build does not require this file.

## Firmware Compatibility

Built with `PSP_FW_VERSION = 660`. Tested on:
- 6.60 PRO-B10 (PSP-1000)
- ARK-4 CXPL (PSP Go)
