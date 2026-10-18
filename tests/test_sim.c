/* Host-side tests for src/sim.h. Build and run with `make test`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sim.h"
#include "levels.h"

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define RUN(fn) do { int before = g_fail; fn(); \
    printf("%s  %s\n", g_fail == before ? "ok  " : "FAIL", #fn); } while (0)

#define AT(s, x, y) ((s)->mat[(y) * GRID_W + (x)])
#define HP(s, x, y) ((s)->hp[(y) * GRID_W + (x)])

/* ---------- helpers ---------- */

static int run_idle(Sim *s, int max_ticks)
{
    int t = 0;
    while ((s->state == S_FLIGHT || s->state == S_SETTLE) && t < max_ticks) {
        sim_tick(s);
        t++;
    }
    return t;
}

static int settle_grid(Sim *s)
{
    int steps = 0;
    while (sim_collapse_step(s) && steps < 500) steps++;
    return steps;
}

static uint32_t hash_grid(const Sim *s)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < GRID_CELLS; i++) {
        h ^= s->mat[i]; h *= 16777619u;
        h ^= s->hp[i];  h *= 16777619u;
    }
    return h;
}

static int count_cells(const Sim *s, uint8_t m)
{
    int i, n = 0;
    for (i = 0; i < GRID_CELLS; i++) if (s->mat[i] == m) n++;
    return n;
}

/* ---------- launch tables ---------- */

static void test_tables(void)
{
    int a, p;
    for (a = 0; a < NUM_ANGLES; a++) {
        for (p = 0; p < NUM_POWERS; p++) {
            int i = a * NUM_POWERS + p;
            CHECK(launch_vx[i] > 0);
            CHECK(launch_vy[i] < 0);
            if (p + 1 < NUM_POWERS) {
                CHECK(launch_vx[i + 1] > launch_vx[i]);
                CHECK(launch_vy[i + 1] < launch_vy[i]);
            }
            if (a + 1 < NUM_ANGLES) {
                CHECK(launch_vx[i + NUM_POWERS] < launch_vx[i]);
                CHECK(launch_vy[i + NUM_POWERS] < launch_vy[i]);
            }
        }
    }
    /* 45 degrees: equal horizontal and vertical speed */
    for (p = 0; p < NUM_POWERS; p++) {
        int i = 8 * NUM_POWERS + p;
        CHECK(ANGLE_MIN_DEG + 8 * ANGLE_STEP_DEG == 45);
        CHECK(launch_vx[i] == -launch_vy[i]);
    }
}

/* ---------- flight ---------- */

/* The split pixel+fraction position must match a plain 32-bit accumulator. */
static void test_fixed_point_matches_accumulator(void)
{
    int a, p, f;
    int bad = 0;
    for (a = 0; a < NUM_ANGLES; a++) {
        for (p = 0; p < NUM_POWERS; p++) {
            Proj pr;
            int32_t X = LAUNCH_X * 256, Y = LAUNCH_Y * 256;
            int16_t vx, vy;
            proj_launch(&pr, a, p);
            vx = pr.vx; vy = pr.vy;
            for (f = 0; f < 200; f++) {
                proj_ballistic_frame(&pr);
                vy += GRAVITY;
                X += (vx >> 1) + (vx >> 1);
                Y += (vy >> 1) + (vy >> 1);
                if (pr.x != (X >> 8) || pr.xf != (X & 0xFF) ||
                    pr.y != (Y >> 8) || pr.yf != (Y & 0xFF)) bad++;
            }
        }
    }
    CHECK(bad == 0);
}

/* Range back at launch height vs v^2 sin(2a) / g. */
static void test_range_matches_physics(void)
{
    int a, p;
    double worst = 0;
    for (a = 0; a < NUM_ANGLES; a++) {
        for (p = 0; p < NUM_POWERS; p++) {
            Proj pr;
            double vx, vy, v, th, g = GRAVITY / 256.0, expect, got, err;
            int f;
            proj_launch(&pr, a, p);
            vx = pr.vx / 256.0; vy = -pr.vy / 256.0;
            v = sqrt(vx * vx + vy * vy);
            th = atan2(vy, vx);
            expect = v * v * sin(2 * th) / g;
            for (f = 0; f < 1000; f++) {
                proj_ballistic_frame(&pr);
                if (pr.y >= LAUNCH_Y) break;
            }
            got = pr.x - LAUNCH_X;
            err = fabs(got - expect) - 4.0;     /* allow one frame of overshoot */
            if (err < 0) err = 0;
            if (expect > 1 && err / expect > worst) worst = err / expect;
            CHECK(err <= 0.05 * expect + 0.5);
        }
    }
    printf("      worst range error beyond 4 px: %.1f%%\n", worst * 100);
}

/* ---------- damage ---------- */

