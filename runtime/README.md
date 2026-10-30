# Fury runtime

A small Win32 program that runs the statically recompiled `FURY.EXE`.

## Build

```
cd recomp\SR\SR-games\Fury\SR
python build_flat.py          # rebuilds fury-flat.bin from seg01.inc
cd runtime
build.cmd                     # NASM helper + MSVC runtime -> fury.exe
```

The game data (`PIC`, `DEC`, `SPR`, `DAT`, `SND`, `KMD`) has to be in this
directory. `fury.img` is the original image loaded at segment 0x0207.

## Run

```
run-level.cmd                 # window + automatic walk into Desert 1
furyrun.exe                   # the presentation, played normally
```

The window is a 3x view of the 320x200 screen. Keys are forwarded as real
scan codes to the game's own `int 9` handler: arrows move (Up jumps),
Space uses the character's ability (fireball / bite / bubble), Enter or Ctrl
also confirms and jumps, Escape leaves the intro or a menu.

## Environment switches

| Variable | Meaning |
| --- | --- |
| `FURY_AUTOSTART=1` | Escape at 2.5 s, then Space every 1.5 s, so the intro walks into the first level |
| `FURY_KEYS=ms:sc,ms:sc:h,ms:sc:dur,...` | scripted key presses; `h` holds the key without a break code, a numeric third field holds it for that many milliseconds, e.g. `4000:39:400`; with `FURY_AUTOSTART` the timestamps start when the level is reached |
| `FURY_OPL=name` | FM chip core: `dbopl` (the DOSBox wave-table core, default) or `nuked` (Nuked OPL3) |
| `FURY_NOGUI=1` | no window (screenshots still work) |
| `FURY_SHOTS=1` | write `shot_NNN.bmp` periodically |
| `FURY_SHOT_MS=n` | interval of those screenshots (default 700 ms) |
| `FURY_DUMP_AT=ms` | write `vga.bin` (256 KiB VGA window + registers) once, after the delay |
| `FURY_NOSOUND=1` | disable the Adlib/OPL2 output (enabled by default) |
| `FURY_VOLUME=n` | master volume in percent (default 100, allowed 0–200) |
| `FURY_SOUND_WAV=file` | also write the rendered stereo PCM at the output rate to a WAV file (debug) |
| `FURY_SOUNDLOG=1` | every ~5 s append the audio thread's health (writes/applied/queue depth) to `sound.log` |
| `FURY_WIDTH=n` | stereo widening in percent 0..100 (default 22; 0 = the original mono balance) |
| `FURY_SPK_ONLY=1` | render only the PC-speaker effects (debug, used to check the sample stream) |
| `FURY_KMD=1` | use the optional C KMD-player fallback instead of the recompiled player |
| `FURY_TP=x,y` | teleport the player (level pixels) once the level is up; used to test exits and level transitions |
| `FURY_DEBUG=1` | verbose logs, port tracing, thread sampling |
| `FURY_TICK_MS=n` | force the PIT period instead of the programmed divisor |
| `FURY_CONSOLE=1` | also accept keys from the console |

`fury.log` is truncated on every start.

## What the runtime emulates

* the CPU through the statically recompiled flat image;
* The FM stream is rendered by the DOSBox DBOPL wave-table core
  (`dbopl.cpp` / `dbopl.h`, GPL-2.1, standalone build) by default;
  `FURY_OPL=nuked` switches back to Nuked OPL3 (`opl3.c` / `opl3.h`,
  LGPL-2.1).  DBOPL is a mono OPL2 core, so its output lands in both
  half-sum lanes; a 1.35 factor keeps the average level matched with the
  Nuked reference so downstream dynamics do not re-calibrate.  `FURY_WIDTH`
  widening is frequency-dependent: only the signal above ~300 Hz gets the
  0.7 ms Haas tilt into the side channel, so the kick stays solidly centred;
  the side is tamed above ~9 kHz as well.  The
  game's own recompiled KMD player writes the OPL2 registers through ports
  `0x388/0x389`; the writes are queued and fed to the chip by the audio
  thread at 49.7 kHz.  Two recompiler fixes were needed for the player: the
  `x86_code_ptr` helper is stdcall (`retn 4`), so the extra `add esp,4`
  which flatify emitted after indirect jumps was removed.  The original
  music divider (`0xA0`, 50 Hz) is kept: the reference onset lags
  **0.597 / 1.195 / 1.792 s** match the Desert pattern.  The game's sound-enable
  flag (`155A:0171`) is kept set.  An optional C KMD-player fallback
  (`kmdplayer.c`, `FURY_KMD=1`) is included for comparison; the recompiled
  player is used by default.  Desert 1 uses the `DESERT.KMD` arrangement
  (installed as `kmd/desert2.kmd`); the original file is kept as
  `desert2.kmd.bak`;
