/* sim stub for freertos/task.h. No real tasking — async ops run inline. */
#pragma once
#include "FreeRTOS.h"
#include <SDL2/SDL.h>
typedef void (*TaskFunction_t)(void *);
static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *n, uint32_t s, void *arg,
                                      int prio, TaskHandle_t *h)
{ (void)n; (void)s; (void)prio; (void)h; if (fn) fn(arg); return pdPASS; }
static inline void vTaskDelay(TickType_t ms) { SDL_Delay(ms); }
static inline void vTaskDelete(void *h) { (void)h; }