static void test_damage_materials(void)
{
    Sim s;
    sim_init(&s, 3);
    sim_set_cell(&s, 1, 15, M_WOOD);
    sim_set_cell(&s, 2, 15, M_STONE);
    sim_set_cell(&s, 3, 15, M_ICE);
    sim_set_cell(&s, 4, 15, M_TARGET);
    sim_set_cell(&s, 5, 15, M_WOOD);

    /* wood: a slow hit scratches, a fast hit breaks it */
    CHECK(damage_cell(&s, 15 * GRID_W + 1, 1) == 0);
    CHECK(HP(&s, 1, 15) == 3);
    CHECK(damage_cell(&s, 15 * GRID_W + 5, 4) == 1);
    CHECK(AT(&s, 5, 15) == M_EMPTY);

    /* stone: slow hits do nothing, fast hits take two shots */
    CHECK(damage_cell(&s, 15 * GRID_W + 2, 1) == 0);
    CHECK(HP(&s, 2, 15) == 4);
    CHECK(damage_cell(&s, 15 * GRID_W + 2, 4) == 0);
    CHECK(HP(&s, 2, 15) == 2);
    CHECK(damage_cell(&s, 15 * GRID_W + 2, 4) == 1);
    CHECK(AT(&s, 2, 15) == M_EMPTY);

    /* ice: shatters from any hit */
    CHECK(damage_cell(&s, 15 * GRID_W + 3, 1) == 1);
    CHECK(AT(&s, 3, 15) == M_EMPTY);

    /* target: two light hits or one hard one; destroying it scores */
    CHECK(s.targets_left == 1);
    CHECK(damage_cell(&s, 15 * GRID_W + 4, 1) == 0);
    CHECK(damage_cell(&s, 15 * GRID_W + 4, 2) == 1);
    CHECK(s.targets_left == 0);
    CHECK(s.score == 10 + 20 + 5 + 100);

    /* ground and empty space are immune */
    CHECK(damage_cell(&s, GROUND_ROW * GRID_W + 3, 255) == 0);
    CHECK(AT(&s, 3, GROUND_ROW) == M_GROUND);
    CHECK(damage_cell(&s, 14 * GRID_W + 3, 255) == 0);
}

/* The sim reports the material of the last cell it damaged, which is what the
 * front end voices as an impact. A dent counts: it is the common case, and it is
 * the only thing that tells the player their shot did anything at all. */
static void test_damage_reports_material(void)
{
    Sim s;
    sim_init(&s, 3);
    CHECK(s.hit_mat == M_EMPTY);            /* nothing has happened yet */

    sim_set_cell(&s, 1, 15, M_WOOD);
    sim_set_cell(&s, 2, 15, M_STONE);
    sim_set_cell(&s, 3, 15, M_ICE);

    CHECK(damage_cell(&s, 15 * GRID_W + 1, 1) == 0);    /* a dent */
    CHECK(s.hit_mat == M_WOOD);

    CHECK(damage_cell(&s, 15 * GRID_W + 3, 1) == 1);    /* a break */
    CHECK(s.hit_mat == M_ICE);

    CHECK(damage_cell(&s, 15 * GRID_W + 2, 1) == 0);    /* stone shrugs it off */
    CHECK(s.hit_mat == M_ICE);                          /* so it reports nothing */

    CHECK(damage_cell(&s, 14 * GRID_W + 5, 4) == 0);    /* empty space */
    CHECK(s.hit_mat == M_ICE);                          /* still nothing */

    /* One frame of events: the tick that follows clears it. */
    s.state = S_SETTLE;
    sim_tick(&s);
    CHECK(s.hit_mat == M_EMPTY);
}

static void test_speed_bucket(void)
{
    CHECK(speed_bucket(100, 0) == 1);
    CHECK(speed_bucket(0x0200, 0) == 2);
    CHECK(speed_bucket(0x0280, 0) == 3);
    CHECK(speed_bucket(0x0400, 0) == 4);
    CHECK(speed_bucket(-0x0400, 0) == 4);
    CHECK(speed_bucket(0, -0x0400) == 4);
}

/* ---------- shots hitting structures ---------- */

static const char *const wall_level[] = {
    "........S..........."
    , "........S..........."
    , "........S..........."
    , "........S..........."
    , "........S..........."
    , "........S..........."
    , "........S.........T."
};

static void test_shot_stops_at_stone(void)
{
    Sim s;
    sim_load_ascii(&s, wall_level, 7, 3);
    CHECK(sim_fire(&s, 0, 7) == 1);
    CHECK(s.state == S_FLIGHT && s.shots_left == 2);
    run_idle(&s, 5000);
    CHECK(s.state == S_AIM);
    CHECK(s.pstate == P_SPENT);
    CHECK(count_cells(&s, M_STONE) == 7);       /* wall still standing */
    CHECK(HP(&s, 8, 12) == 2);                  /* but dented */
}

