# GB-Slingshot
#   make            build build/slingshot.gb (GBDK-2020 via lcc, or Docker if lcc isn't installed)
#   make test       host-side tests for src/sim.h (no GBDK needed)
#   make check-asm  scan the SDCC assembly for the cached-accumulator miscompile
#   make check-levels   brute-force that every level is winnable in its shot budget
#   make emu-test   replay the golden game through the ROM in a headless emulator (pip install pyboy pillow)
#   make sgb-test   check the Super Game Boy border transfer reaches the SGB intact
#   make docker-image   build the GBDK-2020 toolchain image used when lcc isn't on PATH
#   make clean

PYTHON       ?= python3
HOST_CC      ?= gcc
MGBA         ?= mgba
DOCKER_IMAGE ?= gb-slingshot-gbdk

ROM      := build/slingshot.gb
GEN      := src/tables.h src/gfx.h src/gfx.c src/levels.h
# Kept apart from GEN because png2asset needs the GBDK install, where the other
# generators need only Python: the host-side targets (make test) take GEN, and
# only the ROMs take this too.
BORDER   := art/border_sgb.png src/border_data.c src/border_data.h

ifeq ($(shell command -v lcc 2>/dev/null),)
LCC  = docker run --rm -v "$(CURDIR)":/work -w /work $(DOCKER_IMAGE) lcc
P2A  = docker run --rm -v "$(CURDIR)":/work -w /work $(DOCKER_IMAGE) png2asset
else
LCC  = lcc
P2A  = png2asset
endif

.PHONY: all test check-asm check-levels emu-test sgb-test run clean docker-image gen

all: $(ROM)

gen: $(GEN) $(BORDER)

src/tables.h: tools/mktables.py
	$(PYTHON) tools/mktables.py src/tables.h

src/gfx.h src/gfx.c: tools/mkgfx.py
	$(PYTHON) tools/mkgfx.py src

src/levels.h: tools/mklevels.py $(wildcard levels/*.txt)
	$(PYTHON) tools/mklevels.py src

# The Super Game Boy border: art/border_sgb.png (256x224, transparent over the
# 160x144 game screen) -> border_data.c/.h. -pack_mode sgb is what gets the SGB
# layout -- 4bpp tiles, a 256x224 map, one attribute byte per cell -- instead of
# a Game Boy screen. mkborder.py imports the font out of mkgfx.py, so it is a
# prerequisite too. png2asset needs the GBDK install, so unlike the Python
# steps above it follows LCC into the container when lcc isn't here.
art/border_sgb.png: tools/mkborder.py tools/mkgfx.py
	$(PYTHON) tools/mkborder.py art/border_sgb.png

# -keep_palette_order makes png2asset take the palettes from the PNG, which is
# indexed, rather than packing them itself. It has to: png2asset reserves slot 0
# of an SGB palette (the backdrop colour, so nothing drawn there is visible)
# only for palettes whose tiles contain a transparent pixel, and this border's
# second palette never does. mkborder.py reserves the slot for every palette.
src/border_data.c src/border_data.h: art/border_sgb.png
	$(P2A) art/border_sgb.png -map -bpp 4 -max_palettes 4 -keep_palette_order \
	       -pack_mode sgb -use_map_attributes -c src/border_data.c

# Cart type 0x03 = MBC1 + RAM + battery, so the star ratings survive a reset.
# -Wm-ys sets the header's SGB flag (0x146 = 0x03), without which an SGB treats
# the cart as an ordinary Game Boy and shows no border at all. makebin's old
# licensee byte defaults to 0x33 alongside it, which is what the flag needs to
# mean anything.
$(ROM): src/main.c src/sim.h src/sgb_border.c src/sgb_border.h $(GEN) $(BORDER)
	@mkdir -p build
	$(LCC) -Wa-l -Wl-m -Wl-j -Wm-ys -Wm-yn"SLINGSHOT" -Wm-yt0x03 -o $(ROM) \
	       src/main.c src/gfx.c src/sgb_border.c src/border_data.c

build/test_sim: tests/test_sim.c src/sim.h $(GEN)
	@mkdir -p build
	$(HOST_CC) -std=c99 -O1 -Wall -Wextra -Wno-unused-function \
	           -fsanitize=undefined,implicit-conversion -fno-sanitize-recover=all \
	           -Isrc tests/test_sim.c -o $@ -lm

test: build/test_sim
	./build/test_sim

check-asm:
	@mkdir -p build
	$(LCC) -S -o build/main.asm src/main.c
	$(LCC) -S -o build/gfx.asm src/gfx.c
	$(LCC) -S -o build/sgb_border.asm src/sgb_border.c
	@$(PYTHON) tools/check_asm.py --self-test
	@$(PYTHON) tools/check_asm.py build/main.asm build/gfx.asm build/sgb_border.asm

build/check_levels: tools/check_levels.c src/sim.h src/levels.h src/tables.h
	@mkdir -p build
	$(HOST_CC) -std=c99 -O1 -Wall -Wextra \
	           -fsanitize=undefined,implicit-conversion -fno-sanitize-recover=all \
	           -Isrc tools/check_levels.c -o $@ -lm

check-levels: build/check_levels
	./build/check_levels

build/sim_offsets.json: tests/sim_offsets_probe.c src/sim.h src/levels.h src/tables.h
	@mkdir -p build
	$(HOST_CC) -std=c99 -Wall -Wno-unused-function -Isrc tests/sim_offsets_probe.c -o build/sim_offsets_probe
	./build/sim_offsets_probe > $@

emu-test: check-asm $(ROM) build/sim_offsets.json
	$(PYTHON) tests/emu_replay.py $(ROM)

# SGB processing is switched on for this one, and PyBoy answers sgb_check()
# yes, so the release ROM runs the transfer for real: what is checked is the
# bytes the SGB ends up holding against src/border_data.c.
sgb-test: $(ROM)
	$(PYTHON) tests/sgb_border_test.py $(ROM)

run: $(ROM)
	$(MGBA) $(ROM)

docker-image:
	docker build -t $(DOCKER_IMAGE) .

clean:
	rm -rf build
