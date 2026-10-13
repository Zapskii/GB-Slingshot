/* tools/check_levels.c - the level solvability gate from the build plan.
 *
 * Brute-forces the angle/power table per level via breadth-first search and
 * reports how the level can be won: a shot sequence within the level's
 * level_shots budget, or no win at all reachable, or budget exhausted. This
 * runs the real sim (src/sim.h), so a physics or rule tweak that makes a
 * level unwinnable fails here and not on a shipped ROM.
 *
 * The search expands every projectile type (the player cycles all three with
 * B) and every angle/power pair, and includes the type's mid-flight ability
 * (the second A press), which the special levels are built around. Trigger
 * timing is sampled -- every frame up to 12, then at intervals to frame 110
 * -- so an UNWINNABLE verdict means "no win within this action set", not a
 * proof over every possible trigger frame; a WINNABLE verdict, by contrast,
 * is always backed by a concrete playable shot list.
 *
 * Dedup keeps the breadth small: two different shot orders that land on the
 * same grid (with the same shots left) are the same state. Fingerprints are
 * 128-bit (two independent FNV passes), so collapsing two states to one
 * would need both hashes to collide - not a practical risk.
 *
 * Budgets (states per layer, sims per level) bound runtime; a level whose
 * search hits a budget reports UNKNOWN rather than a guess, and the tool
 * exits nonzero on UNKNOWN as well as on UNWINNABLE. Raise a budget with
 *
 *   ./build/check_levels --states N --sims N [level ...]   # default: all levels
 *
 * Verdicts per level: WINNABLE (followed by one found shot list),
 * UNWINNABLE, UNKNOWN.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "sim.h"
#include "levels.h"

#define STATES_BUDGET 48000     /* live states per layer, ~64 MB of Sims */
#define SIMS_BUDGET   8000000   /* shots simulated per level */
#define TICKS_CAP     20000     /* per-shot safety cap, mirrors tests */

static const char *verdict_name(int r)
{
    return r == 1 ? "WINNABLE  " : r == 0 ? "UNWINNABLE" : "UNKNOWN  ";
}

static const char *verdict_text(int r)
{
    return r == 1 ? "winnable" : r == 0 ? "unwinnable within its shot budget" :
           "search budget exceeded (rerun with --states / --sims)";
}

/* Fingerprint of one state: everything that decides its future. The shots
 * left must be in the key - the same grid is a different game with fewer
 * shots to spend. */
static void fingerprint(const Sim *s, uint64_t *h1, uint64_t *h2)
{
    uint64_t a = 0xCBF29CE484222325ull, b = 0x27D4EB2F165667C5ull;
    unsigned i;
    for (i = 0; i < GRID_CELLS; i++) {
        a = (a ^ s->mat[i]) * 0x100000001B3ull;
        a = (a ^ s->hp[i]) * 0x100000001B3ull;
        a = (a ^ s->fall[i]) * 0x100000001B3ull;
        b = (b ^ s->mat[i]) * 0x9E3779B97F4A7C15ull;
        b = (b ^ s->hp[i]) * 0x9E3779B97F4A7C15ull;
        b = (b ^ s->fall[i]) * 0x9E3779B97F4A7C15ull;
    }
    a ^= (uint64_t)s->shots_left * 0x27220A95ull; a *= 0x100000001B3ull;
    a ^= (uint64_t)s->pused * 0x165667B1ull;      a *= 0x100000001B3ull;
    b ^= (uint64_t)s->shots_left * 0x9E3779B9ull; b *= 0x9E3779B97F4A7C15ull;
    b ^= (uint64_t)s->pused * 0x85EBCA77ull;      b *= 0x9E3779B97F4A7C15ull;
    *h1 = a; *h2 = b;
}

/* Open-addressed set of (h1, h2) pairs; 0/0 is the empty marker, guarded by
 * nudging a matching fingerprint to 0/1 before insertion. */
typedef struct { uint64_t *h; size_t cap, used; } SeenSet;

static void seen_init(SeenSet *set, size_t expect)
{
    size_t c = 64;
    while (c < expect * 2) c *= 2;
    set->h = calloc(c, 2 * sizeof(uint64_t));
    if (!set->h) { puts("check_levels: out of memory"); exit(1); }
    set->cap = c;
    set->used = 0;
}

/* Returns 1 if newly added, 0 if already present. */
static int seen_add(SeenSet *set, uint64_t h1, uint64_t h2)
{
    size_t mask = set->cap - 1, i = (size_t)h1 & mask;
    if (h1 == 0 && h2 == 0) h2 = 1;
    while (set->h[2 * i] | set->h[2 * i + 1]) {
        if (set->h[2 * i] == h1 && set->h[2 * i + 1] == h2) return 0;
        i = (i + 1) & mask;
    }
    set->h[2 * i] = h1;
    set->h[2 * i + 1] = h2;
    if (++set->used > set->cap / 2) {
        SeenSet old = *set;
        seen_init(set, old.used * 4);
        for (size_t j = 0; j < old.cap; j++)
            if (old.h[2 * j] | old.h[2 * j + 1])
                seen_add(set, old.h[2 * j], old.h[2 * j + 1]);
        free(old.h);
    }
    return 1;
}

