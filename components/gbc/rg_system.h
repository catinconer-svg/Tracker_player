#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ============================================================================
//  IRAM
// ============================================================================

#ifndef IRAM_ATTR
#define IRAM_ATTR __attribute__((section(".iram1.text")))
#endif

#ifndef EXT_RAM_ATTR
#define EXT_RAM_ATTR __attribute__((section(".ext_ram.text")))
#endif

// ============================================================================
//  ЛОГИРОВАНИЕ
// ============================================================================

#define RG_LOG_PRINT 0
#define RG_LOG_ERROR 1
#define RG_LOG_WARN  2
#define RG_LOG_INFO  3
#define RG_LOG_DEBUG 4
#define RG_LOG_USER  5

#define RG_LOGE(x, ...) printf("!! " x "\n", ## __VA_ARGS__)
#define RG_LOGW(x, ...) printf("** " x "\n", ## __VA_ARGS__)
#define RG_LOGI(x, ...) printf(" * " x "\n", ## __VA_ARGS__)
#define RG_LOGD(x, ...) // printf(">> " x "\n", ## __VA_ARGS__)

static inline void rg_system_log(int level, const char *context, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    if (level == RG_LOG_ERROR || level == 0) {
        printf("!! ");
    } else if (level == RG_LOG_WARN) {
        printf("** ");
    } else if (level == RG_LOG_INFO || level == RG_LOG_USER) {
        printf(" * ");
    }
    if (context) {
        printf("%s: ", context);
    }
    vprintf(format, va);
    va_end(va);
}

#define RG_PANIC(x) do { printf("PANIC: %s\n", x); while(1); } while(0)
#define RG_ASSERT(cond, msg) while (!(cond)) { RG_PANIC(msg); }

// ============================================================================
//  ПАМЯТЬ
// ============================================================================

static inline void *rg_alloc(size_t size, uint32_t caps)
{
    void *ptr = calloc(1, size);
    if (!ptr) {
        RG_LOGE("rg_alloc failed: size=%d", (int)size);
        return NULL;
    }
    return ptr;
}

static inline void *rg_calloc(size_t nmemb, size_t size, uint32_t caps)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr) {
        RG_LOGE("rg_calloc failed: nmemb=%d, size=%d", (int)nmemb, (int)size);
        return NULL;
    }
    return ptr;
}

#define rg_free(ptr) free(ptr)

// ============================================================================
//  УТИЛИТЫ
// ============================================================================

static inline const char *rg_basename(const char *path)
{
    if (!path) return ".";
    const char *name = strrchr(path, '/');
    return name ? name + 1 : path;
}

static inline const char *rg_extension(const char *path)
{
    if (!path) return NULL;
    const char *name = rg_basename(path);
    const char *ext = strrchr(name, '.');
    return ext ? ext + 1 : NULL;
}

// ============================================================================
//  СТРУКТУРЫ
// ============================================================================

typedef struct {
    float speed;          // ← СНАЧАЛА speed
    int saveSlot;         // ← ПОТОМ saveSlot
    uint32_t bootFlags;   // ← ПОТОМ bootFlags
    const char *romPath;  // ← ПОТОМ romPath
} rg_app_t;

static inline rg_app_t *rg_system_get_app(void)
{
    static rg_app_t app = {
        .speed = 1.0f,        // ← СООТВЕТСТВУЕТ ПОРЯДКУ
        .saveSlot = 0,
        .bootFlags = 0,
        .romPath = NULL,
    };
    return &app;
}

// ============================================================================
//  ПУСТЫШКИ
// ============================================================================

#define RG_STORAGE_ROOT "/sdcard"
#define RG_BASE_PATH_SAVES RG_STORAGE_ROOT "/retro-go/saves"
#define RG_BASE_PATH_CACHE RG_STORAGE_ROOT "/retro-go/cache"

typedef struct {
    int totalFrames;
    int fullFrames;
    int busyPercent;
    float totalFPS;
    float fullFPS;
    float skippedFPS;
} rg_stats_t;

static inline rg_stats_t rg_system_get_counters(void)
{
    rg_stats_t stats = {0};
    return stats;
}

static inline int64_t rg_system_timer(void)
{
    return 0;
}

static inline void rg_system_tick(int busyTime) {}

static inline void rg_system_set_led(int value) {}
static inline int rg_system_get_led(void) { return 0; }

#define RG_TARGET_NAME "ESP32-S3"

// ★ НЕ ОПРЕДЕЛЯЙ GB_PIXEL_* и GB_PALETTE_* ЗДЕСЬ! ★
// Они уже определены в gnuboy.h