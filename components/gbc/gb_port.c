#include "gb_port.h"
#include "gnuboy.h"
#include "rg_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stddef.h>

static const char *TAG = "GB_PORT";

// Глобальный хост
gb_host_port_t gb_host = {
    .video = {
        .enabled = true,
        .format = GB_PIXEL_565_LE,
        .colorize = GB_PALETTE_CGB,
        .buffer16 = NULL,
        .buffer8 = NULL,
        .blit_func = NULL,
    },
    .audio = {
        .enabled = true,
        .stereo = true,
        .samplerate = 32768,
        .buffer = NULL,
        .pos = 0,
        .len = 0,
    },
    .pad = 0,
};

bool gb_sram_dirty = false;

// Внешние функции
extern void lvgl_port_render_gb_direct_rgb(uint16_t *rgb_buffer);
extern void gb_audio_submit(int16_t *buffer, size_t samples);

// ============================================================================
//  ИНИЦИАЛИЗАЦИЯ
// ============================================================================

int gb_port_init(int samplerate, bool stereo, int pixformat, void *blit_func)
{
    ESP_LOGI(TAG, "Initializing GB port...");

    gb_host.video.enabled = true;
    gb_host.video.format = pixformat;
    gb_host.video.colorize = GB_PALETTE_CGB;
    gb_host.video.blit_func = (void (*)(void))blit_func;

    // ★ ВЫДЕЛЯЕМ БУФЕР ДЛЯ Gnuboy ★
    gb_host.video.buffer16 = (uint16_t *)heap_caps_calloc(
        160 * 144, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!gb_host.video.buffer16) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer");
        return -1;
    }
    ESP_LOGI(TAG, "RGB buffer allocated at %p", gb_host.video.buffer16);

    // ★ СВЯЗЫВАЕМ С host (глобальный для Gnuboy) ★
    extern gb_host_t host;
    host.video.buffer16 = gb_host.video.buffer16;
    host.video.enabled = true;
    host.video.format = pixformat;
    host.video.colorize = GB_PALETTE_CGB;
    host.video.blit_func = gb_host.video.blit_func;

    // ★ АУДИО ★
    gb_host.audio.enabled = true;
    gb_host.audio.stereo = stereo;
    gb_host.audio.samplerate = samplerate;
    gb_host.audio.len = samplerate / 8;
    gb_host.audio.buffer = (int16_t *)heap_caps_calloc(
        gb_host.audio.len, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!gb_host.audio.buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return -1;
    }
    gb_host.audio.pos = 0;
    ESP_LOGI(TAG, "Audio buffer allocated: %d samples", (int)gb_host.audio.len);

    host.audio.buffer = gb_host.audio.buffer;
    host.audio.len = gb_host.audio.len;
    host.audio.samplerate = samplerate;
    host.audio.stereo = stereo;
    host.audio.enabled = true;

    ESP_LOGI(TAG, "GB port initialized");
    return 0;
}

// ============================================================================
//  ПОЛУЧЕНИЕ БУФЕРОВ
// ============================================================================

void *gb_port_get_framebuffer(void)
{
    return gb_host.video.buffer16;
}

uint16_t *gb_port_get_palette(void)
{
    return gb_host.video.palette;
}

// ============================================================================
//  АУДИО
// ============================================================================

void gb_port_submit_audio(int16_t *buffer, size_t samples)
{
    if (!buffer || samples == 0 || !gb_host.audio.enabled) {
        return;
    }
    gb_audio_submit(buffer, samples);
}

// ============================================================================
//  УПРАВЛЕНИЕ
// ============================================================================

int gb_port_get_pad(void)
{
    return gb_host.pad;
}

void gb_port_set_pad(int pad)
{
    gb_host.pad = pad & 0xFF;
}

// ============================================================================
//  SRAM
// ============================================================================

bool gb_port_is_sram_dirty(void)
{
    return gb_sram_dirty;
}

int gb_port_load_sram(const char *path)
{
    ESP_LOGI(TAG, "Loading SRAM from: %s", path);
    return 0;
}

int gb_port_save_sram(const char *path, bool quick)
{
    ESP_LOGI(TAG, "Saving SRAM to: %s (quick=%d)", path, quick);
    return 0;
}

// ============================================================================
//  СОСТОЯНИЯ
// ============================================================================

int gb_port_load_state(const char *path)
{
    ESP_LOGI(TAG, "Loading state: %s", path);
    return 0;
}

int gb_port_save_state(const char *path)
{
    ESP_LOGI(TAG, "Saving state: %s", path);
    return 0;
}