static void test_shot_breaks_through_ice(void)
{
    static const char *const rows[] = {
        "........I...........",
        "........I...........",
        "........I...........",
        "........I...........",
        "........I...........",
        "........I...........",
        "........I........T..",
    };
    Sim s;
    int t, broke = 0;
    sim_load_ascii(&s, rows, 7, 3);
    CHECK(sim_fire(&s, 0, 7) == 1);
    for (t = 0; t < 5000 && s.state == S_FLIGHT; t++) {
        sim_tick(&s);
        if (AT(&s, 8, 12) == M_EMPTY && s.pstate == P_FLYING) broke = 1;
    }
    CHECK(broke);                               /* kept flying after the hit */
    CHECK(s.score >= 5);
}

/* Level 7's glass box: a 4x4 hollow ice cube with the target sitting inside it.
 * A shot has to break through the wall to reach the target, and this pins what
 * it is worth doing with afterwards. Halving the shot's speed on every break
 * cost two of the four damage buckets per cell, so *every* shot that reached
 * this target arrived in the weakest bucket -- 18 of 18 across the whole
 * angle/power table -- and the target could only ever be chipped, never
 * broken. */
static void test_shot_through_glass_still_kills(void)
{
    static const char *const rows[] = {
        "....................",
        "....................",
        "...........IIII.....",
        "...........I..I.....",
        "...........I.TI.....",
        "...........IIII.....",
    };
    Sim s;
    sim_load_ascii(&s, rows, 6, 3);
    CHECK(count_cells(&s, M_ICE) == 12);
    CHECK(sim_fire(&s, 2, 2) == 1);
    run_idle(&s, 5000);
    CHECK(count_cells(&s, M_ICE) < 12);     /* it punched through the wall */
    CHECK(s.targets_left == 0);             /* and still knocked the target out */
}

/* ---------- projectile types ---------- */

static const char *const shelf_level[] = {
    "....................",
    "....................",
    "....................",
    "....................",
    "....................",
    "........WWWWWWWWWWW.",
    "........WWWWWWWWWWWT"};

static void test_trigger_rules(void)
{
    Sim s;
    sim_load_ascii(&s, wall_level, 7, 3);
    CHECK(sim_trigger(&s) == 0);                /* nothing to boost while aiming */

    sim_fire(&s, 0, 7);
    CHECK(s.ptype == PT_STANDARD);
    CHECK(sim_trigger(&s) == 0);                /* the plain shot has no ability */
    CHECK(s.pused == 0);

    s.ptype = PT_BOOST;
    CHECK(sim_trigger(&s) == 1);
    CHECK(s.pused == 1);
    CHECK(sim_trigger(&s) == 0);                /* once per shot */
}

static void test_boost_hits_harder(void)
{
    Sim plain, boosted;
    sim_load_ascii(&plain, wall_level, 7, 3);
    sim_load_ascii(&boosted, wall_level, 7, 3);
    /* The same shot, boosted on the launch frame, arrives a speed bucket faster.
     * Stone takes half damage, so that bucket is the difference between one
     * point off the wall and two. Measured, see the power table in the plan. */
    CHECK(sim_fire(&plain, 0, 3) == 1);
    run_idle(&plain, 5000);
    boosted.ptype = PT_BOOST;
    CHECK(sim_fire(&boosted, 0, 3) == 1);
    CHECK(sim_trigger(&boosted) == 1);
    run_idle(&boosted, 5000);
    CHECK(HP(&plain, 8, 12) == 3);
    CHECK(HP(&boosted, 8, 12) == 2);
}

static void test_split_makes_three(void)
{
    Sim s, plain;
    int t, wood_plain, wood_split;
    sim_load_ascii(&plain, shelf_level, 7, 3);
    CHECK(sim_fire(&plain, 0, 6) == 1);
    run_idle(&plain, 5000);

    sim_load_ascii(&s, shelf_level, 7, 3);
    s.ptype = PT_SPLIT;
    CHECK(sim_fire(&s, 0, 6) == 1);
    CHECK(s.nproj == 1);
    for (t = 0; t < 33; t++) sim_tick(&s);      /* let the shot travel first */
    CHECK(sim_trigger(&s) == 1);
    CHECK(s.nproj == MAX_PROJ);
    CHECK(s.spent == 0);                        /* all three still in flight */
    CHECK(s.p[0].vx != s.p[1].vx || s.p[0].vy != s.p[1].vy);
    CHECK(s.p[0].vx != s.p[2].vx || s.p[0].vy != s.p[2].vy);
    run_idle(&s, 5000);

    wood_plain = count_cells(&plain, M_WOOD);
    wood_split = count_cells(&s, M_WOOD);
    CHECK(wood_split < wood_plain);             /* the fan hits more than one cell */
    CHECK(s.state == S_AIM && s.pstate == P_SPENT);
}

/* ---------- PT_PIERCE ---------- */

static int hp_sum(const Sim *s)
{
    int i, t = 0;
    for (i = 0; i < GRID_CELLS; i++) t += s->hp[i];
    return t;
}

/* The drill spends the one ability like every type: pused set, once per shot,
 * and no velocity is touched (unlike boost, which multiplies it). */
