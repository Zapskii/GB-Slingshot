/*
 * GB-Slingshot - Phase 4: levels, star ratings and a battery-saved save.
 *
 *   D-pad up/down   angle        D-pad left/right   power
 *   B               cycle projectile type
 *   A               fire         A again in flight  use the type's ability
 *   START           restart / next level        SELECT   back to the level select
 *
 * Game rules live in sim.h, the levels in levels.h. This file only reads the
 * pad, steps the sim and draws the result.
 */
#include <gb/gb.h>
#include <stdint.h>
#include "gfx.h"
#include "levels.h"
#include "sim.h"

#define SPR_BALL 0
#define SPR_DOT0 1
#define NUM_DOTS 5
#define DOT_EVERY 6             /* flight frames between preview dots */

/* Falling cells get their own sprites, one per move in sim.moves. */
#define SPR_FALL0 (SPR_DOT0 + NUM_DOTS)

/* The puff a shot kicks up. Ten fits the budget the build plan set for
 * particles; with the falls and the ball that can crowd one scanline, and the
 * hardware drops a sprite for that line rather than doing anything worse. */
#define SPR_PUFF0 (SPR_FALL0 + MAX_FALL_MOVES)
#define MAX_PUFFS 10
#define PUFF_LIFE 12            /* frames a speck is drawn for */
#define NUM_PUFF_SPECKS 4       /* one burst, out along the diagonals */
#define PUFF_LEAD 6             /* px the specks start out at, clear of the ball */

/* Screen shake, for a collapse big enough to be worth feeling. The background
 * moves and the window layer does not, so the HUD stays put.
 *
 * Two is "big" for this game: measured over every shot on every level, a
 * collapse step moves one cell 49% of the time, two 33%, three 16% and four
 * 2.6%, and nothing bigger ever happens. One is the ordinary settling droop,
 * so the knob sits between the two. Tuned by playing; loosen it to 1 if every
 * creak should thump. */
#define SHAKE_FRAMES 10
#define SHAKE_MIN_FALLS 2       /* cells falling in one step */

/* The chain flash: a fall that destroyed cells scores double, and a "CHAIN"
 * line appears under the HUD for this many frames. Tuned by playing. */
#define CHAIN_FRAMES 30

#define SLING_COL 2             /* left tile of the two-tile slingshot */
#define SLING_ROW 12

/* Where the aim starts on a fresh level. It has to land on the board: at the
 * old default (40 degrees, power 4) the shot left the screen without touching
 * anything on every level. This one lands mid-screen and never wins outright.
 * Calibration knobs - tune per level by moving the values into levels.h. */
#define AIM_ANGLE 3             /* 20 degrees */
#define AIM_POWER 1

#define WIN_W 20
#define HUD_ROW 136             /* window Y that leaves one row of HUD visible */

/* The boot title: the 9x2 block the doubled logo occupies. mkgfx.py slices
 * the "SLINGSHOT" canvas to match: TITLE_0 is the block's top-left tile, and
 * the rest follow row-major. The title waits for a button, like the prompt
 * says. */
#define TITLE_COLS 9
#define TITLE_ROWS 2

#define DIRS (J_UP | J_DOWN | J_LEFT | J_RIGHT)
#define REPEAT_DELAY 14
#define REPEAT_MASK 3           /* repeat every 4th frame once held */

Sim sim;                 /* not static, so the emulator test can find it in the map */
static uint8_t shown[GRID_CELLS];   /* tile currently on screen per cell */
static uint8_t hidden[GRID_CELLS];  /* cell blanked because a sprite stands in for it */

/* A cell sliding from (x, y-1) to (x, y). mat == M_EMPTY means the slot is free;
 * empty cells never fall, so that is unambiguous. */
typedef struct {
    uint8_t mat, x, y, frames;
} Fall;
static Fall falls[MAX_FALL_MOVES];

/* One speck of the puff a shot kicks up where it hit. frames == 0 means the
 * slot is free; a live speck always has at least one frame left. */
typedef struct {
    int16_t x, y;               /* whole pixels */
    int8_t vx, vy;              /* whole pixels per frame: dust, not ballistics */
    uint8_t frames;
} Puff;
static Puff puffs[MAX_PUFFS];
static const int8_t puff_vx[NUM_PUFF_SPECKS] = { -1, 1, -1, 1 };
static const int8_t puff_vy[NUM_PUFF_SPECKS] = { -1, -1, 1, 1 };

/* A decaying wobble the background is held at, one entry per frame. */
static const int8_t shake_off[SHAKE_FRAMES] = { 2, -2, 2, -2, 1, -1, 1, -1, 0, 0 };
static uint8_t shake;               /* frames of shake left */

static uint8_t angle, power;
static uint8_t ptype;               /* PT_*, chosen with B before firing */
/* Each type flies its own sprite: the aim ball and the flight ball wear the
 * tile of the armed type, so B visibly changes what you are about to fire. */
