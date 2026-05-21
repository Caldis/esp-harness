/* sim stub for freertos/FreeRTOS.h. */
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef uint32_t BaseType_t;
typedef void *   TaskHandle_t;
typedef void *   SemaphoreHandle_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)
