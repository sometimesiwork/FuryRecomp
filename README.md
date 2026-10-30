# Fury of the Furries — static recompilation (Windows native)

A statically recompiled build of **Fury of the Furries** (1992, Kalisto) that
runs natively on Windows: the real-mode x86 code of `FURY.EXE` was
transcribed into protected-mode assembly once, at build time, and the
resulting flat image runs under a small Win32 runtime that emulates just the
machine the game was written for — VGA mode 13h/mode X, DOS/BIOS calls, the
8254 timer, the keyboard, Adlib FM music and the one-bit PC-speaker effects.

No emulator box: the game's own code runs on your CPU, full native speed.

```
┌────────────┐  flatify.py   ┌─────────────────┐   NASM    ┌──────────────┐
│ seg01.inc  │──────────────▶│  fury-flat.bin   │──────────▶│  runtime     │
│ (SR output)│  seg01_flat.inc│  0x20000000     │           │  fury.exe    │
└────────────┘               └─────────────────┘           └──────────────┘
```

This repository contains **only the programs** — recompiler pipeline and
runtime — none of the game's original data files.  Details below.

## Repository layout

```
sr/                  the recompiler pipeline (Python + NASM)
  seg01.inc          transcribed code of FURY.EXE (SR static recomp output)
  flatify.py         rewriter: real-mode -> flat addresses, far-call fixes
  build_flat.py      orchestrates flatify + NASM, widens short jumps
  repair_short_jumps.py
  fury-flat.asm      top-level NASM file (header, includes, symbol table)
  macros_flat.inc    shared flat-mode macros (far-call/ret, helper calls)
  syms.inc           guest linear address -> flat label table
  code_entries.sci   code entry points (SR audit info)
  noret_instructions.sci
  SR.cfg             SR configuration (real mode, load segment 0x0207)

runtime/             the Win32 host
  fury.c             the emulated machine: guest memory, VGA (mode 13h /
                     mode X, planar registers, DAC/palettes), DOS/BIOS,
                     PIT + keyboard IRQs, window/keyboard/scaling,
                     all debug helpers
  sound.c            audio output: timestamped register-write queue, the FM
                     core glue, PC-speaker decoding, the DSP/output chain
  dbopl.cpp/dbopl.h  DOSBox DBOPL OPL2/OPL3 core (default FM backend)
  opl3.c/opl3.h      Nuked OPL3 (alternative core, FURY_OPL=nuked)
  kmdplayer.c/.h     optional C KMD music fallback (FURY_KMD=1)
  x86_helpers.asm    small NASM helpers for far-return/int-call glue
  build.cmd          MSVC + NASM build for the runtime
  measure_wav.py     WAV nap-shot meter (levels, side-band split)
  run-level.cmd      launcher with the auto-walk-to-Desert-1 helper
  README.md          full documentation, with a change log and known gaps
```

## Building

Two build steps: the guest flat binary, then the Win32 runtime.

### Requirements

* Python 3 (any recent version)
* NASM 2.x
* MSVC (any recent `cl` with the Win32 SDK; `vcvars32.bat` in your path)

The build scripts locate NASM in two steps: the `NASM` environment
variable if set, else `%TEMP%\opencode\nasm\nasm-2.16.03\nasm.exe`.
Set `NASM` to your local path to override.

### Guest image

```
cd sr
python build_flat.py        # writes fury-flat.bin + syms.inc
```

`build_flat.py` calls `flatify.py` to rewrite `seg01.inc` into
`seg01_flat.inc`, assembles `fury-flat.asm` with NASM into the flat 32-bit
image, and widens out-of-range short jumps automatically until the
assembler is happy.  The result is `fury-flat.bin` at `\org 0x20000000`,
with a header pointing at the guest entry point and the symbol table used
for indirect jumps and interrupt vectors.

### Runtime

```
cd runtime
build.cmd                   # cl fury.c sound.c opl3.c kmdplayer.c dbopl.cpp
                            # + nasm x86_helpers.asm -> fury.exe
```

## Running

You need the original `FURY OF THE FURRIES` data files.  Copy the data
directories into `runtime/`:

```
PIC/  DEC/  SPR/  dat/  snd/  KMD/
```

(`fury.img`, the 577 KB real-mode image the runtime loads at segment
`0x0207`, is packed out of the original `FURY.EXE`.)

