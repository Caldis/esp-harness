/*
 * Application-level console commands for the AI harness.
 *
 * These plug into console_protocol. Call harness_commands_register() after
 * console_protocol_init() and after the display + scene framework are up.
 *
 * Commands:
 *   ?stat                 — JSON: fps, heap, uptime, scene index/name
 *   ?dump [w=N]           — screenshot: 128x128 RGB565 base64 (default)
 *                          if w is given, that size is used
 *   tap                   — center tap (back-compat with image-viewer)
 *   tap X Y               — tap at pixel (X,Y) on the screen
 *   swipe X1 Y1 X2 Y2 [DUR_MS]  — drag from (X1,Y1) to (X2,Y2)
 *   scene next            — switch to next scene
 *   scene prev            — switch to previous scene
 *   scene N               — switch to scene by index
 *   scene name            — get current scene name (also returns index)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void harness_commands_register(void);

#ifdef __cplusplus
}
#endif