static const uint8_t ball_tile[PT_COUNT] = {
    TILE_BALL, TILE_BALL_B, TILE_BALL_C, TILE_BALL_D
};
static uint8_t keys, prev_keys, last_dirs, hold;
static uint8_t last_state;
static uint8_t arc_dirty;
static uint8_t win_mode;            /* which of HUD / splash / select the window holds */
static uint8_t hud_shots, hud_targets, hud_type;
static uint16_t hud_score;

static uint8_t chain_frames;        /* remaining frames of the chain flash */
static uint16_t chain_shown;        /* points the flash is displaying */

static uint8_t level;               /* the level being played */
static uint8_t cursor;              /* the level the select screen is pointing at */
static uint8_t menu;                /* 1 while the select screen is up */
static uint8_t title;               /* 1 while the boot title is up */
static uint8_t stars[NUM_LEVELS];   /* best rating per level, mirrored in save RAM */

enum { W_HUD = 0, W_SPLASH, W_SELECT };

/* ---------- save ---------- */

/* MBC1 gates cartridge RAM behind a magic write to the ROM area. Writing it
 * on a cart without MBC1 is harmless: ROM ignores writes. */
#define MBC1_RAM_ENABLE (*(volatile uint8_t *)0x0000)
#define MBC1_RAM_BANK   (*(volatile uint8_t *)0x4000)

#define SAVE_MAGIC 0x5A             /* marks RAM as written by this game */

static void load_stars(void)
{
    uint8_t i;
    MBC1_RAM_ENABLE = 0x0A;
    MBC1_RAM_BANK = 0;
    if (_SRAM[0] != SAVE_MAGIC) {
        for (i = 0; i < NUM_LEVELS; i++) stars[i] = 0;
    } else {
        for (i = 0; i < NUM_LEVELS; i++)
            stars[i] = _SRAM[1 + i] <= 3 ? _SRAM[1 + i] : 0;
    }
}

static void save_stars(void)
{
    uint8_t i;
    MBC1_RAM_ENABLE = 0x0A;
    MBC1_RAM_BANK = 0;
    for (i = 0; i < NUM_LEVELS; i++) _SRAM[1 + i] = stars[i];
    _SRAM[0] = SAVE_MAGIC;      /* written last, so a torn save reads as empty */
}

/* ---------- sound ---------- */

/* The APU written directly: GBDK 4.5 ships no sound helpers at all. Every byte
 * below is a calibration knob to be tuned by ear in mgba, not a derived value. */
#define SFX_VOL        0x77     /* NR50: both terminals, full volume */
#define SFX_PAN        0xFF     /* NR51: every channel to both terminals */
#define SFX_SWEEP      0x15     /* NR10: ch1 sweep up - that is the launch whoosh */
#define SFX_IMPACT_ENV 0xF4     /* NR42: volume 15, fading, period 4 */
#define SFX_COOLDOWN   6        /* frames of quiet after a crunch */
#define SFX_STEP       8        /* frames per jingle note */
#define TITLE_STEP     12       /* frames per note of the title tune */

/* Win is C E G C rising, lose is G E C falling, as (NR23 low, NR24 high) pairs.
 * Flat and stride 2 rather than an array of pairs: SDCC is happier without a
 * pointer to an array type. */
static const uint8_t win_notes[]  = { 0x05, 0x07, 0x39, 0x07, 0x59, 0x07, 0x83, 0x07 };
static const uint8_t lose_notes[] = { 0xB2, 0x06, 0x73, 0x06, 0x0C, 0x06 };

/* The title tune: one C-major loop, the win arpeggio drawn out into a tune -
 * phrase and answer, a scale run home, then resolve. (0, 0) is a rest: the
 * note before it is still ringing, so a pair of rests is a hold. The table
 * is rendered to a WAV by tools/render_titles.py for tuning by ear. */
static const uint8_t title_notes[] = {
    /* C5  E5  G5  E5  */
    0x06, 0x07,  0x39, 0x07,  0x59, 0x07,  0x39, 0x07,
    /* A5  G5  E5   -  */
    0x6B, 0x07,  0x59, 0x07,  0x39, 0x07,  0x00, 0x00,
    /* F5  E5  D5  E5  */
    0x44, 0x07,  0x39, 0x07,  0x21, 0x07,  0x39, 0x07,
    /* C5   -   G4   -  */
    0x06, 0x07,  0x00, 0x00,  0xB2, 0x06,  0x00, 0x00,
    /* A4  B4  C5  D5  */
    0xD6, 0x06,  0xF7, 0x06,  0x06, 0x07,  0x21, 0x07,
    /* E5   -   -    -  */
    0x39, 0x07,  0x00, 0x00,  0x00, 0x00,  0x00, 0x00,
    /* G5  E5  D5  B4  */
    0x59, 0x07,  0x39, 0x07,  0x21, 0x07,  0xF7, 0x06,
    /* C5   -   -    -  */
    0x06, 0x07,  0x00, 0x00,  0x00, 0x00,  0x00, 0x00
};