After building, copy the flat binary from `sr/` into `runtime/`:

```
Copy-Item sr\fury-flat.bin runtime\
```

The runtime requires `fury-flat.bin` in its own directory — it is not copied
automatically by the build scripts.

Then:

```
runtime\fury.exe            # windowed game
runtime\run-level.cmd       # same, and automatically walks the
                            # presentation into Desert 1
```

The window is a 3x view of the 320x200 screen.  Keys are forwarded as real
scan codes to the game's own `int 9` handler: arrows move (Up jumps),
Space uses the character's ability (fireball / bite / bubble), Enter or Ctrl
also confirms and jumps, Escape leaves the intro or a menu.

## Sound architecture (the interesting part)

* the recompiled KMD player writes OPL2 registers through ports `0x388/0x389`
  exactly as the original did; the writes carry the guest sample clock and
  are applied by the audio thread at the **sample they were made**, so note
  onsets keep the exact timing of the original card (a 15 Hz DC blocker and a
  four-buffer queue with a time-critical thread keep it glitch-free);
* the FM core is the DOSBox **DBOPL** wave-table emulator by default
  (`FURY_OPL=nuked` switches to Nuked OPL3); a 1.35 factor puts the mono
  OPL2 core's level at the reference set by the panned Nuked pair;
* the digitised effects arrive as a 1-bit PC-speaker stream on port `0x61`;
  the runtime averages it over the ~5 kHz bit clock and filters it with a
  four-pole 4 kHz low-pass that models the physical speaker;
* everything meets in one calibrated output stage: a ~20 ms level rider, a
  0.3 ms look-ahead peak limiter (never clipping, ceiling 30600), per-band
  stereo widening (only the signal above ~300 Hz gets the 0.7 ms Haas tilt,
  keeping the kick centred), an authentic tilt shelf and presence peak that
  match the DOSBox reference balance, plus TPDF dithering before the
  final 16-bit quantisation.

Useful switches (all optional): `FURY_VOLUME=0..200`, `FURY_WIDTH=0..100`,
`FURY_OPL=dbopl|nuked`, `FURY_MONO=1`, `FURY_NOSOUND=1`,
`FURY_SOUND_WAV=file` (WAV tap), `FURY_SOUNDLOG=1` (audio-thread health).
See `runtime/README.md` for the complete list, plus test/debug switches
(`FURY_AUTOSTART`, `FURY_KEYS`, `FURY_SPK_ONLY`, `FURY_OPLTEST`, ...).

## Accuracy notes

* the guest runs at the flat code base `0x20000000`; guest memory is mapped
  at `0x08000000`, the image at `0x08002070`;
* every ES VGA access goes through a helper applying the map mask, write
  mode 0–3, set/reset, rotate/ALU, bit mask, latches, and the read
  mode/read-map select;
* `mov bp, sp` frames whose arguments are the words pushed immediately
  before the frame get the flat-return-address correction (+2) applied
  selectively by `flatify.py` — see the notes in `runtime/README.md`;
* the long-jump exits use a `flat_retf` far-return path; a path that
  computes a real-mode segment from data can still land outside the flat
  image and should be re-added to `syms.inc` if you hit one.

## Legal

* This repository contains **no original game data**: no FURY.EXE, no
  PIC/DEC/SPR/dat/snd/KMD assets, no built guest binary.  To play, you need
  a legal copy of Fury of the Furries and its data files.
* The transcribed code (`sr/seg01.inc`) is a functional transformation of
  the original executable and is provided for interoperability and
  preservation purposes; it remains © Kalisto.
* [DBOPL](https://github.com/dosbox-staging/dosbox-staging) — © The DOSBox
  Team, GPL-2.0.
* [Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3) — © Alexey Khokholov,
  LGPL-2.1.
* A small C KMD music player fallback lives in `runtime/kmdplayer.c`.

## Status

Desert 1 is tuned end-to-end: audio, level rendering, sprite blitters,
scrolling and the recompiled KMD music all play against a DOSBox reference.
Other worlds are expected to work but have their own KMD modules and were
not individually tuned.  Known gaps: no mouse (`int 33h`), no joystick, and
the long-jump paths rely on `syms.inc` being complete (see
`runtime/README.md` → *Known gaps*).