static void test_pierce_trigger_rules(void)
{
    Sim s;
    int16_t vx, vy;
    sim_load_ascii(&s, wall_level, 7, 3);
    s.ptype = PT_PIERCE;
    CHECK(sim_fire(&s, 0, 3) == 1);
    vx = s.p[0].vx;
    vy = s.p[0].vy;
    CHECK(sim_trigger(&s) == 1);
    CHECK(s.pused == 1);
    CHECK(s.p[0].vx == vx && s.p[0].vy == vy);  /* trigger moves no velocity */
    CHECK(sim_trigger(&s) == 0);                /* once per shot */
}

/* Without the trigger the pierce ball behaves exactly like the standard shot. */
static void test_pierce_untriggered_stops(void)
{
    Sim plain, pierce;
    sim_load_ascii(&plain, wall_level, 7, 3);
    sim_load_ascii(&pierce, wall_level, 7, 3);
    CHECK(sim_fire(&plain, 0, 3) == 1);
    run_idle(&plain, 5000);
    pierce.ptype = PT_PIERCE;
    CHECK(sim_fire(&pierce, 0, 3) == 1);
    run_idle(&pierce, 5000);                    /* no trigger: same stop at the wall */
    CHECK(count_cells(&pierce, M_STONE) == count_cells(&plain, M_STONE));
    CHECK(hp_sum(&pierce) == hp_sum(&plain));
    CHECK(pierce.pused == 0);
}

/* With the ability spent, the ball grinds the wall: strictly more damage than
 * the same unboosted shot, and at high power the drill destroys a stone the
 * plain shot only dents. Triggered on the launch frame, like the boost test. */
static void test_pierce_drills_through(void)
{
    Sim plain, drill;
    sim_load_ascii(&plain, wall_level, 7, 3);
    sim_load_ascii(&drill, wall_level, 7, 3);
    CHECK(sim_fire(&plain, 0, 7) == 1);
    run_idle(&plain, 5000);
    drill.ptype = PT_PIERCE;
    CHECK(sim_fire(&drill, 0, 7) == 1);
    CHECK(sim_trigger(&drill) == 1);
    run_idle(&drill, 5000);
    CHECK(count_cells(&drill, M_STONE) < count_cells(&plain, M_STONE));
    CHECK(hp_sum(&drill) < hp_sum(&plain));
}

/* The drill never tunnels through the ground: an arced shot lands and stops,
 * even with the ability spent. */
static void test_pierce_ground_holds(void)
{
    Sim drill;
    sim_load_ascii(&drill, wall_level, 7, 3);
    drill.ptype = PT_PIERCE;
    CHECK(sim_fire(&drill, 12, 3) == 1);
    CHECK(sim_trigger(&drill) == 1);
    run_idle(&drill, 5000);
    CHECK(drill.p[0].y < SCREEN_H);
    CHECK(count_cells(&drill, M_STONE) == 7);   /* nothing else was there to hit */
}

/* ---------- collapse ---------- */

static void test_empty_level_is_stable(void)
{
    Sim s;
    sim_init(&s, 1);
    CHECK(sim_collapse_step(&s) == 0);
}

static void test_stack_falls_together(void)
{
    Sim s;
    sim_init(&s, 1);
    sim_set_cell(&s, 5, 10, M_WOOD);
    sim_set_cell(&s, 5, 11, M_WOOD);
    sim_set_cell(&s, 5, 12, M_WOOD);
    CHECK(settle_grid(&s) < 100);
    CHECK(AT(&s, 5, 13) == M_WOOD);
    CHECK(AT(&s, 5, 14) == M_WOOD);
    CHECK(AT(&s, 5, 15) == M_WOOD);
    CHECK(count_cells(&s, M_WOOD) == 3);
    /* the stack lands as one: only the bottom block takes the 3-cell hit */
    CHECK(HP(&s, 5, 15) == 2);
    CHECK(HP(&s, 5, 14) == 4);
    CHECK(HP(&s, 5, 13) == 4);
    CHECK(sim_collapse_step(&s) == 0);
}

static void test_long_fall_destroys_cells(void)
{
    Sim s;
    sim_init(&s, 1);
    sim_set_cell(&s, 3, 2, M_WOOD);
    sim_set_cell(&s, 8, 2, M_STONE);
    sim_set_cell(&s, 12, 5, M_TARGET);
    settle_grid(&s);
    CHECK(count_cells(&s, M_WOOD) == 0);
    CHECK(count_cells(&s, M_STONE) == 0);
    CHECK(s.targets_left == 0);
    /* Collapse deaths pay out double: the chain bonus. */
    CHECK(s.score == 2 * (10 + 20 + 100));
}

/* The chain event itself: one fall, one landing, one death - the doubling
 * lands in chain_points exactly once and clears on the next tick. */
