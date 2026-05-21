/*
 * scenes.h — extern declarations for all available scenes.
 *
 * Each scene_*.c file defines a single `scene_t` in file scope; this
 * header makes them visible to aurora_main.c so it can register them
 * in the desired display order.
 */

#pragma once

#include "harness/scene_framework.h"

#ifdef __cplusplus
extern "C" {
#endif

extern scene_t scene_halo;
extern scene_t scene_grid;
extern scene_t scene_bloom;
extern scene_t scene_tilt;
extern scene_t scene_pulse;
extern scene_t scene_echo;
extern scene_t scene_vault;
extern scene_t scene_whisper;
extern scene_t scene_spectrum;
extern scene_t scene_cell;
extern scene_t scene_keys;
extern scene_t scene_listen;
extern scene_t scene_tone;
extern scene_t scene_system;
extern scene_t scene_glow;
extern scene_t scene_spin;
extern scene_t scene_survey;
extern scene_t scene_sniff;
extern scene_t scene_notify;
extern scene_t scene_track;

#ifdef __cplusplus
}
#endif