static uint8_t sfx_cool;
static const uint8_t *jingle;
static uint8_t jingle_i, jingle_n, jingle_wait;
static uint8_t jingle_step;        /* frames per note (jingles tick faster than the tune) */
static uint8_t jingle_loop;        /* 1 while the tune should keep coming round */

static void sfx_init(void)
{
    NR52_REG = 0x80;                /* APU on */
    NR50_REG = SFX_VOL;
    NR51_REG = SFX_PAN;
    NR10_REG = SFX_SWEEP;
}

/* ch1: the launch is a pulse that rises on its own sweep. */
static void sfx_launch(void)
{
    NR11_REG = 0x80;                /* 50% duty, full length */
    NR12_REG = 0xF3;
    NR13_REG = 0x00;
    NR14_REG = 0x86;
}

/* ch2: one note. The ability blip and the target pop are the same thing at
 * different pitches. */
static void sfx_note(uint8_t lo, uint8_t hi)
{
    NR21_REG = 0x80;
    NR22_REG = 0xF3;
    NR23_REG = lo;
    NR24_REG = (uint8_t)(hi | 0x80);        /* trigger */
}

static void sfx_blip(void) { sfx_note(0xB0, 0x07); }
static void sfx_pop(void)  { sfx_note(0xC0, 0x07); }

/* ch4: the impacts. One envelope for all three; the divisor is the pitch, so
 * wood, stone and ice are the same crunch at three heights. */
static void sfx_impact(uint8_t mat)
{
    NR41_REG = 0x00;
    NR42_REG = SFX_IMPACT_ENV;
    NR43_REG = mat == M_WOOD ? 0x53 : mat == M_STONE ? 0x65 : 0x32;
    NR44_REG = 0x80;
}

static void sfx_jingle(const uint8_t *notes, uint8_t n)
{
    jingle = notes;
    jingle_n = n;
    jingle_i = 0;
    jingle_wait = 0;
    jingle_step = SFX_STEP;
    jingle_loop = 0;
}

/* The title's tune: the same sequencer, slower, looping until told to stop. */
static void sfx_title(void)
{
    jingle = title_notes;
    jingle_n = sizeof(title_notes) / 2;
    jingle_i = 0;
    jingle_wait = 0;
    jingle_step = TITLE_STEP;
    jingle_loop = 1;
}

/* Stop whatever ch2 is playing and silence it; the next blip re-arms the
 * envelope. Called when the title hands over to the level select. */
static void sfx_title_off(void)
{
    jingle_loop = 0;
    jingle_i = jingle_n = 0;
    NR22_REG = 0x00;
}

/* Once per frame, from the top of the main loop, so the jingle keeps playing on
 * the level select and the splash screens too. A (0, 0) pair is a rest: the
 * last note is left ringing, so double rests read as held notes. */
static void sfx_frame(void)
{
    if (sfx_cool) sfx_cool--;
    if (jingle_i >= jingle_n) return;
    if (jingle_wait) { jingle_wait--; return; }
    if (jingle[jingle_i * 2 + 1])
        sfx_note(jingle[jingle_i * 2], jingle[jingle_i * 2 + 1]);
    jingle_i++;
    if (jingle_loop && jingle_i >= jingle_n) jingle_i = 0;
    jingle_wait = jingle_step;
}

/* Voice what the sim just reported. Only called from the flight/settle branch:
 * sim_tick is what clears hit_mat, so reading it anywhere else would replay the
 * last hit on every frame of S_AIM. */
static void sfx_hit_events(void)
{
    if (sim.hit_mat == M_TARGET) {
        sfx_pop();                  /* the payoff, so not rate-limited */
    } else if (sim.hit_mat && !sfx_cool) {
        sfx_impact(sim.hit_mat);
        sfx_cool = SFX_COOLDOWN;    /* a collapse step can damage two cells at once */
    }
}

/* A collapse big enough to feel. Read here, beside sfx_hit_events, because
 * spawn_falls() clears nmoves and this is the only frame it is set. */
static void shake_events(void)
{
    if (sim.nmoves >= SHAKE_MIN_FALLS) shake = SHAKE_FRAMES;
}

/* ---------- drawing ---------- */

/* A cell that has taken damage is drawn cracked. Damage accumulates and a
 * cell that only dents stops the shot, so without this a chip leaves no trace
 * and the next identical shot appears to do something different. Indexed by
 * material (M_EMPTY first). */
static const uint8_t broken_tile[M_COUNT] = {
    TILE_BLANK, TILE_WOOD_BROKEN, TILE_STONE_BROKEN, TILE_ICE_BROKEN,
    TILE_TARGET_BROKEN, TILE_GROUND_TOP
};

static uint8_t tile_for(uint16_t idx)
{
    uint8_t m = sim.mat[idx];
    if (m == M_GROUND && idx < (uint16_t)(GROUND_ROW + 1) * GRID_W) return TILE_GROUND_TOP;
    if (sim.hp[idx] < mat_hp[m]) return broken_tile[m];
    return m;                   /* material ids are tile ids */
}