* the digitised effects (the furry appearing, fireballs, pickups) are sent by
  the game as a 1-bit PC-speaker stream on port `0x61`; the runtime averages
  it over the ~5 kHz bit clock, filters it with a four-pole 4 kHz low-pass
  (the physical speaker response) and mixes it with the music.  The mix is
  rendered at the chip's native 49.7 kHz when the device accepts it (48 kHz
  fallback with cubic resampling).  The Adlib channels keep their original
  left/right placement; on top of that a mid/sure-inspired widener
  (`FURY_WIDTH`, default 22%) widens the side component with per-side 5% rolls
  at 110 Hz and 8 kHz plus a 0.7 ms Haas tilt, so the mix opens up without
  losing mono compatibility.  A two-stage dynamics chain - a 20 ms opto-ish
  master ride together with a 0.3 ms per-channel look-ahead limiter - keeps
  everything clipped-free and sits at a comfortable loudness; TPDF dithering
  before the final 16-bit quantisation removes quantisation noise.  The audio
  runs on a four-buffer queue with a time-critical thread to avoid underruns;
  the DSP chain sits on top of the chip core, so both emulator backends share
  one calibrated output stage; a 15 Hz DC blocker keeps the mix clean
  (`FURY_VOLUME` sets the master volume
  in percent); `FURY_NOSOUND=1` disables the audio output.  Register writes carry the
  sample they were made at and the chip advances one sample per rendered
  sample, so note onsets keep the exact timing of the original card
  instead of being quantised to whole output buffers (queue overflow is
  handled by releasing the oldest entry, never by dropping a new write);

* guest memory at `0x08000000`, the image at `0x08002070`, the recompiled code
  at `0x20000000`, a double-mapped 64 KiB guest stack;
* segment bases for DS/ES/SS, and a VGA helper for every ES access that
  applies the map mask, write mode (0/1/2/3), set/reset, rotate/ALU, bit mask
  and latches, and the read mode/read-map-select on loads;
* VGA mode 13h / 8-bit mode X and mode 0Dh (16 colours, four planes), CRTC
  start/offset, attribute pan, and the two palettes (DAC for 256 colours, the
  driver's `155A:017C` palette for the 16-colour levels);
* DOS file I/O, BIOS mode set and palette registers, timer/keyboard IRQs
  dispatched to the vectors the game installs.

## Known gaps

* other worlds use their own KMD modules; only the Desert 1 music has been
  tuned against the DOSBox reference so far;  the FM mix is equalised to the
  reference with a 110 Hz high-pass, a 150 Hz bass-body path (100 Hz peak),
  presence at 600/1000/1850/4500 Hz and a 4.2 kHz shelf, so the band
  response is within a few dB of the capture;
* no mouse (`int 33h`) and no joystick (the game port returns "no joystick");
* the level renders with the player sprite, animated objects and scrolling;
  the sprite blitters use `mov bp, sp` frames whose arguments are the words
  pushed immediately before the frame.  `flatify.py` therefore applies the
  wider-flat-return-address correction (+2) only to frames opened by
  `push bp` / `push ds` (arguments live above the return address); frames
  made of the pushed words themselves must not be shifted;
* far returns go through `flat_retf`, which translates a real-mode `IP:CS`
  frame pushed by hand; this covers the long-jump exits, but a path that
  computes a segment from data can still land outside the recompiled code;
* the `jmp word [cs:bx+…]` parameter tables are resolved through the symbol
  table, but a path that reaches an entry missing from `syms.inc` will jump
  into data.