/* The BFS bookkeeping. Layer k holds every distinct state reachable after k
 * shots; why[k][j] is the shot that produced state j, parent[k][j] its index
 * in layer k-1. Sims are kept only for the layer being expanded; the shot
 * records stay alive for the whole level so a win can be traced back. */
typedef struct {
    Sim *s;                 /* only the layer currently being expanded */
    uint32_t *parent;
    uint8_t (*why)[4];      /* the shot: ptype, angle, power, trigger tick (255 = none) */
    size_t n, cap_s, cap_i;
} Layer;

static void layer_push(Layer *l, const Sim *s, uint32_t parent, const uint8_t why[4])
{
    if (l->cap_s == 0) {
        l->s = malloc(64 * sizeof(Sim));
        if (!l->s) goto oom;
        l->cap_s = 64;
    }
    if (l->n == l->cap_s) {
        Sim *sp = realloc(l->s, (l->cap_s * 2) * sizeof(Sim));
        if (!sp) goto oom;
        l->s = sp;
        l->cap_s *= 2;
    }
    if (l->cap_i == 0) {
        l->parent = malloc(64 * sizeof(uint32_t));
        l->why = malloc(64 * 4);
        if (!l->parent || !l->why) goto oom;
        l->cap_i = 64;
    }
    if (l->n == l->cap_i) {
        uint32_t *pp = realloc(l->parent, (l->cap_i * 2) * sizeof(uint32_t));
        uint8_t (*wp)[4] = realloc(l->why, (l->cap_i * 2) * 4);
        if (!pp || !wp) goto oom;
        l->parent = pp;
        l->why = wp;
        l->cap_i *= 2;
    }
    l->s[l->n] = *s;
    l->parent[l->n] = parent;
    memcpy(l->why[l->n], why, 4);
    l->n++;
    return;
oom:
    puts("check_levels: out of memory");
    exit(1);
}

static void layer_reset(Layer *l)
{
    free(l->s);
    l->s = NULL;
    l->cap_s = l->n = 0;
}

#define MAX_LAYERS 4            /* one more than the largest shot budget */

static long total_sims;

/* Sampled trigger timings: frame 0 first - the stone-tower levels only fall
 * to a boost fired on the very first flight frame, so a set that starts at 1
 * misreports them unwinnable (that gap cost a false UNWINNABLE, found by
 * --dense) - then dense early (that is when the golden replay triggers its
 * split), coarse later. `--dense` replaces the set with every frame up to
 * TICKS_CAP - 1 (a trigger later than that cannot happen mid-flight),
 * trading runtime for exhaustiveness on one hard level. Timing -1 never
 * triggers, and NUM_TRIGGER_TS is a variable the timing loop reads, so
 * --dense can swap the set out. */
static int TRIGGER_TS[TICKS_CAP];
static int NUM_TRIGGER_TS;

static void trigger_ts_init(int dense)
{
    NUM_TRIGGER_TS = 0;
    if (dense) {
        int t;
        for (t = 0; t < TICKS_CAP; t++) TRIGGER_TS[NUM_TRIGGER_TS++] = t;
    } else {
        static const int ts[] = { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
                                  11, 12, 16, 24, 32, 44, 60, 80, 110 };
        memcpy(TRIGGER_TS, ts, sizeof(ts));
        NUM_TRIGGER_TS = (int)(sizeof(ts) / sizeof(ts[0]));
    }
}

static void play_to_end(Sim *s, int trigger_t)
{
    int t = 0;
    while ((s->state == S_FLIGHT || s->state == S_SETTLE) && t < TICKS_CAP) {
        if (t == trigger_t && s->state == S_FLIGHT) sim_trigger(s);
        sim_tick(s);
        t++;
    }
}

static void print_shots(const Layer layers[], unsigned d, size_t win_idx,
                        const uint8_t win_shot[4])
{
    /* layers[k].why[j] is shot k+1; walk the parents back and forward again
     * so the list reads in play order instead of reverse. */
    uint8_t shots[MAX_LAYERS][4];
    size_t j = win_idx;
    int k;
    memcpy(shots[d - 1], win_shot, 4);
    for (k = (int)d - 1; k >= 1; k--) {
        memcpy(shots[k - 1], layers[k].why[j], 4);
        if (k >= 2) j = layers[k].parent[j];
    }
    printf("        win in %u shots:", d);
    for (k = 0; k < (int)d; k++) {
        printf(" %s a%u p%u",
               shots[k][0] == PT_STANDARD ? "" : shots[k][0] == PT_BOOST ? "B" : "S",
               shots[k][1], shots[k][2]);
        if (shots[k][3] != 255)
            printf(" (ability at frame %u)", shots[k][3]);
    }
    printf("\n");
}