static void draw_sling(void)
{
    set_bkg_tile_xy(SLING_COL, SLING_ROW, TILE_FORK_L);
    set_bkg_tile_xy(SLING_COL + 1, SLING_ROW, TILE_FORK_R);
    set_bkg_tile_xy(SLING_COL, SLING_ROW + 1, TILE_POST_L);
    set_bkg_tile_xy(SLING_COL + 1, SLING_ROW + 1, TILE_POST_R);
    set_bkg_tile_xy(SLING_COL, SLING_ROW + 2, TILE_POST_L);
    set_bkg_tile_xy(SLING_COL + 1, SLING_ROW + 2, TILE_POST_R);
    set_bkg_tile_xy(SLING_COL, SLING_ROW + 3, TILE_POST_L);
    set_bkg_tile_xy(SLING_COL + 1, SLING_ROW + 3, TILE_POST_R);
}

static void draw_all(void)
{
    uint8_t x, y;
    uint16_t idx = 0;
    uint8_t t;
    for (y = 0; y < GRID_H; y++) {
        for (x = 0; x < GRID_W; x++) {
            t = tile_for(idx);
            set_bkg_tile_xy(x, y, t);
            shown[idx] = t;
            idx++;
        }
    }
    draw_sling();
}

/* Redraw only the cells that changed since the last frame. Cells standing in
 * for a falling sprite keep whatever the sprite spawn left on the background. */
static void redraw_changes(void)
{
    uint8_t x, y, t;
    uint16_t idx = 0;
    for (y = 0; y < GRID_H; y++) {
        for (x = 0; x < GRID_W; x++) {
            if (!hidden[idx]) {
                t = tile_for(idx);
                if (t != shown[idx]) {
                    set_bkg_tile_xy(x, y, t);
                    shown[idx] = t;
                }
            }
            idx++;
        }
    }
}

/* Place an 8x8 sprite centred on (x, y) in screen pixels. */
static void place_sprite(uint8_t n, int16_t x, int16_t y)
{
    if (x < -3 || x > 163 || y < -11 || y > 150) {
        move_sprite(n, 0, 0);
        return;
    }
    move_sprite(n, (uint8_t)(x + 4), (uint8_t)(y + 12));
}

static void hide_arc(void)
{
    uint8_t d;
    for (d = 0; d < NUM_DOTS; d++) move_sprite(SPR_DOT0 + d, 0, 0);
}

/* ---------- falling cells ---------- */

static void hide_falls(void)
{
    uint8_t i;
    uint16_t idx;
    for (i = 0; i < MAX_FALL_MOVES; i++) {
        falls[i].mat = M_EMPTY;
        move_sprite(SPR_FALL0 + i, 0, 0);
    }
    for (idx = 0; idx < GRID_CELLS; idx++) hidden[idx] = 0;
}

/* Hand a landed cell back to the background layer. Reads the sim now, so a
 * cell destroyed by landing damage or by falling on is drawn as it ended up. */
static void release_fall(Fall *f)
{
    uint16_t idx = (uint16_t)f->y * GRID_W + f->x;
    uint8_t t = tile_for(idx);
    hidden[idx] = 0;
    set_bkg_tile_xy(f->x, f->y, t);
    shown[idx] = t;
    move_sprite(SPR_FALL0 + (uint8_t)(f - falls), 0, 0);
    f->mat = M_EMPTY;
}

/* Slide the cells logged by the collapse step that just ran. Runs after
 * sim_tick, so a cell moving on a second step is released onto the grid its
 * successor is leaving, and the two sprites meet with no visible jump. */
static void spawn_falls(void)
{
    uint8_t i, slot;
    for (i = 0; i < sim.nmoves; i++) {
        const FallMove *m = &sim.moves[i];
        Fall *f = 0;
        uint16_t idx;
        for (slot = 0; slot < MAX_FALL_MOVES; slot++) {
            if (falls[slot].mat == M_EMPTY) { f = &falls[slot]; break; }
        }
        if (!f) break;                  /* out of sprites: the rest snap into place */
        f->mat = m->mat;
        f->x = m->x;
        f->y = (uint8_t)(m->y + 1);
        f->frames = 0;
        idx = (uint16_t)f->y * GRID_W + f->x;
        hidden[idx] = 1;
        set_bkg_tile_xy(f->x, f->y, TILE_BLANK);
        shown[idx] = TILE_BLANK;
        set_sprite_tile(SPR_FALL0 + slot, m->mat);
        place_sprite(SPR_FALL0 + slot, (int16_t)m->x * 8, (int16_t)m->y * 8);
    }
    sim.nmoves = 0;
}

/* One frame of slide. Entries expire exactly as often as collapse steps run,
 * so a cell that keeps falling is re-spawned the moment its old entry retires. */