static void test_chain_bonus_event(void)
{
    Sim s;
    sim_init(&s, 1);
    sim_set_cell(&s, 3, 2, M_WOOD);     /* dies on its single landing */
    settle_grid(&s);
    CHECK(s.score == 2 * 10);           /* base payout plus the chain bonus */
    sim_tick(&s);
    CHECK(s.chain_points == 0);         /* cleared by the tick */
    /* A step that destroys nothing must not raise the event again. */
    sim_init(&s, 1);
    sim_set_cell(&s, 5, 15, M_WOOD);    /* propped, never falls */
    CHECK(settle_grid(&s) == 0);
    sim_tick(&s);
    CHECK(s.chain_points == 0);
}

/* The puff's ground position: the first cell a fall landing destroyed
 * reports its own (x, y), cleared by the tick like the other events, and a
 * mere dented landing reports nothing. */
static void test_chain_points_where(void)
{
    Sim s;
    sim_init(&s, 1);
    sim_set_cell(&s, 3, 2, M_WOOD);     /* dies at its landing on the ground */
    settle_grid(&s);
    CHECK(s.chain_points == 10);
    CHECK(s.chain_x == 3 && s.chain_y == 15);
    sim_tick(&s);
    CHECK(s.chain_x == 0xFF && s.chain_y == 0xFF);

    /* a fall that only dents: wood dropping two rows onto wood (raw 1 ->
     * damage 1 against hp 4) is damage without destruction, so no chain and
     * no dust position */
    sim_init(&s, 1);
    sim_set_cell(&s, 4, 15, M_WOOD);
    sim_set_cell(&s, 4, 13, M_WOOD);    /* falls two rows onto the support */
    settle_grid(&s);
    sim_tick(&s);
    CHECK(AT(&s, 4, 15) == M_WOOD && AT(&s, 4, 14) == M_WOOD);  /* fell together */
    CHECK(s.score == 0);                /* a dent pays nothing, so no bonus either */
    CHECK(s.chain_x == 0xFF && s.chain_y == 0xFF);
}

/* A landing damages the cell below before the falling cell, so when that one
 * is what broke, the dust is at (x, r+1), not under the faller. */
static void test_chain_points_when_below_dies(void)
{
    Sim s;
    sim_init(&s, 1);
    sim_set_cell(&s, 4, 15, M_ICE);     /* hp 2, mul 4: a raw 1 deals 2 and kills it */
    sim_set_cell(&s, 4, 12, M_WOOD);    /* falls three rows onto it */
    settle_grid(&s);
    CHECK(s.chain_points == 5);         /* ice scores 5, doubled into the score */
    CHECK(s.score == 10);
    CHECK(s.chain_x == 4 && s.chain_y == 15);   /* the cell that broke */
    CHECK(AT(&s, 4, 15) == M_WOOD && HP(&s, 4, 15) == 3); /* dented by the landing */
    sim_tick(&s);
    CHECK(s.chain_x == 0xFF && s.chain_y == 0xFF);
}

static void test_bridge_holds(void)
{
    Sim s;
    int x, y;
    sim_init(&s, 1);
    for (y = 12; y <= 15; y++) {
        sim_set_cell(&s, 3, y, M_STONE);
        sim_set_cell(&s, 7, y, M_STONE);
    }
    for (x = 3; x <= 7; x++) sim_set_cell(&s, x, 11, M_WOOD);
    CHECK(sim_collapse_step(&s) == 0);
    CHECK(AT(&s, 5, 11) == M_WOOD);
}

static void test_bridge_collapses_when_pier_removed(void)
{
    Sim s;
    int x, y;
    sim_init(&s, 1);
    for (y = 12; y <= 15; y++) {
        sim_set_cell(&s, 3, y, M_STONE);
        sim_set_cell(&s, 7, y, M_STONE);
    }
    for (x = 3; x <= 7; x++) sim_set_cell(&s, x, 11, M_WOOD);
    for (y = 12; y <= 15; y++) sim_set_cell(&s, 7, y, M_EMPTY);
    CHECK(settle_grid(&s) < 100);
    CHECK(AT(&s, 3, 11) == M_WOOD);             /* still on its pier */
    for (x = 4; x <= 7; x++) CHECK(AT(&s, x, 11) == M_EMPTY);
    CHECK(AT(&s, 3, 12) == M_STONE);
    CHECK(sim_collapse_step(&s) == 0);
}

static void test_fall_damages_what_it_lands_on(void)
{
    Sim s;
    sim_init(&s, 1);
    sim_set_cell(&s, 5, 15, M_STONE);
    sim_set_cell(&s, 5, 10, M_WOOD);
    settle_grid(&s);
    CHECK(AT(&s, 5, 14) == M_WOOD);
    CHECK(HP(&s, 5, 14) == 1);                  /* fell 4: raw 3 -> 3 damage */
    CHECK(HP(&s, 5, 15) == 3);                  /* stone takes half: 1 */
}

