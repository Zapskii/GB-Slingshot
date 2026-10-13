#!/usr/bin/env python3
"""Replay the recorded golden game through the real ROM in a headless emulator.

Selects the recorded level on the level select screen, plays the three golden
shots, and compares the emulated game state and grid hash with the host-side
values. That checks the SDCC build behaves like the gcc one. It also checks the
front end, which the host tests cannot reach: that the collapse slides sprites,
that the window HUD tracks the score, that the win and lose splashes replace it
(a second replay wins with shots to spare, so the 3-star splash and banking are
covered too), and that the star ratings survive a reset.

The save check needs two emulator sessions: the first plays and saves, the
second starts from a cold boot and reads the .sav back.
Needs: pip install pyboy pillow

    python3 tests/emu_replay.py build/slingshot.gb [screenshots.png]
"""
import json
import os
import re
import sys

from PIL import Image
from pyboy import PyBoy

GRID_W, GRID_H = 20, 18                # the play field; the map itself is 32x32
# The Sim byte layout comes from build/sim_offsets.json, which make emu-test
# regenerates from the real header (tests/sim_offsets_probe.c) before every
# run. Nothing here is hand-maintained, so struct edits stay in sync instead
# of shipping silently-wrong emulator checks.
if not os.path.exists("build/sim_offsets.json"):
    sys.exit("tests/emu_replay.py: build/sim_offsets.json is missing; run "
             "make emu-test, or compile tests/sim_offsets_probe.c once")
with open("build/sim_offsets.json") as _f:
    _SIM = {k: int(v) for k, v in json.load(_f).items()}
CELLS = _SIM["cells"]
OFF_MAT = _SIM["mat"]
OFF_HP = _SIM["hp"]
OFF_STATE = _SIM["state"]
OFF_SHOTS_LEFT = _SIM["shots_left"]
OFF_TARGETS = _SIM["targets_left"]
OFF_SCORE = _SIM["score"]               # uint16, little endian
S_AIM = _SIM["s_aim"]
S_FLIGHT = _SIM["s_flight"]
S_SETTLE = _SIM["s_settle"]
S_WON = _SIM["s_won"]
S_LOST = _SIM["s_lost"]

# Front-end constants from src/main.c and src/gfx.h.
REG_WY = 0xFF4A
REG_SCY, REG_SCX = 0xFF42, 0xFF43        # screen shake moves the background only
REG_NR10, REG_NR51 = 0xFF10, 0xFF25     # ch1 sweep and sound panning: both readable
LAUNCH_SWEEP = 0x15                     # main.c SFX_SWEEP, written by sfx_init
REG_NR42 = 0xFF21                       # ch4 envelope: write-only on hardware, but
IMPACT_ENV = 0xF4                       # PyBoy latches it, and it reads 0 until a hit
BKG_MAP = 0x9800                        # background map base, where the cells land
WIN_MAP = 0x9C00                        # window map base
SRAM = 0xA000
OAM = 0xFE00
HUD_ROW = 136                           # move_win(7, HUD_ROW) parks the HUD at the bottom
SAVE_MAGIC = 0x5A
TILE_BALL, TILE_TARGET = 7, 4
TILE_FONT_A, TILE_FONT_0, TILE_STAR = 13, 39, 51
TILE_BROKEN0, TILE_BROKEN3 = 52, 55     # WOOD/STONE/ICE/TARGET_BROKEN
TILE_PUFF = 56                         # appended after them in tools/mkgfx.py
SPR_FALL0, NUM_FALL = 6, 8              # sprites 6..13 slide falling cells
SPR_PUFF0, NUM_PUFF = 14, 10            # and 14..23 are the impact puffs
PUFF_LIFE = 12                         # main.c: frames a speck is drawn for

