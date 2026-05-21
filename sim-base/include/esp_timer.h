/* sim stub for esp_timer — backed by SDL_GetTicks (ms) * 1000 for µs. */
#pragma once
#include <stdint.h>
#include <SDL2/SDL.h>
static inline int64_t esp_timer_get_time(void) {
    return (int64_t)SDL_GetTicks() * 1000;
}