static void age_falls(void)
{
    uint8_t i;
    for (i = 0; i < MAX_FALL_MOVES; i++) {
        Fall *f = &falls[i];
        if (f->mat == M_EMPTY) continue;
        if (++f->frames >= COLLAPSE_INTERVAL) {
            release_fall(f);
        } else {
            place_sprite(SPR_FALL0 + i, (int16_t)f->x * 8,
                         (int16_t)((f->y - 1) * 8 + (f->frames * 8) / COLLAPSE_INTERVAL));
        }
    }
}

/* ---------- puffs ---------- */

static void hide_puffs(void)
{
    uint8_t i;
    for (i = 0; i < MAX_PUFFS; i++) {
        puffs[i].frames = 0;
        move_sprite(SPR_PUFF0 + i, 0, 0);
    }
}

/* A burst of dust. The callers decide where it comes from and when it is
 * warranted: the ball's own hit is known only through sim.hit_mat (which
 * sim_tick clears each frame) and only while the shot was airborne last
 * frame - a shot that dents a cell stops on the spot, so by then the sim is
 * settling and the ball is frozen; and a collapse landing damages cells in
 * that same settling state, so asking hit_mat there would puff every crush
 * out of the wrong place. That is why the two reads are the callers'.
 *
 * The one burst that *is* collapse's own: a fall that destroys a cell reports
 * the site in sim.chain_x/chain_y beside chain_points, and puffs there. */
static void spawn_puffs(int16_t px, int16_t py)
{
    uint8_t i, n;
    for (i = 0; i < NUM_PUFF_SPECKS; i++) {
        for (n = 0; n < MAX_PUFFS && puffs[n].frames; n++) { }
        if (n == MAX_PUFFS) break;      /* out of sprites: the rest of the burst is dropped */
        puffs[n].x = (int16_t)(px + puff_vx[i] * PUFF_LEAD);
        puffs[n].y = (int16_t)(py + puff_vy[i] * PUFF_LEAD);
        puffs[n].vx = puff_vx[i];
        puffs[n].vy = puff_vy[i];
        puffs[n].frames = PUFF_LIFE;
        place_sprite(SPR_PUFF0 + n, puffs[n].x, puffs[n].y);
    }
}

static void age_puffs(void)
{
    uint8_t i;
    for (i = 0; i < MAX_PUFFS; i++) {
        Puff *p = &puffs[i];
        if (!p->frames) continue;
        p->x = (int16_t)(p->x + p->vx);
        p->y = (int16_t)(p->y + p->vy);
        if (--p->frames) place_sprite(SPR_PUFF0 + i, p->x, p->y);
        else move_sprite(SPR_PUFF0 + i, 0, 0);
    }
}

/* ---------- window: HUD and splash ---------- */