static void test_ground_never_moves(void)
{
    Sim s;
    int i, bad = 0;
    sim_init(&s, 1);
    for (i = 0; i < 30; i++) {
        sim_set_cell(&s, (uint8_t)(i % GRID_W), (uint8_t)(i % 12), M_WOOD);
    }
    settle_grid(&s);
    for (i = GROUND_ROW * GRID_W; i < GRID_CELLS; i++)
        if (s.mat[i] != M_GROUND) bad++;
    CHECK(bad == 0);
}

/* ---------- animation hook ---------- */

/* The front end replays sim_collapse_step's move log as sliding sprites, so
 * every entry has to describe a cell that really did move down a row. */
static void test_collapse_emits_moves(void)
{
    static const char *const dangling[] = { "..W.", "" };   /* rows align to the ground */
    static const char *const held[] = { ".WWW.", "WW.WW" };
    static const char *const spread[] = { "W.W.W.W.W.W.W.W.W.W.", "" };
    Sim s;
    uint8_t i;

    /* A lone cell with nothing under it moves, and is logged once. */
    sim_load_ascii(&s, dangling, 2, 1);
    CHECK(sim_collapse_step(&s) == 1);
    CHECK(s.nmoves == 1);
    CHECK(s.moves[0].x == 2 && s.moves[0].y == 14);
    CHECK(s.moves[0].mat == M_WOOD);
    CHECK(AT(&s, 2, 14) == M_EMPTY && AT(&s, 2, 15) == M_WOOD);

    /* A cell wedged between two solid neighbours stays put: no move, no log. */
    sim_load_ascii(&s, held, 2, 1);
    CHECK(sim_collapse_step(&s) == 0);
    CHECK(s.nmoves == 0);

    /* More movers than sprite slots: the log is capped, the rest still move. */
    sim_load_ascii(&s, spread, 2, 1);
    CHECK(sim_collapse_step(&s) > MAX_FALL_MOVES);
    CHECK(s.nmoves == MAX_FALL_MOVES);
    for (i = 0; i < s.nmoves; i++)
        CHECK(s.moves[i].mat != M_EMPTY && s.moves[i].y == 14);

    /* A move that lands on something is a landing, not a move. */
    sim_load_ascii(&s, dangling, 2, 1);
    sim_collapse_step(&s);
    CHECK(sim_collapse_step(&s) > 0);       /* the landing registers */
    CHECK(s.nmoves == 0);                   /* but nothing slid this step */
}

/* ---------- win / lose ---------- */

static const char *const flat_target[] = {
    "..............T....."
};

static void test_win_and_stars(void)
{
    Sim s;
    int a, p, found = 0, fa = 0, fp = 0;
    for (a = 0; a < NUM_ANGLES && !found; a++) {
        for (p = 0; p < NUM_POWERS && !found; p++) {
            sim_load_ascii(&s, flat_target, 1, 2);
            sim_fire(&s, a, p);
            run_idle(&s, 5000);
            if (s.state == S_WON) { found = 1; fa = a; fp = p; }
        }
    }
    CHECK(found);
    sim_load_ascii(&s, flat_target, 1, 2);
    CHECK(sim_stars(&s) == 0);
    CHECK(sim_fire(&s, fa, fp) == 1);
    CHECK(sim_stars(&s) == 0);                  /* not won until settled */
    run_idle(&s, 5000);
    CHECK(s.state == S_WON);
    CHECK(s.targets_left == 0);
    CHECK(s.shots_left == 1);
    CHECK(sim_stars(&s) == 2);
    CHECK(sim_fire(&s, fa, fp) == 0);           /* game over */
}

static void test_lose_when_out_of_shots(void)
{
    static const char *const rows[] = { "..................T." };
    Sim s;
    sim_load_ascii(&s, rows, 1, 2);
    CHECK(sim_fire(&s, 0, 0) == 1);
    CHECK(sim_fire(&s, 0, 0) == 0);             /* can't fire mid-flight */
    run_idle(&s, 5000);
    CHECK(s.state == S_AIM && s.shots_left == 1);
    CHECK(sim_fire(&s, 0, 0) == 1);
    run_idle(&s, 5000);
    CHECK(s.state == S_LOST);
    CHECK(sim_stars(&s) == 0);
    CHECK(sim_fire(&s, 0, 0) == 0);
}

static void test_fire_rejects_bad_input(void)
{
    Sim s;
    sim_load_ascii(&s, flat_target, 1, 1);
    CHECK(sim_fire(&s, NUM_ANGLES, 0) == 0);
    CHECK(sim_fire(&s, 0, NUM_POWERS) == 0);
    CHECK(s.state == S_AIM && s.shots_left == 1);
}

/* ---------- solvability brute force ---------- */

static int solvable_one_shot(const char *const *rows, uint8_t n)
{
    int a, p;
    for (a = 0; a < NUM_ANGLES; a++) {
        for (p = 0; p < NUM_POWERS; p++) {
            Sim s;
            sim_load_ascii(&s, rows, n, 1);
            sim_fire(&s, (uint8_t)a, (uint8_t)p);
            run_idle(&s, 5000);
            if (s.state == S_WON) return 1;
        }
    }
    return 0;
}

