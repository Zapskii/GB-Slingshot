/*
 * sim.h - GB-Slingshot game logic. No hardware calls, so it builds on the
 * host (make test) and under GBDK-2020 / SDCC.
 *
 * Positions are pixels plus an 8-bit fraction (a 16-bit pixel value cannot
 * hold 160 px at 8.8). Velocities and gravity are 8.8 fixed point, in
 * pixels per frame. No floating point, no 32-bit maths.
 *
 * All functions are static: include this header from one translation unit.
 */
#ifndef SIM_H
#define SIM_H

#include <stddef.h>
#include <stdint.h>
#include "tables.h"

#define GRID_W 20
#define GRID_H 18
#define GRID_CELLS 360
#define GROUND_ROW 16           /* rows 16 and 17 are indestructible ground */
#define SCREEN_W 160
#define SCREEN_H 144

#define LAUNCH_X 24
#define LAUNCH_Y 100
#define GRAVITY 8               /* 8.8: 0.03125 px/frame^2 */
#define COLLAPSE_INTERVAL 4     /* ticks between collapse steps */
#define MAX_FLIGHT_FRAMES 900

/* Cells the front end will animate in one collapse step. Moves past this are
 * left out of the log, so they snap into place instead of sliding: the sprite
 * budget and the per-frame cost stay bounded. */
#define MAX_FALL_MOVES 8

/* Material ids double as tile ids, see tools/mkgfx.py. */
enum { M_EMPTY = 0, M_WOOD, M_STONE, M_ICE, M_TARGET, M_GROUND, M_COUNT };
enum { S_AIM = 0, S_FLIGHT, S_SETTLE, S_WON, S_LOST };
enum { P_IDLE = 0, P_FLYING, P_SPENT };

/* Projectile types. The ability fires on a second A press while in flight. */
enum { PT_STANDARD = 0, PT_BOOST, PT_SPLIT, PT_PIERCE, PT_COUNT };

/* A shot can be more than one projectile at once: PT_SPLIT fans into three.
 * p[0] is always the one the front end steers its ball sprite with. */
#define MAX_PROJ 3

/* How hard the split fans out, as a right shift: the followers get vx +/- vy>>N.
 * A cheap rotation - no trig table, and the shift keeps it inside int16. */
#define SPLIT_NUDGE 3

/* Speed a shot loses for each cell it breaks through, as a right shift: 2 is a
 * quarter. The damage buckets are only about 1.4x apart, so the old halving
 * (shift 1) cost two of the four buckets at once and a shot that punched through
 * two glass bricks arrived in the weakest one, able to do no more than dent the
 * target behind them. A knob, tuned by playing, not derived. */
#define BREAK_SLOW_SHIFT 2

/* Starting hit points. Damage taken = (raw * mat_mul) >> 1, so mul 2 is
 * normal, 1 is half (stone) and 4 is double (ice). */
static const uint8_t mat_hp[M_COUNT]    = { 0, 4, 4, 2, 2, 255 };
static const uint8_t mat_mul[M_COUNT]   = { 0, 2, 1, 4, 2, 0 };
static const uint8_t mat_score[M_COUNT] = { 0, 10, 20, 5, 100, 0 };

typedef struct {
    int16_t x, y;       /* whole pixels */
    uint8_t xf, yf;     /* fractions, 1/256 px */
    int16_t vx, vy;     /* 8.8 px per frame */
} Proj;

/* One cell that fell a row during a collapse step: (x, y) moved to (x, y+1).
 * sim_collapse_step writes these; the front end slides a sprite down over
 * COLLAPSE_INTERVAL frames so a collapse reads as gravity, not a teleport. */
typedef struct {
    uint8_t x, y, mat;
} FallMove;

