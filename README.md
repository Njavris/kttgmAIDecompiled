# kttgm — "Kur tu teci, Gailīti manu?" decompiled from NES to C

Everything needed to decompile and rebuild the game lives in this
directory. The NES ROM `kttgm.nes` (Snauts' Latvian rooster platformer,
[itch.io page](https://snauts.itch.io/kur-tu-teci-gailti-manu)) is
statically decompiled into compilable C and rebuilt for x86-64/arm64
Linux and macOS.

The 6502 machine code is **not** interpreted at runtime. The lifter
(`lift.py`, driven by `dis6502.py`) translates every instruction of the
ROM's PRG bank into one C statement, groups basic blocks into C
functions by control-flow structure (SCCs + DAG merging), lowers JSR
to calls / RTS to return / branches to `goto`, and folds the hardware
spin loops into runtime hooks. The output (`game.c`) is plain C that
the compiler turns into native code. This approach is called
**static recompilation**.

Only the NES **hardware** (not the CPU) is modelled at runtime, since
the game's logic is meaningless without its memory-mapped registers:
`runtime.c` implements the bus (RAM, PPU registers with loopy scroll,
OAM DMA, controller port, APU register image), `apu.c` synthesises the
2A03 audio, and `main.c` is the SDL2 frontend.

## Build

**macOS** (Apple Silicon or Intel):

```
brew install sdl2
make
```

**Linux** (Debian/Ubuntu):

```
sudo apt install libsdl2-dev build-essential
make
```

Produces `./kttgm`. To override the SDL2 detection:
`make SDL_CFLAGS="..." SDL_LIBS="..."`.

Without SDL2 (PPM/WAV dump mode only):

```
make headless
```

## Run

```
./kttgm                          # window, keyboard
```

Controls:

| key | NES |
|-----|-----|
| arrows | d-pad |
| Z | A |
| X | B |
| Enter | Start |
| Tab | Select |
| Esc | quit |

Start (or A) begins the level from the intro screen; Start pauses
during play.

## Headless / verification mode

```
./kttgm --headless N [prefix]              # run N frames, dump frame PPMs
./kttgm --headless N --input "F:MASK,..."  # scripted button presses
KTTGM_WAV=out.wav ./kttgm --headless N     # also write audio to a WAV
KTTGM_TRACE=1 ./kttgm --headless N         # RAM/state trace to stderr
```

Button mask bits: `A B Select Start Up Down Left Right` (bit 0 = A).

Example: press Start at frame 120, release at 140, press A at 300,
release at 310, capture 600 frames and the audio:

```
KTTGM_WAV=music.wav ./kttgm --headless 600 \
    --input "120:0x08,140:0x00,300:0x01,310:0x00" f
```

## Regenerating the decompilation

```
make game.c      # re-run the lifter on kttgm.nes (needs python3)
make disasm      # write a full 6502 disassembly listing (kttgm.asm)
```

## Files

| file | what |
|------|------|
| `kttgm.nes` | the source ROM (input to the pipeline) |
| `dis6502.py` | 6502 disassembler + control-flow recovery |
| `lift.py` | the decompiler: ROM → C (static recompilation) |
| `game.c` | **lifted game logic** — generated, one C stmt per 6502 instruction |
| `runtime.h` / `runtime.c` | CPU helpers, NES bus, PPU, controller, frame hooks |
| `apu.h` / `apu.c` | 2A03 APU (2 pulse + noise + DMC) |
| `main.c` | SDL2 window + keyboard, or headless PPM/WAV dumper |
| `Makefile` | build; SDL2 autodetection for Linux + macOS |

## Caveats

* Time advances at frame granularity: the game only synchronises to the
  hardware at its vblank / frame-counter spin loops (verified in the
  ROM's control flow), and this game uses no mid-frame raster tricks.
* The APU is synthesised per frame — accurate for this game's
  frame-driven music engine, not cycle-exact.