static void test_solvability_check(void)
{
    static const char *const boxed[] = {
        ".............SSS....",
        ".............STS....",
    };
    CHECK(solvable_one_shot(flat_target, 1) == 1);
    CHECK(solvable_one_shot(boxed, 2) == 0);
}

/* ---------- levels ---------- */

/* Flight frames at which a shot may spend its ability. Deliberately coarse: a
 * player aims by eye, and a finer grid only makes the search slower. 255 means
 * never trigger, which is the only entry a standard shot has. */
static const uint8_t TRIGS[] = { 255, 0, 4, 8, 12, 16, 20, 26, 32, 38, 44 };

/* A standard shot has no ability to spend, so it only ever tries "never". */
static int trig_count(uint8_t type)
{
    return type == PT_STANDARD ? 1 : (int)(sizeof TRIGS / sizeof TRIGS[0]);
}

/* Fire one shot of the given type and run it to the end. */
static void shoot(Sim *s, uint8_t type, uint8_t a, uint8_t p, uint8_t trig)
{
    int t;
    s->ptype = type;
    sim_fire(s, a, p);
    if (trig != 255) {
        for (t = 0; t < trig && s->state == S_FLIGHT; t++) sim_tick(s);
        sim_trigger(s);
    }
    run_idle(s, 5000);
}

/* How many of the first shots win the level outright. A wide number means a
 * forgiving level; a flat 0 means it needs more than one shot. */
static int one_shot_wins(const char *const *rows, uint8_t h, uint8_t shots, uint8_t type)
{
    int n = 0, ti;
    uint8_t a, p;
    for (a = 0; a < NUM_ANGLES; a++) {
        for (p = 0; p < NUM_POWERS; p++) {
            for (ti = 0; ti < trig_count(type); ti++) {
                Sim s;
                sim_load_ascii(&s, rows, h, shots);
                shoot(&s, type, a, p, TRIGS[ti]);
                if (s.state == S_WON) n++;
            }
        }
    }
    return n;
}

/* Play the level on the level's own shot budget with one projectile type,
 * always taking the first shot that leaves the fewest targets standing
 * (target-count tie-break only: score is deliberately not a criterion, so a
 * scoring-rule change cannot drift what these solvability tests see).
 * Returns the shots used, or -1 if the budget ran out with a target still
 * up. Cheap proxy for "is this level beatable with this type", which is all
 * the difficulty ordering and the type gates need. */
static int greedy_play(const char *const *rows, uint8_t h, uint8_t shots, uint8_t type)
{
    Sim s, best, t;
    int used, ti;
    uint8_t a, p;
    sim_load_ascii(&s, rows, h, shots);
    for (used = 0; used < shots; used++) {
        int found = 0;
        for (a = 0; a < NUM_ANGLES; a++) {
            for (p = 0; p < NUM_POWERS; p++) {
                for (ti = 0; ti < trig_count(type); ti++) {
                    t = s;
                    shoot(&t, type, a, p, TRIGS[ti]);
                    if (!found || t.targets_left < best.targets_left) {
                        best = t;
                        found = 1;
                    }
                }
            }
        }
        s = best;
        if (s.state == S_WON) return used + 1;
    }
    return -1;
}

/* Every level has to be beatable and has to sit still until the first shot:
 * a cell that is not supported would collapse under the player's feet. */
static void test_levels_are_winnable(void)
{
    int i, worst = 0;
    printf("      lvl  rows  shots  type  one-shot wins  greedy shots\n");
    for (i = 0; i < NUM_LEVELS; i++) {
        const char *const *rows = level_rows[i];
        uint8_t h = level_height[i], shots = level_shots[i], need = level_needs[i];
        int width, used, type, best = -1;
        Sim s;
        char label[8];

        snprintf(label, sizeof label, "%d", i + 1);
        sim_load_ascii(&s, rows, h, shots);
        CHECK(s.targets_left > 0);
        CHECK(h > 0 && h <= GRID_H - 2);
        CHECK(need < PT_COUNT);
        CHECK(sim_collapse_step(&s) == 0);      /* stable before the first shot */

        /* A level is beatable if any projectile type beats it. */
        for (type = 0; type < PT_COUNT; type++) {
            used = greedy_play(rows, h, shots, (uint8_t)type);
            if (used > 0 && (best < 0 || used < best)) best = used;
        }
        width = one_shot_wins(rows, h, shots, need);
        printf("      %-3s  %4d  %5d  %4c  %14d  %12d\n", label, h, shots,
               'A' + need, width, best);
        CHECK(best > 0);
        CHECK(best <= shots);
        if (best > worst) worst = best;
    }
    printf("      hardest level needs %d of its budget\n", worst);
}

/* Levels 11 onwards declare a projectile type they are built around. The
 * declaration is checked here, by search, so a level cannot claim a gate it
 * does not have: the standard shot must fail and the declared type must win. */