typedef struct {
    uint8_t mat[GRID_CELLS];
    uint8_t hp[GRID_CELLS];
    uint8_t fall[GRID_CELLS];   /* cells fallen since last supported */
    Proj p[MAX_PROJ];           /* p[0] is live for every shot; a split uses all three */
    uint8_t ptype;              /* PT_*: chosen before firing */
    uint8_t pused;              /* ability already spent this shot */
    uint8_t nproj;              /* projectiles in flight this shot, 1..MAX_PROJ */
    uint8_t spent;              /* bit i set: p[i] has finished */
    uint8_t pstate;
    /* volatile: SDCC's gbz80 backend clobbers the accumulator after a compare
     * but still believes it holds this field, so any cached/plain read of it
     * can be seeded with the difference instead of the value. See the
     * compiler note in the README. Layout is unchanged, so the byte offsets
     * tests/emu_replay.py reads still hold. */
    volatile uint8_t state;
    uint8_t shots_left;
    uint8_t shots_total;
    uint8_t targets_left;
    uint8_t settle_timer;
    uint16_t score;
    uint16_t flight_frames;
    /* What the front end should show this frame: points just added for cells
     * destroyed by the collapse (scored double, the chain bonus), 0 for
     * nothing. Like hit_mat this is data, not a hardware call. Cleared at
     * the top of sim_tick. */
    uint16_t chain_points;
    /* tests/emu_replay.py reads this struct by byte offset, but the offsets
     * are regenerated mechanically on every emu-test run by
     * tests/sim_offsets_probe.c (into build/sim_offsets.json), so new fields
     * may go anywhere. The emulator checks that refer to a field must still
     * be updated if its meaning changes. */
    uint8_t nmoves;                     /* valid until the front end consumes them */
    FallMove moves[MAX_FALL_MOVES];
    /* What the front end should voice this frame: the material of the last cell
     * damage_cell() touched, 0 for nothing. Sound is the front end's business,
     * so this is data, not a hardware call. Cleared at the top of sim_tick. */
    uint8_t hit_mat;
    /* The first cell a fall landing destroyed this frame: the puff's ground
     * position (column, row). Valid while chain_points is nonzero; cleared
     * by sim_tick like hit_mat, never by sim_collapse_step — so a host test
     * stepping the collapse directly must tick before asserting these. */
    uint8_t chain_x, chain_y;
} Sim;

/* The ROM builds with SDCC, while tests/sim_offsets_probe.c regenerates the
 * emulator test's field offsets from this header on the host. The two
 * layouts agree only while every 2-byte field keeps an even offset (no
 * struct padding), so hold that invariant by assertion: a new 2-byte field
 * inserted behind a 1-byte field stops the build here instead of drifting
 * the emulator checks out of sync. Negative-array typedefs, because a
 * failed _Static_assert is only a warning on SDCC. */
typedef char sim_align_p_2byte[(offsetof(Sim, p) % 2 == 0) * 2 - 1];
typedef char sim_align_proj_2byte[(offsetof(Sim, p[1].vx) % 2 == 0) * 2 - 1];
typedef char sim_align_score_2byte[(offsetof(Sim, score) % 2 == 0) * 2 - 1];
typedef char sim_align_flight_2byte[(offsetof(Sim, flight_frames) % 2 == 0) * 2 - 1];
typedef char sim_align_chain_2byte[(offsetof(Sim, chain_points) % 2 == 0) * 2 - 1];

/* ---------- fixed point and projectile motion ---------- */

static void add_fixed(int16_t *pix, uint8_t *frac, int16_t v)
{
    uint16_t f = (uint16_t)*frac + (uint16_t)(v & 0xFF);
    *pix = (int16_t)(*pix + (v >> 8) + (int16_t)(f >> 8));
    *frac = (uint8_t)(f & 0xFF);
}

static void proj_half_move(Proj *p)
{
    add_fixed(&p->x, &p->xf, (int16_t)(p->vx >> 1));
    add_fixed(&p->y, &p->yf, (int16_t)(p->vy >> 1));
}

/* One frame of flight with no collision. The aim preview uses this too, so
 * the dotted arc matches the real flight exactly. */
static void proj_ballistic_frame(Proj *p)
{
    p->vy += GRAVITY;
    proj_half_move(p);
    proj_half_move(p);
}

static void proj_launch(Proj *p, uint8_t angle, uint8_t power)
{
    uint8_t i = (uint8_t)(angle * NUM_POWERS + power);
    p->x = LAUNCH_X;
    p->y = LAUNCH_Y;
    p->xf = 0;
    p->yf = 0;
    p->vx = launch_vx[i];
    p->vy = launch_vy[i];
}

/* Cheap speed estimate (max + min/2) bucketed 1..4. */
static uint8_t speed_bucket(int16_t vx, int16_t vy)
{
    uint16_t ax = (uint16_t)(vx < 0 ? -vx : vx);
    uint16_t ay = (uint16_t)(vy < 0 ? -vy : vy);
    uint16_t sp = ax > ay ? (uint16_t)(ax + (ay >> 1)) : (uint16_t)(ay + (ax >> 1));
    if (sp < 0x0180) return 1;
    if (sp < 0x0230) return 2;
    if (sp < 0x0300) return 3;
    return 4;
}

