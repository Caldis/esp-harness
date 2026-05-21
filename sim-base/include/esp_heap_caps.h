/* sim stub for esp_heap_caps — host malloc/free, ignore caps. */
#pragma once
#include <stdlib.h>
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_DEFAULT  0
#define MALLOC_CAP_DMA      0
#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_32BIT    0
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t s, int caps) { (void)caps; return calloc(n, s); }
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 1024 * 1024; }
static inline size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return 256 * 1024; }
static inline size_t heap_caps_get_minimum_free_size(int caps) { (void)caps; return 512 * 1024; }