# Must match test_golden_replay in tests/test_sim.c.
GOLDEN_LEVEL = 2
GOLDEN_HASH = 0xC6967041
GOLDEN_SCORE = 120
SHOTS = [(0, 7), (0, 5), (0, 5)]        # (angle index, power index)
# An early win with shots to spare: level 1 falls to this single shot, leaving
# two and rating 3 stars. The golden replay wins on its last shot, so this
# session in main() covers the other branch: the splash, the 3-star row, and a
# 3-star bank, all reached from a win the golden game never makes. Found by a
# sim search (sim_load_ascii + sim_fire); level 1 needs no special projectile.
EARLY_SHOTS = [(0, 4)]
START_ANGLE, START_POWER = 3, 1         # main.c AIM_ANGLE / AIM_POWER
SHAKE_PX = 2                           # main.c shake_off peaks at 2


failures = []


def check(cond, msg):
    print(("ok    " if cond else "FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def fnv(mat, hp):
    h = 2166136261
    for m, p in zip(mat, hp):
        h = ((h ^ m) * 16777619) & 0xFFFFFFFF
        h = ((h ^ p) * 16777619) & 0xFFFFFFFF
    return h


class Game:
    """One emulator session, with the helpers the checks are written in."""

    def __init__(self, rom):
        self.pb = PyBoy(rom, window="null")
        self.pb.set_emulation_speed(0)
        self.mem = self.pb.memory
        noi = open(rom.rsplit(".", 1)[0] + ".noi").read()
        self.base = int(re.search(r"DEF _sim 0x([0-9A-Fa-f]+)", noi).group(1), 16)
        self.images = []
        # The most of each effect seen at once, tracked frame by frame while a
        # shot plays: both are transient, so a snapshot after the fact is empty.
        self.max_puffs = 0
        self.max_shake = 0

    def byte(self, off):
        """A field of the Sim struct, which is what the byte offsets describe."""
        return self.mem[self.base + off]

    def grid_hash(self):
        mat = [self.mem[self.base + OFF_MAT + i] for i in range(CELLS)]
        hp = [self.mem[self.base + OFF_HP + i] for i in range(CELLS)]
        return fnv(mat, hp)

    def tick(self, n=1):
        self.pb.tick(n)

    def press(self, button, hold=2, gap=2):
        self.pb.button_press(button)
        self.pb.tick(hold)
        self.pb.button_release(button)
        self.pb.tick(gap)

    def win_row(self, r):
        return [self.mem[WIN_MAP + r * 32 + c] for c in range(20)]

    def win_text(self, r):
        out = []
        for t in self.win_row(r):
            if TILE_FONT_A <= t <= TILE_FONT_A + 25:
                out.append(chr(ord("A") + t - TILE_FONT_A))
            elif TILE_FONT_0 <= t <= TILE_FONT_0 + 9:
                out.append(chr(ord("0") + t - TILE_FONT_0))
            else:
                out.append(" " if t == 0 else "?")
        return "".join(out).rstrip()

    def sliding_cells(self):
        """How many collapse-animation sprites are on screen right now."""
        n = 0
        for s in range(SPR_FALL0, SPR_FALL0 + NUM_FALL):
            if self.mem[OAM + s * 4] >= 16 and 1 <= self.mem[OAM + s * 4 + 2] <= 5:
                n += 1
        return n

    def ball_sprites(self):
        """Balls and split shards: OAM entries wearing the ball tile."""
        n = 0
        for s in range(16):
            y = self.mem[OAM + s * 4]
            if y >= 16 and self.mem[OAM + s * 4 + 2] == TILE_BALL:
                n += 1
        return n

    def puff_sprites(self):
        """Specks of the puff a shot kicks up where it hit."""
        n = 0
        for s in range(SPR_PUFF0, SPR_PUFF0 + NUM_PUFF):
            if self.mem[OAM + s * 4] >= 16 and self.mem[OAM + s * 4 + 2] == TILE_PUFF:
                n += 1
        return n

    def shake(self):
        """How far the background is currently knocked off zero."""
        dx = self.mem[REG_SCX]
        dy = self.mem[REG_SCY]
        return max(dx if dx < 128 else 256 - dx, dy if dy < 128 else 256 - dy)

    def dirty_margin(self):
        """Map tiles outside the 20x18 play field that are not blank. The shake
        scrolls into them, so anything there is garbage the player will see."""
        n = 0
        for y in range(32):
            for x in range(GRID_W, 32):
                if self.mem[BKG_MAP + y * 32 + x] != 0:
                    n += 1
        for x in range(GRID_W):
            for y in range(GRID_H, 32):
                if self.mem[BKG_MAP + y * 32 + x] != 0:
                    n += 1
        return n

    def broken_cells(self):
        """How many cells the background draws cracked. A shot that only chips
        a cell damages it without destroying it, so this is the only way the
        player hears about it."""
        return sum(1 for i in range(32 * 18)
                   if TILE_BROKEN0 <= self.mem[BKG_MAP + i] <= TILE_BROKEN3)

    def snap(self, label):
        self.images.append((label, self.pb.screen.image.copy()))

    def snap_once(self, label):
        """Both effects are over in a few frames, so the only way to get a still
        of one is to catch it the first frame it appears."""
        if not any(l == label for l, _ in self.images):
            self.snap(label)

    def wait_for(self, pred, frames=900):
        """Tick until pred() holds. The emulator plays its own boot logo first."""
        for _ in range(frames):
            self.pb.tick(1)
            if pred():
                return True
        return False

    def boot(self):
        ok = self.wait_for(lambda: self.win_text(3).strip() == "SLINGSHOT")
        self.pb.tick(6)
        return ok

    def pick_level(self, n):
        """Walk the select cursor to level n. Left first, so a save left over
        from an earlier run cannot move the starting point."""
        for _ in range(20):
            self.press("left")
        for _ in range(n):
            self.press("right")
        self.pb.tick(4)

    def stop(self):
        self.pb.stop()


def play_until_idle(g, limit=900):
    """Run the current shot to its end. Returns how many collapse sprites were
    seen sliding at once. The transient effects are sampled frame by frame and
    their peaks left on the Game, since neither survives to the end of the shot."""
    slid = 0
    seen_puff = False
    for _ in range(limit):
        g.tick(1)
        slid = max(slid, g.sliding_cells())
        puffs = g.puff_sprites()
        g.max_puffs = max(g.max_puffs, puffs)
        g.max_shake = max(g.max_shake, g.shake())
        # Shadow OAM reaches the PPU at the next VBL, so this frame's poke is
        # drawn on the next one: snap a puff the frame after it shows up.
        if seen_puff:
            g.snap_once("puff")
        seen_puff = puffs > 0
        if g.shake():
            g.snap_once("shake")
        if g.byte(OFF_STATE) not in (S_FLIGHT, S_SETTLE):
            break
    g.tick(4)                           # let the last VRAM writes reach the screen
    return slid


def wait_splash(g, frames=180):
    """A splash is some 400 VRAM writes, and GBDK's set_win_tile_xy waits on the
    PPU, so it takes a few frames to land. Row 14 is written last: wait for it
    rather than counting frames, or a timing shift leaves the check reading a
    half-drawn screen."""
    return g.wait_for(lambda: g.win_text(14).strip() == "PRESS START", frames)


def aim_to(g, a, p, from_angle, from_power):
    for _ in range(abs(a - from_angle)):
        g.press("up" if a > from_angle else "down")
    for _ in range(abs(p - from_power)):
        g.press("right" if p > from_power else "left")
    return a, p


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else "build/slingshot.gb"
    sheet_path = sys.argv[2] if len(sys.argv) > 2 else None
    # PyBoy keeps cartridge RAM in <rom>.ram and loads it on boot, so session 1
    # has to start from a blank cart or last run's stars leak into the checks.
    ram = rom + ".ram"
    if os.path.exists(ram):
        os.remove(ram)

    # ---------- session 1: play, then save ----------
    g = Game(rom)
    check(g.boot(), "boots into the level select screen")
    check(g.mem[REG_WY] == 0, "level select covers the whole screen (WY=%d)" % g.mem[REG_WY])
    # NR51 powers on at 0xF0, so this cannot pass without sfx_init running.
    # NR50 is deliberately not checked: it powers on at 0x77, the value we write.
    check(g.mem[REG_NR51] == 0xFF,
          "the APU is initialised (NR51=%02X)" % g.mem[REG_NR51])
    check(g.win_text(15).strip() == "PRESS A", "level select prompts for A")
    check(g.mem[SRAM] != SAVE_MAGIC, "a fresh cart has no save yet")
    g.snap("select")

    g.pick_level(GOLDEN_LEVEL)
    check(g.win_text(9).strip() == str(GOLDEN_LEVEL + 1),
          "level select shows the cursor (got %r)" % g.win_text(9))
    g.press("a")
    g.tick(6)
    check(g.mem[REG_WY] == HUD_ROW, "A starts the level and brings the HUD back")

    start_hash = g.grid_hash()
    check(g.byte(OFF_STATE) == S_AIM, "starts in the aim state")
    check(g.byte(OFF_SHOTS_LEFT) == 3, "starts with 3 shots")
    check(g.win_row(0)[0] == TILE_BALL and g.win_row(0)[2] == TILE_FONT_0 + 3,
          "HUD shows 3 shots before the first shot")
    check("SCORE" in g.win_text(0) and g.win_row(0)[5] == TILE_TARGET,
          "HUD shows targets and a score label")

    # First play it badly: three weak shots leave the target standing.
    angle, power = aim_to(g, 0, 0, START_ANGLE, START_POWER)
    check(g.broken_cells() == 0, "every cell starts out drawn whole")
    g.max_puffs = 0
    g.press("a")
    # NR10 bit 7 is unmapped and reads as 1, so compare the seven real bits.
    check(g.mem[REG_NR10] & 0x7F == LAUNCH_SWEEP,
          "firing reaches the APU: the launch sweep is set (NR10=%02X)"
          % g.mem[REG_NR10])
    play_until_idle(g)
    check(g.broken_cells() > 0,
          "a shot that chips a cell without breaking it draws it cracked (%d)"
          % g.broken_cells())
    # This shot stops dead on the cell it hits rather than punching through, and
    # the puff has to survive that: it was the case that went missing when the
    # puff only fired while the sim was still in flight.
    check(g.max_puffs >= 4,
          "a shot that stops dead still kicks up a burst of %d puffs" % g.max_puffs)
    # The puff has to age out wherever the sim ends up. Aged only while the shot
    # was in flight, a speck still alive when the state machine left S_FLIGHT
    # froze on screen next to the reloaded ball until the next shot.
    g.tick(PUFF_LIFE + 2)
    check(g.puff_sprites() == 0 and g.byte(OFF_STATE) == S_AIM,
          "the burst ages out on its own instead of freezing over the aim")
    check(g.mem[REG_NR42] == IMPACT_ENV,
          "the hit reaches the APU: the impact envelope is set (NR42=%02X, "
          "pitch NR43=%02X)" % (g.mem[REG_NR42], g.mem[0xFF22]))
    for _ in range(2):
        g.press("a")
        play_until_idle(g)
    check(g.byte(OFF_STATE) == S_LOST, "three weak shots run out of shots")
    check(wait_splash(g), "the lose splash finishes drawing")
    check(g.mem[REG_WY] == 0, "lose splash takes over the whole screen")
    check(g.win_text(4).strip() == "OUT OF SHOTS",
          'lose splash says "OUT OF SHOTS" (got %r)' % g.win_text(4))
    check(g.win_text(7).strip() == "TARGETS 1", "lose splash reports the targets left")
    check(g.win_text(14).strip() == "PRESS START", "lose splash prompts for START")
    check(g.mem[SRAM] != SAVE_MAGIC, "losing banks nothing")
    g.snap("lost")

    g.press("start")
    g.tick(6)
    check(g.byte(OFF_STATE) == S_AIM and g.byte(OFF_SHOTS_LEFT) == 3,
          "START after a loss retries the same level")
    check(g.grid_hash() == start_hash, "the retry restores the starting grid")
    check(g.broken_cells() == 0, "the retry redraws every cell whole")

    # Projectile types. B cycles before firing; a split shot fans into three
    # balls, which only the front end can show.
    g.press("b")
    g.tick(2)
    check(g.win_row(0)[9] == TILE_FONT_A + 1, "B switches the HUD to projectile B")
    g.press("b")
    g.tick(2)
    check(g.win_row(0)[9] == TILE_FONT_A + 2, "B again switches to projectile C")
    g.press("a")
    g.tick(20)
    check(g.ball_sprites() == 1, "one ball in flight before the split")
    g.press("a")                            # mid-flight: spend the ability
    g.tick(3)
    check(g.ball_sprites() == 3,
          "a split shot draws three balls (got %d)" % g.ball_sprites())
    g.snap("split")
    g.press("start")                        # abandon the shot, get the level back
    g.tick(6)
    check(g.grid_hash() == start_hash, "START mid-flight abandons the shot")
    g.press("b")                            # C wraps round to A for the golden run
    g.tick(2)
    check(g.win_row(0)[9] == TILE_FONT_A, "B wraps back round to projectile A")

    # Now the recorded game.
    angle, power = START_ANGLE, START_POWER
    slid = 0
    g.max_puffs = 0                     # the shake is measured over the golden shots
    g.max_shake = 0
    for n, (a, p) in enumerate(SHOTS):
        angle, power = aim_to(g, a, p, angle, power)
        if n == 0:
            g.snap("aimed")
        g.press("a")
        g.tick(25)
        if n == 0:
            check(g.byte(OFF_STATE) == S_FLIGHT, "shot 1 is in flight after 25 frames")
            check(g.win_row(0)[2] == TILE_FONT_0 + 2, "HUD counts the shot down to 2")
            g.snap("flight")
        slid = max(slid, play_until_idle(g))

    check(slid > 0, "collapse slid up to %d cells as sprites" % slid)
    check(g.max_shake == SHAKE_PX,
          "the %d-cell collapse shakes the background %d px" % (slid, g.max_shake))
    check(g.shake() == 0, "and the screen settles back to 0,0")
    check(g.dirty_margin() == 0,
          "the margin the shake scrolls into is blank, not leftover VRAM (%d tiles)"
          % g.dirty_margin())
    check(g.byte(OFF_STATE) == S_WON, "game ends won (state %d)" % g.byte(OFF_STATE))
    check(g.byte(OFF_TARGETS) == 0, "no targets left")
    check(g.byte(OFF_SHOTS_LEFT) == 0, "all three shots used")
    score = g.byte(OFF_SCORE) | (g.byte(OFF_SCORE + 1) << 8)
    check(score == GOLDEN_SCORE, "score %d matches host (%d)" % (score, GOLDEN_SCORE))
    h = g.grid_hash()
    check(h == GOLDEN_HASH, "grid hash 0x%08X matches host golden 0x%08X" % (h, GOLDEN_HASH))

    check(wait_splash(g), "the win splash finishes drawing")
    check(g.mem[REG_WY] == 0, "win splash takes over the whole screen (WY=%d)" % g.mem[REG_WY])
    check(g.win_text(4).strip() == "YOU WIN", 'win splash says "YOU WIN" (got %r)' % g.win_text(4))
    check(g.win_text(14).strip() == "PRESS START", "win splash prompts for START")
    check(g.win_row(7)[9] == TILE_STAR, "win splash rates the finish at 1 star")
    g.snap("won")

    # START banks the star (one star: three shots used) and moves on.
    g.press("start")
    g.tick(8)
    check(g.mem[SRAM] == SAVE_MAGIC, "winning writes the save")
    check(g.mem[SRAM + 1 + GOLDEN_LEVEL] == 1,
          "level %d's star is 1 in battery RAM" % (GOLDEN_LEVEL + 1))
    check(g.byte(OFF_STATE) == S_AIM and g.byte(OFF_SHOTS_LEFT) == 3,
          "START after a win loads the next level")
    check(g.grid_hash() != start_hash, "the next level is a different grid")
    check(g.mem[REG_WY] == HUD_ROW, "the HUD comes back for the next level")

    g.press("select")
    g.tick(6)
    check(g.win_text(3).strip() == "SLINGSHOT", "SELECT returns to the level select")
    check(g.win_text(9).strip() == str(GOLDEN_LEVEL + 2),
          "the cursor moved on to the next level (got %r)" % g.win_text(9))
    g.stop()

    # ---------- session 2: a cold boot must read the stars back ----------
    g2 = Game(rom)
    check(g2.boot(), "the ROM boots again")
    check(g2.mem[SRAM] == SAVE_MAGIC, "the save survived the reset")
    # The cursor resumes on the first level without a star. Only level 3 was
    # beaten here, so that is level 1 -- which is the right answer for a player
    # who still has 1 and 2 to do.
    check(g2.win_text(9).strip() == "1",
          "the cursor resumes on the first unbeaten level (got %r)" % g2.win_text(9))
    # Levels 11+ are built around one projectile type, and the select screen is
    # the only place that tells the player which.
    g2.pick_level(14)
    check(g2.win_text(5).strip() == "NEEDS B",
          "the select screen names the needed projectile (got %r)" % g2.win_text(5))
    g2.snap("needs")
    g2.pick_level(GOLDEN_LEVEL)
    check(g2.win_text(9).strip() == str(GOLDEN_LEVEL + 1),
          "the cursor can walk back to the beaten level")
    check(g2.win_row(12)[9] == TILE_STAR, "the saved star is drawn on the select screen")
    # The select screen looks like a game-over screen and follows the "PRESS
    # START" splash, so START has to work on it as well as A.
    g2.press("start")
    g2.tick(6)
    check(g2.byte(OFF_STATE) == S_AIM and g2.mem[REG_WY] == HUD_ROW,
          "START on the level select starts the level")
    g2.snap("saved")
    g2.stop()

    # ---------- session 3: an early win, with shots to spare ----------
    # The golden replay wins on its last shot; most real play does not. This
    # session wins level 1 with two shots still in the bank, so the splash,
    # the 3-star row and 3-star banking are all exercised on the branch the
    # golden game never takes.
    g3 = Game(rom)
    check(g3.boot(), "the ROM boots for the early win")
    g3.pick_level(0)                        # level 1 needs no special projectile
    g3.press("a")                           # A starts the level
    g3.tick(6)
    aim_to(g3, *EARLY_SHOTS[0], START_ANGLE, START_POWER)
    g3.press("a")
    play_until_idle(g3)
    check(g3.byte(OFF_STATE) == S_WON and g3.byte(OFF_SHOTS_LEFT) == 2,
          "one shot wins level 1 with 2 to spare (state %d, shots %d)"
          % (g3.byte(OFF_STATE), g3.byte(OFF_SHOTS_LEFT)))
    check(wait_splash(g3), "the early win still draws the splash")
    check(g3.mem[REG_WY] == 0, "early-win splash covers the screen (WY=%d)"
          % g3.mem[REG_WY])
    check(g3.win_text(4).strip() == "YOU WIN",
          'early-win splash says "YOU WIN" (got %r)' % g3.win_text(4))
    for col in (7, 9, 11):                  # 3 stars centred on tile 10
        check(g3.win_row(7)[col] == TILE_STAR,
              "3-star row drawn at column %d (got %d)" % (col, g3.win_row(7)[col]))
    g3.snap("won-early")
    g3.press("start")                       # bank the 3 stars, move on
    g3.tick(8)
    check(g3.mem[SRAM + 1] == 3,
          "the 3-star early win is banked for level 1 (got %d)" % g3.mem[SRAM + 1])
    check(g3.byte(OFF_STATE) == S_AIM, "START after the early win loads level 2")
    g3.stop()

    if sheet_path:
        images = g.images + g2.images + g3.images
        w, h = images[0][1].size
        scale, pad = 2, 8
        sheet = Image.new("RGB", (len(images) * (w * scale + pad) - pad, h * scale), (255, 0, 255))
        for i, (_, img) in enumerate(images):
            sheet.paste(img.resize((w * scale, h * scale), Image.NEAREST), (i * (w * scale + pad), 0))
        sheet.save(sheet_path)
        print("screenshots:", ", ".join(l for l, _ in images), "->", sheet_path)

    print("\n%d failed" % len(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