/* ---------- grid ---------- */

static void sim_init(Sim *s, uint8_t shots)
{
    uint16_t i;
    for (i = 0; i < GRID_CELLS; i++) {
        s->mat[i] = M_EMPTY;
        s->hp[i] = 0;
        s->fall[i] = 0;
    }
    for (i = (uint16_t)GROUND_ROW * GRID_W; i < GRID_CELLS; i++) {
        s->mat[i] = M_GROUND;
        s->hp[i] = 255;
    }
    s->p[0].x = LAUNCH_X;
    s->p[0].y = LAUNCH_Y;
    s->p[0].xf = 0;
    s->p[0].yf = 0;
    s->p[0].vx = 0;
    s->p[0].vy = 0;
    s->ptype = PT_STANDARD;
    s->pused = 0;
    s->nproj = 1;
    s->spent = 0;
    s->pstate = P_IDLE;
    s->state = S_AIM;
    s->shots_left = shots;
    s->shots_total = shots;
    s->targets_left = 0;
    s->settle_timer = 0;
    s->score = 0;
    s->flight_frames = 0;
    s->nmoves = 0;
    s->hit_mat = M_EMPTY;
    s->chain_x = 0xFF;
    s->chain_y = 0xFF;
}

static void sim_set_cell(Sim *s, uint8_t x, uint8_t y, uint8_t m)
{
    uint16_t idx = (uint16_t)y * GRID_W + x;
    if (s->mat[idx] == M_TARGET && m != M_TARGET) s->targets_left--;
    if (s->mat[idx] != M_TARGET && m == M_TARGET) s->targets_left++;
    s->mat[idx] = m;
    s->hp[idx] = mat_hp[m];
    s->fall[idx] = 0;
}

/* Load ASCII rows aligned to the bottom (the last row sits just above the
 * ground). W wood, S stone, I ice, T target, anything else empty. */
static void sim_load_ascii(Sim *s, const char *const *rows, uint8_t n, uint8_t shots)
{
    uint8_t i, x;
    sim_init(s, shots);
    for (i = 0; i < n; i++) {
        uint8_t y = (uint8_t)(GROUND_ROW - n + i);
        for (x = 0; x < GRID_W; x++) {
            char c = rows[i][x];
            if (c == 0) break;
            if (c == 'W') sim_set_cell(s, x, y, M_WOOD);
            else if (c == 'S') sim_set_cell(s, x, y, M_STONE);
            else if (c == 'I') sim_set_cell(s, x, y, M_ICE);
            else if (c == 'T') sim_set_cell(s, x, y, M_TARGET);
        }
    }
}

static void destroy_cell(Sim *s, uint16_t idx)
{
    uint8_t m = s->mat[idx];
    s->score = (uint16_t)(s->score + mat_score[m]);
    if (m == M_TARGET) s->targets_left--;
    s->mat[idx] = M_EMPTY;
    s->hp[idx] = 0;
    s->fall[idx] = 0;
}

/* Returns 1 if the cell was destroyed. */
static uint8_t damage_cell(Sim *s, uint16_t idx, uint8_t raw)
{
    uint8_t m = s->mat[idx];
    uint16_t d;
    if (m == M_EMPTY || m == M_GROUND) return 0;
    d = (uint16_t)(((uint16_t)raw * mat_mul[m]) >> 1);
    if (d == 0) return 0;
    s->hit_mat = m;             /* the front end voices this as a crunch */
    if (d >= s->hp[idx]) {
        destroy_cell(s, idx);
        return 1;
    }
    s->hp[idx] = (uint8_t)(s->hp[idx] - d);
    return 0;
}

/* ---------- collapse ---------- */

/* One collapse step, bottom row first. A cell with nothing below falls one
 * cell unless both horizontal neighbours are solid (bridges hold). A cell
 * that lands after falling 2+ cells damages itself and what it hit. A stack
 * that falls together does not damage itself. Returns activity (moves plus
 * landings); 0 means the structure is stable.
 *
 * Each downward move is also logged in s->moves for the front end to animate.
 * The log is a hint only: leaving moves out of it changes nothing here. */