/* Search one level. Returns 1 winnable, 0 unwinnable, -1 out of budget. */
static int solve_level(unsigned level, long states_budget, long sims_budget, int verbose)
{
    const unsigned shots = level_shots[level];
    Sim init = { 0 };
    Layer layers[MAX_LAYERS] = { 0 };
    SeenSet seen;
    unsigned d;
    int verdict = 0;
    const uint8_t start_why[4] = { 0, 0, 0, 255 };

    sim_load_ascii(&init, level_rows[level], level_height[level], shots);
    seen_init(&seen, states_budget);
    {   uint64_t h1, h2;
        fingerprint(&init, &h1, &h2);
        seen_add(&seen, h1, h2);
    }
    layer_push(&layers[0], &init, UINT32_MAX, start_why);

    for (d = 1; d <= shots && !verdict; d++) {
        Layer *from = &layers[d - 1], *to = &layers[d];
        size_t i;
        to->n = 0;
        for (i = 0; i < from->n && !verdict; i++) {
            unsigned pt, a, p, ti, n_t;
            for (pt = 0; pt < PT_COUNT && !verdict; pt++)
            for (a = 0; a < NUM_ANGLES && !verdict; a++)
            for (p = 0; p < NUM_POWERS && !verdict; p++)
            for (ti = 0, n_t = (pt == PT_STANDARD ? 1u : NUM_TRIGGER_TS + 1);
                 ti < n_t && !verdict; ti++) {
                int trigger_t;
                Sim s2;
                uint8_t shot[4];
                uint64_t h1, h2;
                if (pt == PT_STANDARD || ti == 0) trigger_t = -1;
                else trigger_t = TRIGGER_TS[ti - 1];
                s2 = from->s[i];
                shot[0] = (uint8_t)pt;
                shot[1] = (uint8_t)a;
                shot[2] = (uint8_t)p;
                shot[3] = trigger_t >= 0 ? (uint8_t)trigger_t : 255;
                s2.ptype = (uint8_t)pt;
                total_sims++;
                if (total_sims > sims_budget) { verdict = -1; break; }
                if (!sim_fire(&s2, (uint8_t)a, (uint8_t)p)) continue;
                play_to_end(&s2, trigger_t);
                if (s2.state == S_WON) {
                    print_shots(layers, d, i, shot);
                    verdict = 1;
                    break;
                }
                if (s2.state != S_AIM) continue;         /* ran out of shots */
                if (to->n >= (size_t)states_budget) { verdict = -1; break; }
                fingerprint(&s2, &h1, &h2);
                if (!seen_add(&seen, h1, h2)) continue;  /* state already known */
                if (d > 1) layer_push(to, &s2, (uint32_t)i, shot);
                else layer_push(to, &s2, UINT32_MAX, shot);
            }
        }
        layer_reset(from);
        if (!verdict && to->n == 0) verdict = 0;      /* frontier ran dry */
        if (verbose) printf("        depth %u: %zu states\n", d, to->n);
        if (verdict) break;
    }

    for (d = 0; d < MAX_LAYERS; d++) {
        layer_reset(&layers[d]);
        free(layers[d].parent);
        free(layers[d].why);
    }
    free(seen.h);
    return verdict;
}

int main(int argc, char **argv)
{
    long states_budget = STATES_BUDGET, sims_budget = SIMS_BUDGET;
    unsigned level, lo = 0, hi = NUM_LEVELS - 1;
    int verbose = 0, bad = 0, dense = 0;
    unsigned i;
    clock_t start = clock();

    for (i = 1; i < (unsigned)argc; i++) {
        if (!strcmp(argv[i], "--states")) {
            if (++i >= (unsigned)argc) { puts("usage: --states N"); return 1; }
            states_budget = atol(argv[i]);
        } else if (!strcmp(argv[i], "--sims")) {
            if (++i >= (unsigned)argc) { puts("usage: --sims N"); return 1; }
            sims_budget = atol(argv[i]);
        } else if (!strcmp(argv[i], "--dense")) {
            dense = 1;
        } else if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else {
            lo = hi = (unsigned)atoi(argv[i]);
            if (lo >= NUM_LEVELS) { printf("no level %u (max %u)\n", lo, hi); return 1; }
        }
    }
    trigger_ts_init(dense);

    for (level = lo; level <= hi; level++) {
        int r = solve_level(level, states_budget, sims_budget, verbose);
        printf("%s level %u: %s\n", verdict_name(r), level + 1, verdict_text(r));
        if (r != 1) bad = 1;
    }
    printf("        checked %ld shot sims against levels %u-%u in %.1fs\n",
           total_sims, lo + 1, hi + 1,
           (double)(clock() - start) / CLOCKS_PER_SEC);
    return bad;
}