/*
 * ui_shell — Aurora's UI chrome.
 *
 *   ┌── top arc ──┐
 *   │  I. Particles │      ← scene label, ~70% opacity
 *   │               62fps│ ← tiny fps, ~30% opacity, top-right
 *   │      scene canvas    │
 *   │     · · ●            │ ← indicator dots, bottom-centre
 *   └─────────────────────┘
 *
 * All shell elements sit on `lv_layer_top()` so they survive scene fades.
 * The scene framework calls ui_shell_set_active() whenever the current
 * scene changes, and ui_shell_tick() once per second updates the FPS read.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_shell_init(int scene_count);
void ui_shell_set_active(int idx, const char *display_name);
void ui_shell_set_fps(float fps);
void ui_shell_toggle_chrome(void);  /* hide everything for "focus mode" */

#ifdef __cplusplus
}
#endif