static uint8_t sim_collapse_step(Sim *s)
{
    uint8_t activity = 0;
    uint8_t mv[GRID_W], prev_land[GRID_W], cur_land[GRID_W];
    uint8_t r, x, m, f;
    uint16_t idx, bidx;

    s->nmoves = 0;
    for (x = 0; x < GRID_W; x++) prev_land[x] = 0;

    r = GROUND_ROW;
    while (r > 0) {
        r--;
        for (x = 0; x < GRID_W; x++) {
            mv[x] = 0;
            cur_land[x] = 0;
            idx = (uint16_t)r * GRID_W + x;
            m = s->mat[idx];
            if (m == M_EMPTY || m == M_GROUND) continue;
            bidx = (uint16_t)(idx + GRID_W);
            if (s->mat[bidx] == M_EMPTY) {
                if (x > 0 && x < GRID_W - 1 &&
                    s->mat[idx - 1] != M_EMPTY && s->mat[idx + 1] != M_EMPTY) {
                    s->fall[idx] = 0;       /* held by its neighbours */
                } else {
                    mv[x] = 1;
                }
            } else if (s->fall[idx] > 0) {
                if (prev_land[x]) {
                    s->fall[idx] = 0;       /* rode a stack down */
                    cur_land[x] = 1;
                } else {
                    mv[x] = 2;
                }
            }
        }
        for (x = 0; x < GRID_W; x++) {
            idx = (uint16_t)r * GRID_W + x;
            bidx = (uint16_t)(idx + GRID_W);
            if (mv[x] == 1) {
                if (s->nmoves < MAX_FALL_MOVES) {
                    FallMove *fm = &s->moves[s->nmoves++];
                    fm->x = x;
                    fm->y = r;
                    fm->mat = s->mat[idx];
                }
                s->mat[bidx] = s->mat[idx];
                s->hp[bidx] = s->hp[idx];
                s->fall[bidx] = s->fall[idx] < 255 ? (uint8_t)(s->fall[idx] + 1) : 255;
                s->mat[idx] = M_EMPTY;
                s->hp[idx] = 0;
                s->fall[idx] = 0;
                activity++;
            } else if (mv[x] == 2) {
                f = s->fall[idx];
                s->fall[idx] = 0;
                cur_land[x] = 1;
                activity++;
                if (f >= 2) {
                    /* A fall that destroys cells scores them again: the
                     * chain bonus. Recorded in chain_points for the flash. */
                    uint16_t before = s->score;
                    damage_cell(s, bidx, (uint8_t)(f - 1));
                    damage_cell(s, idx, (uint8_t)(f - 1));
                    s->chain_points = (uint16_t)(s->chain_points + (s->score - before));
                    s->score = (uint16_t)(s->score + (s->score - before));
                    /* The first destroyed cell of the frame points at the
                     * dust: one puff burst is all the sprite budget allows. */
                    if (s->score - before && s->chain_x == 0xFF) {
                        s->chain_x = x;
                        s->chain_y = r;
                    }
                }
            }
        }
        for (x = 0; x < GRID_W; x++) prev_land[x] = cur_land[x];
    }
    return activity;
}

/* ---------- flight ---------- */

/* Check projectile i against the grid. Returns 1 while it is still flying. */
static uint8_t proj_check_collision(Sim *s, uint8_t i)
{
    Proj *p = &s->p[i];
    int16_t px = p->x, py = p->y;
    uint16_t idx;
    uint8_t bucket;

    if (px < 0 || px >= SCREEN_W || py >= SCREEN_H) return 0;
    if (py < 0) return 1;       /* above the screen: nothing to hit */

    idx = (uint16_t)((py >> 3) * GRID_W + (px >> 3));
    if (s->mat[idx] != M_EMPTY) {
        bucket = speed_bucket(p->vx, p->vy);
        if (damage_cell(s, idx, bucket)) {
            /* broke through, but slower */
            p->vx = (int16_t)(p->vx - (p->vx >> BREAK_SLOW_SHIFT));
            p->vy = (int16_t)(p->vy - (p->vy >> BREAK_SLOW_SHIFT));
        } else if (s->ptype == PT_PIERCE && s->pused && s->mat[idx] != M_GROUND) {
            /* drill: with the ability spent, the ball keeps flying through
             * the cell it dented. The same cell takes the dent again on
             * every pass, so stone dies after two heavy overlaps. Ground
             * has zero damage in the table and stops the ball as ever. */
            p->vx = (int16_t)(p->vx - (p->vx >> BREAK_SLOW_SHIFT));
            p->vy = (int16_t)(p->vy - (p->vy >> BREAK_SLOW_SHIFT));
        } else {
            return 0;
        }
    }
    return 1;
}

