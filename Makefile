# GB-Slingshot
#   make            build build/slingshot.gb (GBDK-2020 via lcc, or Docker if lcc isn't installed)
#   make test       host-side tests for src/sim.h (no GBDK needed)
#   make check-asm  scan the SDCC assembly for the cached-accumulator miscompile
#   make check-levels   brute-force that every level is winnable in its shot budget
#   make emu-test   replay the golden game through the ROM in a headless emulator (pip install pyboy pillow)
#   make docker-image   build the GBDK-2020 toolchain image used when lcc isn't on PATH
#   make clean

PYTHON       ?= python3
HOST_CC      ?= gcc
MGBA         ?= mgba
DOCKER_IMAGE ?= gb-slingshot-gbdk

ROM  := build/slingshot.gb
GEN  := src/tables.h src/gfx.h src/gfx.c src/levels.h

ifeq ($(shell command -v lcc 2>/dev/null),)
LCC = docker run --rm -v "$(CURDIR)":/work -w /work $(DOCKER_IMAGE) lcc
else
LCC = lcc
endif

.PHONY: all test check-asm check-levels emu-test run clean docker-image gen

all: $(ROM)

gen: $(GEN)

src/tables.h: tools/mktables.py
	$(PYTHON) tools/mktables.py src/tables.h

src/gfx.h src/gfx.c: tools/mkgfx.py
	$(PYTHON) tools/mkgfx.py src

src/levels.h: tools/mklevels.py $(wildcard levels/*.txt)
	$(PYTHON) tools/mklevels.py src

# Cart type 0x03 = MBC1 + RAM + battery, so the star ratings survive a reset.
$(ROM): src/main.c src/sim.h $(GEN)
	@mkdir -p build
	$(LCC) -Wa-l -Wl-m -Wl-j -Wm-yn"SLINGSHOT" -Wm-yt0x03 -o $(ROM) src/main.c src/gfx.c

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
	@$(PYTHON) tools/check_asm.py --self-test
	@$(PYTHON) tools/check_asm.py build/main.asm build/gfx.asm

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

run: $(ROM)
	$(MGBA) $(ROM)

docker-image:
	docker build -t $(DOCKER_IMAGE) .

clean:
	rm -rf build
