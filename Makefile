# kttgm - "Kur tu teci, Gailīti manu?" NES ROM statically decompiled to C
# and rebuilt for x86-64/arm64 Linux and macOS.
#
#   make            - build ./kttgm          (SDL2 window + audio)
#   make headless   - build ./kttgm-headless (no SDL2; PPM/WAV dump mode)
#   make game.c     - regenerate the lifted C from kttgm.nes (needs python3)
#   make disasm     - full 6502 disassembly listing (kttgm.asm)
#
# macOS:  brew install sdl2        (then plain `make`)
# Linux:  apt install libsdl2-dev  (then plain `make`)
#
# SDL2 flags can be overridden:  make SDL_CFLAGS="..." SDL_LIBS="..."

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-label -Wno-unused-parameter \
            -Wno-unused-but-set-variable
LDFLAGS ?=

# ---- SDL2 discovery: sdl2-config (PATH or Homebrew), pkg-config, hints ----
SDL2_CONFIG := $(shell { command -v sdl2-config || \
                         command -v /opt/homebrew/bin/sdl2-config || \
                         command -v /usr/local/bin/sdl2-config; } 2>/dev/null)
ifeq ($(SDL2_CONFIG),)
SDL2_HINT := $(shell pkg-config --cflags sdl2 2>/dev/null)
ifeq ($(SDL2_HINT),)
SDL_CFLAGS = -I/opt/homebrew/include/SDL2 -I/usr/local/include/SDL2 -I/usr/include/SDL2
SDL_LIBS   = -L/opt/homebrew/lib -L/usr/local/lib -lSDL2
else
SDL_CFLAGS = $(SDL2_HINT)
SDL_LIBS   = $(shell pkg-config --libs sdl2 2>/dev/null)
endif
else
SDL_CFLAGS = $(shell $(SDL2_CONFIG) --cflags)
SDL_LIBS   = $(shell $(SDL2_CONFIG) --libs)
endif

# ---- build mode ----
MODE ?= sdl
ifeq ($(MODE),headless)
  OBJDIR := obj-headless
  BIN    := kttgm-headless
  MODE_CFLAGS := -DKTTGM_NO_SDL
  MODE_SDL_CFLAGS :=
  MODE_SDL_LIBS :=
else
  OBJDIR := obj-sdl
  BIN    := kttgm
  MODE_CFLAGS :=
  MODE_SDL_CFLAGS = $(SDL_CFLAGS)
  MODE_SDL_LIBS = $(SDL_LIBS)
endif

OBJS = $(OBJDIR)/main.o $(OBJDIR)/runtime.o $(OBJDIR)/apu.o $(OBJDIR)/game.o

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(MODE_SDL_LIBS)

$(OBJS): | $(OBJDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/main.o $(OBJDIR)/runtime.o $(OBJDIR)/apu.o: runtime.h apu.h
$(OBJDIR)/game.o: runtime.h

$(OBJDIR)/%.o: %.c
	$(CC) $(CFLAGS) $(MODE_CFLAGS) $(MODE_SDL_CFLAGS) -c -o $@ $<

headless:
	@$(MAKE) MODE=headless kttgm-headless

# regenerate the decompiled game logic from the ROM
game.c: lift.py dis6502.py kttgm.nes
	python3 lift.py kttgm.nes game.c

# full disassembly listing (reference/documentation)
disasm:
	python3 dis6502.py kttgm.nes kttgm.asm kttgm.json
	rm -f chr.bin

clean:
	rm -f kttgm kttgm-headless
	rm -rf obj-sdl obj-headless __pycache__

.PHONY: all clean headless disasm