static void sim_flight_frame(Sim *s)
{
    uint8_t i, flying = 0;
    for (i = 0; i < s->nproj; i++) {
        Proj *p = &s->p[i];
        if (s->spent & (uint8_t)(1 << i)) continue;
        p->vy += GRAVITY;
        proj_half_move(p);
        if (proj_check_collision(s, i)) {
            proj_half_move(p);
            if (proj_check_collision(s, i)) flying = 1;
            else s->spent |= (uint8_t)(1 << i);
        } else {
            s->spent |= (uint8_t)(1 << i);
        }
    }
    /* The shot ends when every projectile is done, or the frame budget runs out. */
    s->flight_frames++;
    if (!flying || s->flight_frames > MAX_FLIGHT_FRAMES) s->pstate = P_SPENT;
}

/* ---------- game flow ---------- */

static uint8_t sim_fire(Sim *s, uint8_t angle, uint8_t power)
{
    if (s->state != S_AIM || s->shots_left == 0) return 0;
    if (angle >= NUM_ANGLES || power >= NUM_POWERS) return 0;
    proj_launch(&s->p[0], angle, power);
    s->nproj = 1;
    s->spent = 0;
    s->pused = 0;
    s->pstate = P_FLYING;
    s->state = S_FLIGHT;
    s->flight_frames = 0;
    s->settle_timer = 0;
    s->shots_left--;
    return 1;
}

/* The second A press. Each projectile type spends its one ability here:
 * PT_BOOST speeds the ball up (so it hits harder), PT_SPLIT fans it into
 * three, PT_PIERCE turns it into a drill for the rest of the flight.
 * Returns 1 if something happened. */
static uint8_t sim_trigger(Sim *s)
{
    Proj *p;
    if (s->state != S_FLIGHT || s->pused) return 0;
    p = &s->p[0];
    if (s->ptype == PT_BOOST) {
        s->pused = 1;
        p->vx = (int16_t)(p->vx + (p->vx >> 1));
        p->vy = (int16_t)(p->vy + (p->vy >> 1));
        return 1;
    }
    if (s->ptype == PT_SPLIT) {
        int16_t x = p->x, y = p->y, vx = p->vx, vy = p->vy;
        uint8_t xf = p->xf, yf = p->yf, i;
        s->pused = 1;
        for (i = 1; i < MAX_PROJ; i++) {
            Proj *q = &s->p[i];
            /* Two rotations of the launch velocity, opposite ways. */
            q->x = x;
            q->y = y;
            q->xf = xf;
            q->yf = yf;
            q->vx = i == 1 ? (int16_t)(vx - (vy >> SPLIT_NUDGE))
                           : (int16_t)(vx + (vy >> SPLIT_NUDGE));
            q->vy = i == 1 ? (int16_t)(vy + (vx >> SPLIT_NUDGE))
                           : (int16_t)(vy - (vx >> SPLIT_NUDGE));
        }
        s->nproj = MAX_PROJ;
        return 1;
    }
    if (s->ptype == PT_PIERCE) {
        /* PT_PIERCE's ability: from here on (see proj_check_collision) the
         * ball drills through cells it dents without stopping. */
        s->pused = 1;
        return 1;
    }
    return 0;
}

/* Advance one frame. */
static void sim_tick(Sim *s)
{
    s->hit_mat = M_EMPTY;       /* one frame of events: the front end reads them now */
    s->chain_points = 0;
    s->chain_x = 0xFF;
    s->chain_y = 0xFF;
    if (s->state == S_FLIGHT) {
        sim_flight_frame(s);
        if (s->pstate == P_SPENT) {
            s->state = S_SETTLE;
            s->settle_timer = 0;
        }
    } else if (s->state == S_SETTLE) {
        if (++s->settle_timer >= COLLAPSE_INTERVAL) {
            s->settle_timer = 0;
            if (sim_collapse_step(s) == 0) {
                if (s->targets_left == 0) s->state = S_WON;
                else if (s->shots_left == 0) s->state = S_LOST;
                else s->state = S_AIM;
            }
        }
    }
}

/* 0 until won; then 1 to 3 depending on shots left over. */
static uint8_t sim_stars(const Sim *s)
{
    if (s->state != S_WON) return 0;
    if (s->shots_left >= 2) return 3;
    if (s->shots_left >= 1) return 2;
    return 1;
}

#endif
