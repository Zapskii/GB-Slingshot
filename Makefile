# GB-Slingshot
#   make            build build/slingshot.gb (GBDK-2020 via lcc, or Docker if lcc isn't installed)
#   make test       host-side tests for src/sim.h (no GBDK needed)
#   make run        build, then open in mgba
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

.PHONY: all test emu-test run clean docker-image gen

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
	$(HOST_CC) -std=c99 -O1 -Wall -Wextra -Wno-unused-function -Isrc tests/test_sim.c -o $@ -lm

test: build/test_sim
	./build/test_sim

emu-test: $(ROM)
	$(PYTHON) tests/emu_replay.py $(ROM)

run: $(ROM)
	$(MGBA) $(ROM)

docker-image:
	docker build -t $(DOCKER_IMAGE) .

clean:
	rm -rf build
