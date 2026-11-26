/* The stock firmware's hardware init as register writes (src/fw_init.c, made by tools/mkinit.py). */
#pragma once
#include "types.h"
/* rmw 0: write `set`. rmw 1: the firmware read the register and changed bits: reg = (reg & ~clr) | set. */
typedef struct { u32 addr, set, clr; u8 bits, rmw; } fw_write;
extern const fw_write fw_board[], fw_board_41[], fw_display[];
extern const int fw_board_n, fw_board_41_n, fw_display_n;
void fw_replay(const fw_write *w, int n);