static uint8_t char_tile(char c)
{
    if (c >= 'A' && c <= 'Z') return (uint8_t)(TILE_FONT_A + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(TILE_FONT_0 + (c - '0'));
    if (c == '-') return TILE_FONT_DASH;
    if (c == ':') return TILE_FONT_COLON;
    return TILE_BLANK;
}

static void win_text(uint8_t x, uint8_t y, const char *s)
{
    while (*s) set_win_tile_xy(x++, y, char_tile(*s++));
}

/* Right-aligned in w columns. Values wider than the field keep their low digits. */
static void win_num(uint8_t x, uint8_t y, uint16_t v, uint8_t w)
{
    char b[8];
    uint8_t n = 0, i;
    do {
        b[n++] = (char)('0' + v % 10);
        v = (uint16_t)(v / 10);
    } while (v && n < w);
    for (i = 0; i < (uint8_t)(w - n); i++) set_win_tile_xy((uint8_t)(x + i), y, TILE_BLANK);
    for (i = 0; i < n; i++)
        set_win_tile_xy((uint8_t)(x + w - n + i), y, char_tile(b[n - 1 - i]));
}

static void win_clear(uint8_t rows)
{
    uint8_t x, y;
    for (y = 0; y < rows; y++)
        for (x = 0; x < WIN_W; x++) set_win_tile_xy(x, y, TILE_BLANK);
}

/* The chain flash slides the window up one row during play so its blank row 1
 * comes on screen under the live HUD, where "CHAIN +nn" is drawn. Every
 * restore is gated on win_mode: the splash and select screens set their own
 * window position, and a late restore would truncate a full-screen splash. */
static void chain_flash(void)
{
    if (!chain_frames) return;
    if (win_mode != W_HUD) {
        chain_frames = 0;       /* splash/select own the window now */
        return;
    }
    if (chain_frames == CHAIN_FRAMES) {         /* first frame: draw (also redraws) */
        win_text(1, 1, "CHAIN");
        win_num(7, 1, chain_shown, 3);
        move_win(7, (uint8_t)(HUD_ROW - 8));
    }
    chain_frames--;
    if (!chain_frames) move_win(7, HUD_ROW);
}

static void update_hud(void)
{
    if (win_mode != W_HUD) return;
    if (sim.shots_left == hud_shots && sim.targets_left == hud_targets &&
        ptype == hud_type && sim.score == hud_score) return;
    hud_shots = sim.shots_left;
    hud_targets = sim.targets_left;
    hud_type = ptype;
    hud_score = sim.score;
    win_clear(1);                              /* only row 0 is on screen */
    set_win_tile_xy(0, 0, TILE_BALL);           /* shots left */
    win_num(2, 0, sim.shots_left, 1);
    set_win_tile_xy(5, 0, TILE_TARGET);         /* targets left */
    win_num(7, 0, sim.targets_left, 1);
    set_win_tile_xy(9, 0, char_tile((char)('A' + ptype)));  /* projectile type */
    win_text(10, 0, "SCORE");
    win_num(16, 0, sim.score, 4);
    move_win(7, chain_frames ? (uint8_t)(HUD_ROW - 8) : HUD_ROW);
}

static void show_splash(void)
{
    uint8_t s, n;
    win_clear(GRID_H);
    move_win(7, 0);
    if (sim.state == S_WON) {
        n = sim_stars(&sim);
        win_text(6, 4, "YOU WIN");
        for (s = 0; s < n; s++)
            set_win_tile_xy((uint8_t)(10 - n + s * 2), 7, TILE_STAR);
        win_text(5, 10, "SCORE");
        win_num(11, 10, sim.score, 4);
    } else {
        win_text(4, 4, "OUT OF SHOTS");
        win_text(5, 7, "TARGETS");
        win_num(13, 7, sim.targets_left, 1);
    }
    win_text(4, 14, "PRESS START");
    win_mode = W_SPLASH;
}

/* The level select doubles as the title screen, and it is the only place the
 * saved stars are shown, which is what makes the battery save visible. */
static void show_select(void)
{
    uint8_t n = stars[cursor], s;
    win_clear(GRID_H);
    move_win(7, 0);
    win_text(5, 3, "SLINGSHOT");
    /* Levels built around a projectile type say so: there is no other way for
     * the player to know the standard shot cannot finish them. */
    if (level_needs[cursor] != PT_STANDARD) {
        win_text(7, 5, "NEEDS");
        set_win_tile_xy(13, 5, char_tile((char)('A' + level_needs[cursor])));
    }
    win_text(7, 7, "LEVEL");
    win_num(9, 9, (uint16_t)(cursor + 1), 2);
    for (s = 0; s < n; s++)
        set_win_tile_xy((uint8_t)(10 - n + s * 2), 12, TILE_STAR);
    win_text(5, 15, "PRESS A");
    win_mode = W_SELECT;
}

/* The boot title. All background layer, so the window can slide out from
 * under it (HIDE_WIN) and the select screen can simply cover it up. The word
 * is the doubled font, mkgfx.py's TITLE_* tiles; everything else is the
 * game's own kit - the slingshot, an arc of dots, stars, a ground line - so
 * it ages without new art. Advance is the main loop's job. */
static void bkg_text(uint8_t x, uint8_t y, const char *s)
{
    while (*s) set_bkg_tile_xy(x++, y, char_tile(*s++));
}

static void show_title(void)
{
    const uint8_t arc_x[] = { 5, 7, 9, 11, 13 };
    const int8_t arc_y[] = { 10, 9, 8, 7, 6 };
    uint8_t x, t;

    for (t = 0; t < TITLE_COLS * TITLE_ROWS; t++)
        set_bkg_tile_xy((uint8_t)(6 + t % TITLE_COLS), (uint8_t)(2 + t / TITLE_COLS),
                        (uint8_t)(TILE_TITLE_0 + t));
    set_bkg_tile_xy(9, 5, TILE_STAR);
    set_bkg_tile_xy(11, 5, TILE_STAR);
    set_bkg_tile_xy(13, 5, TILE_STAR);
    draw_sling();
    for (t = 0; t < NUM_DOTS; t++)
        set_bkg_tile_xy(arc_x[t], (uint8_t)arc_y[t], TILE_DOT);
    set_sprite_tile(SPR_BALL, TILE_BALL);
    place_sprite(SPR_BALL, LAUNCH_X, LAUNCH_Y);
    for (x = 0; x < GRID_W; x++) {
        set_bkg_tile_xy(x, 16, TILE_GROUND_TOP);
        set_bkg_tile_xy(x, 17, TILE_GROUND);
    }
    bkg_text(5, 14, "PRESS START");
}

/* ---------- game flow ---------- */

static void start_level(uint8_t n)
{
    level = n;
    sim_load_ascii(&sim, level_rows[n], level_height[n], level_shots[n]);
    hide_falls();
    hide_puffs();
    shake = 0;
    draw_all();
    angle = AIM_ANGLE;
    power = AIM_POWER;
    hold = 0;
    last_dirs = 0;
    arc_dirty = 1;
    move_sprite(SPR_BALL, 0, 0);
    /* Force the next update_hud through its change check. */
    hud_shots = 0xFF;
    hud_targets = 0xFF;
    hud_type = 0xFF;
    hud_score = 0xFFFF;
    chain_frames = 0;
    win_mode = W_HUD;
    update_hud();
    last_state = sim.state;
}

static void handle_aim(void)
{
    uint8_t dirs = keys & DIRS;
    uint8_t act = 0;

    if (dirs != last_dirs) {
        hold = 0;
        act = dirs;
    } else if (dirs) {
        hold++;
        if (hold >= REPEAT_DELAY && (hold & REPEAT_MASK) == 0) act = dirs;
    }
    last_dirs = dirs;

    if ((act & J_UP) && angle < NUM_ANGLES - 1) { angle++; arc_dirty = 1; }
    if ((act & J_DOWN) && angle > 0)            { angle--; arc_dirty = 1; }
    if ((act & J_RIGHT) && power < NUM_POWERS - 1) { power++; arc_dirty = 1; }
    if ((act & J_LEFT) && power > 0)            { power--; arc_dirty = 1; }

    if ((keys & J_B) && !(prev_keys & J_B)) {
        ptype = (uint8_t)((ptype + 1) % PT_COUNT);
    }

    if ((keys & J_A) && !(prev_keys & J_A)) {
        sim.ptype = ptype;
        if (sim_fire(&sim, angle, power)) {
            hide_arc();
            sfx_launch();
            update_hud();          /* shots left changes the moment you fire */
        }
    }
}

/* The ball, plus the two shards a PT_SPLIT shot fans out. The preview dots
 * share these sprites, but the arc is hidden for the whole of a flight. */
static void draw_proj(void)
{
    uint8_t i, n = sim.pstate == P_FLYING ? sim.nproj : 0;
    if (n) {
        set_sprite_tile(SPR_BALL, ball_tile[sim.ptype]);
        place_sprite(SPR_BALL, sim.p[0].x, sim.p[0].y);
    } else move_sprite(SPR_BALL, 0, 0);
    for (i = 1; i < MAX_PROJ; i++) {
        if (i < n) {
            set_sprite_tile(SPR_DOT0 + i - 1, ball_tile[sim.ptype]);
            place_sprite(SPR_DOT0 + i - 1, sim.p[i].x, sim.p[i].y);
        } else {
            move_sprite(SPR_DOT0 + i - 1, 0, 0);
        }
    }
}

/* Dotted preview using the same step as the real flight. It ignores the
 * structure, so it shows where the shot would go if nothing were in the way. */
static void update_arc(void)
{
    Proj pr;
    uint8_t d, f;
    proj_launch(&pr, angle, power);
    for (d = 0; d < NUM_DOTS; d++) {
        for (f = 0; f < DOT_EVERY; f++) proj_ballistic_frame(&pr);
        if (pr.y >= GROUND_ROW * 8 || pr.x >= SCREEN_W) break;
        set_sprite_tile(SPR_DOT0 + d, TILE_DOT);   /* a split may have left a ball here */
        place_sprite(SPR_DOT0 + d, pr.x, pr.y);
    }
    for (; d < NUM_DOTS; d++) move_sprite(SPR_DOT0 + d, 0, 0);
}

void main(void)
{
    DISPLAY_OFF;
    BGP_REG = 0xE4;
    OBP0_REG = 0xE4;
    sfx_init();

    /* Background and sprites share tile slots, so load the same set to both. */
    set_bkg_data(0, GFX_TILE_COUNT, gfx_tiles);
    set_sprite_data(0, GFX_TILE_COUNT, gfx_tiles);

    /* The background map is 32 tiles wide and draw_all() only ever writes the
     * 20x18 play field, so anything that scrolls it would show whatever VRAM
     * held. Blank the lot once here and the screen shake has a margin to move
     * into. The display is still off, so this is cheap. */
    fill_bkg_rect(0, 0, 32, 32, TILE_BLANK);

    set_sprite_tile(SPR_BALL, TILE_BALL);
    {
        uint8_t d;
        for (d = 0; d < NUM_DOTS; d++) set_sprite_tile(SPR_DOT0 + d, TILE_DOT);
        for (d = 0; d < MAX_PUFFS; d++) set_sprite_tile(SPR_PUFF0 + d, TILE_PUFF);
    }

    keys = 0;
    prev_keys = 0;
    last_state = 0xFF;
    load_stars();
    /* Start the cursor on the first level without stars: resume where you left off. */
    cursor = 0;
    while (cursor + 1 < NUM_LEVELS && stars[cursor]) cursor++;
    menu = 1;                   /* the select screen is where the title leads */
    title = 1;
    show_title();
    sfx_title();

    SHOW_BKG;
    SHOW_SPRITES;
    HIDE_WIN;                   /* the title is background-only */
    DISPLAY_ON;

    while (1) {
        vsync();
        /* Screen shake. Written here, in the blanking period, because move_bkg
         * pokes SCX/SCY directly rather than through a shadow. Only the
         * background moves: the window layer is above it and stays put. */
        if (shake) {
            int8_t d = shake_off[SHAKE_FRAMES - shake];
            move_bkg((uint8_t)d, (uint8_t)d);
            shake--;
        } else {
            move_bkg(0, 0);
        }
        prev_keys = keys;
        keys = joypad();
        sfx_frame();

        /* Aged here rather than in the flight branch: a shot that ends while a
         * speck is still alive leaves the state machine, and anything aged only
         * there would freeze on screen until the next shot. Every path through
         * the loop reaches this line, so no puff can outlive PUFF_LIFE. */
        age_puffs();

        /* The boot title waits for the button the prompt names: A or START,
         * like the splash and the select screen. It never times out. */
        if (title) {
            if (((keys & J_START) && !(prev_keys & J_START)) ||
                ((keys & J_A) && !(prev_keys & J_A))) {
                title = 0;
                sfx_title_off();
                SHOW_WIN;
                show_select();
            }
            continue;
        }

        if (menu) {
            if ((keys & J_LEFT) && !(prev_keys & J_LEFT) && cursor > 0) {
                cursor--;
                show_select();
            }
            if ((keys & J_RIGHT) && !(prev_keys & J_RIGHT) && cursor + 1 < NUM_LEVELS) {
                cursor++;
                show_select();
            }
            /* START works too: the win splash has just taught the player to press
             * it, and this is the screen that follows the last level. */
            if (((keys & J_A) && !(prev_keys & J_A)) ||
                ((keys & J_START) && !(prev_keys & J_START))) {
                menu = 0;
                start_level(cursor);
            }
            continue;
        }

        if ((keys & J_START) && !(prev_keys & J_START)) {
            if (sim.state == S_WON) {
                if (sim_stars(&sim) > stars[level]) {
                    stars[level] = sim_stars(&sim);
                    save_stars();
                }
                if (level + 1 < NUM_LEVELS) {
                    start_level((uint8_t)(level + 1));   /* straight on to the next */
                } else {
                    cursor = level;
                    menu = 1;
                    show_select();
                }
            } else {
                start_level(level);                      /* retry the same level */
            }
            continue;
        }

        if ((keys & J_SELECT) && !(prev_keys & J_SELECT)) {
            cursor = level;
            menu = 1;
            show_select();
            continue;
        }

        if (sim.state == S_AIM) {
            handle_aim();
            if (sim.state == S_AIM && arc_dirty) {
                update_arc();
                arc_dirty = 0;
            }
            if (sim.state == S_AIM) {
                set_sprite_tile(SPR_BALL, ball_tile[ptype]);
                place_sprite(SPR_BALL, LAUNCH_X, LAUNCH_Y);
            }
        } else if (sim.state == S_FLIGHT || sim.state == S_SETTLE) {
            uint8_t was_flying = sim.state == S_FLIGHT;
            if ((keys & J_A) && !(prev_keys & J_A) && sim_trigger(&sim)) sfx_blip();
            sim_tick(&sim);
            sfx_hit_events();
            shake_events();
            if (sim.chain_points) {
                chain_shown = sim.chain_points;     /* 0 next frame; see chain_flash */
                chain_frames = CHAIN_FRAMES;
            }
            age_falls();        /* after the step, so releases land on a fresh grid */
            spawn_falls();
            /* the two puff warrants: the airborne ball's hit, and the fall
             * landing's own destruction, each at its own ground */
            if (was_flying && sim.hit_mat != M_EMPTY) spawn_puffs(sim.p[0].x, sim.p[0].y);
            if (sim.chain_points) spawn_puffs(
                (int16_t)(sim.chain_x * 8 + 4), (int16_t)(sim.chain_y * 8 + 4));
            redraw_changes();
            draw_proj();
        } else {
            move_sprite(SPR_BALL, 0, 0);
            hide_arc();
        }

        /* SDCC's gbz80 backend leaves A holding `sim.state - last_state` after
         * the `!=` below but still believes A is sim.state, so a plain local
         * would be assigned the difference: on a win (3 - 2 = 1) that matches
         * neither S_WON nor S_LOST and the splash never draws. sim.state is
         * declared volatile in sim.h, so initializing st is a real memory
         * load the backend cannot satisfy from that accumulator; the volatile
         * on the local also keeps each test below reading through memory. */
        {
            volatile uint8_t st = sim.state;
            if (st != last_state) {
                if (st == S_WON || st == S_LOST) {
                    hide_arc();
                    hide_falls();
                    hide_puffs();
                    shake = 0;
                    chain_frames = 0;   /* the splash owns the window now */
                    if (st == S_WON) sfx_jingle(win_notes, 4);
                    else sfx_jingle(lose_notes, 3);
                    show_splash();
                } else if (st == S_AIM) {
                    arc_dirty = 1;      /* the shot is over, draw the preview again */
                }
                last_state = st;
            }
        }
        chain_flash();
        update_hud();
    }
}