static void test_late_levels_need_a_type(void)
{
    int i;
    printf("      lvl  needs  targets  shots  standard one-shot wins\n");
    for (i = 0; i < NUM_LEVELS; i++) {
        const char *const *rows = level_rows[i];
        uint8_t h = level_height[i], shots = level_shots[i], need = level_needs[i];
        Sim s;
        char label[8];

        if (need == PT_STANDARD) continue;
        snprintf(label, sizeof label, "%d", i + 1);
        sim_load_ascii(&s, rows, h, shots);
        printf("      %-3s  %5c  %7d  %5d  %23d\n", label, 'A' + need,
               s.targets_left, shots, one_shot_wins(rows, h, shots, PT_STANDARD));
        /* Exhaustive over every standard shot, then greedy over the budget. */
        CHECK(one_shot_wins(rows, h, shots, PT_STANDARD) == 0);
        CHECK(greedy_play(rows, h, shots, PT_STANDARD) < 0);
        CHECK(greedy_play(rows, h, shots, need) > 0);
    }
}

/* ---------- golden replay ---------- */

/* Which level the recorded game is played on. tests/emu_replay.py has to
 * select the same one in the ROM, so keep the two in step. */
#define GOLDEN_LEVEL 2

/* Play a shot to the end; returns how many settle ticks changed the grid. */
static int play_shot(Sim *s, uint8_t a, uint8_t p)
{
    int changes = 0;
    if (!sim_fire(s, a, p)) return -1;
    while (s->state == S_FLIGHT || s->state == S_SETTLE) {
        uint32_t before = hash_grid(s);
        int was_settling = s->state == S_SETTLE;
        sim_tick(s);
        if (was_settling && hash_grid(s) != before) changes++;
    }
    return changes;
}

/* A recorded three-shot game that includes collapses. If a rule change
 * alters the outcome this fails; re-record with UPDATE_GOLDEN=1 make test
 * once the new behaviour is deliberate. */
static void test_golden_replay(void)
{
    static const uint8_t shots[][2] = { { 0, 7 }, { 0, 5 }, { 0, 5 } };
    static const uint32_t GOLDEN = 0xC6967041u;
    const char *const *rows = level_rows[GOLDEN_LEVEL];
    uint8_t n = level_height[GOLDEN_LEVEL];
    uint32_t h1 = 0, h2 = 0;
    int pass, i, changes = 0;
    for (i = 0; i < n; i++) CHECK(strlen(rows[i]) == GRID_W);
    CHECK(n == 6);                             /* the recorded game assumes this */
    for (pass = 0; pass < 2; pass++) {
        Sim s;
        sim_load_ascii(&s, rows, n, level_shots[GOLDEN_LEVEL]);
        CHECK(sim_collapse_step(&s) == 0);      /* starts stable */
        changes = 0;
        for (i = 0; i < 3; i++) changes += play_shot(&s, shots[i][0], shots[i][1]);
        if (pass == 0) h1 = hash_grid(&s); else h2 = hash_grid(&s);
        CHECK(s.state == S_WON);
        CHECK(changes > 0);                     /* collapse really ran */
        CHECK(s.targets_left == 0);
        if (pass == 0 && getenv("UPDATE_GOLDEN"))
            printf("      golden hash: 0x%08Xu  score=%u stars=%d collapse-ticks=%d\n",
                   h1, s.score, sim_stars(&s), changes);
    }
    CHECK(h1 == h2);                            /* deterministic */
    CHECK(h1 == GOLDEN);                        /* unchanged since recorded */
}

int main(void)
{
    RUN(test_tables);
    RUN(test_fixed_point_matches_accumulator);
    RUN(test_range_matches_physics);
    RUN(test_damage_materials);
    RUN(test_damage_reports_material);
    RUN(test_speed_bucket);
    RUN(test_shot_stops_at_stone);
    RUN(test_shot_breaks_through_ice);
    RUN(test_shot_through_glass_still_kills);
    RUN(test_trigger_rules);
    RUN(test_boost_hits_harder);
    RUN(test_split_makes_three);
    RUN(test_pierce_trigger_rules);
    RUN(test_pierce_untriggered_stops);
    RUN(test_pierce_drills_through);
    RUN(test_pierce_ground_holds);
    RUN(test_empty_level_is_stable);
    RUN(test_stack_falls_together);
    RUN(test_long_fall_destroys_cells);
    RUN(test_chain_bonus_event);
    RUN(test_chain_points_where);
    RUN(test_chain_points_when_below_dies);
    RUN(test_bridge_holds);
    RUN(test_bridge_collapses_when_pier_removed);
    RUN(test_fall_damages_what_it_lands_on);
    RUN(test_ground_never_moves);
    RUN(test_collapse_emits_moves);
    RUN(test_win_and_stars);
    RUN(test_lose_when_out_of_shots);
    RUN(test_fire_rejects_bad_input);
    RUN(test_solvability_check);
    RUN(test_levels_are_winnable);
    RUN(test_late_levels_need_a_type);
    RUN(test_golden_replay);
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
