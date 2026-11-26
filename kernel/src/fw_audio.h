/* The stock firmware's audio bring-up (src/fw_audio.c, made by tools/mkaudio.py). */
#pragma once
#include "types.h"
/* kind 0: codec write: register a, then v value bytes b[]. kind 1: audio block register a = v. */
typedef struct { u8 kind; u32 a, v; u8 b[8]; } fw_audio_op;
extern const fw_audio_op fw_audio[];
extern const int fw_audio_n;
