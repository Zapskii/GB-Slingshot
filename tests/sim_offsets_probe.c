/* tests/sim_offsets_probe.c - print the byte layout of Sim as JSON.
 *
 * tests/emu_replay.py reads game state straight out of the SDCC-built map at
 * fixed field offsets. Those offsets used to be duplicate constants, so any
 * struct edit could silently desync them (src/sim.h carried a warning asking
 * new fields to go last, purely to keep the copies aligned). make emu-test
 * now regenerates build/sim_offsets.json from the real header before every
 * run, which makes this hand-maintained copy - and the put-new-fields-at-
 * the-end rule - obsolete. */
#include <stddef.h>
#include <stdio.h>
#include "sim.h"

int main(void)
{
    printf("{\n");
    printf("  \"cells\": %u,\n", (unsigned)GRID_CELLS);
    printf("  \"mat\": %u,\n", (unsigned)offsetof(Sim, mat));
    printf("  \"hp\": %u,\n", (unsigned)offsetof(Sim, hp));
    printf("  \"state\": %u,\n", (unsigned)offsetof(Sim, state));
    printf("  \"shots_left\": %u,\n", (unsigned)offsetof(Sim, shots_left));
    printf("  \"targets_left\": %u,\n", (unsigned)offsetof(Sim, targets_left));
    printf("  \"score\": %u,\n", (unsigned)offsetof(Sim, score));
    printf("  \"s_aim\": %u,\n", (unsigned)S_AIM);
    printf("  \"s_flight\": %u,\n", (unsigned)S_FLIGHT);
    printf("  \"s_settle\": %u,\n", (unsigned)S_SETTLE);
    printf("  \"s_won\": %u,\n", (unsigned)S_WON);
    printf("  \"s_lost\": %u\n", (unsigned)S_LOST);
    printf("}\n");
    return 0;
}