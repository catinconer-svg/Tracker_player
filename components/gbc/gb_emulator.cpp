#include "gb_emulator.hpp"
#include "gb_display_adapter.h"
#include "gb_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>

extern "C" {
#include "gnuboy.h"
#include "hw.h"
#include "cpu.h"
#include "sound.h"
#include "lcd.h"
}

extern gb_hw_t GB;
#define hw GB

static const char *TAG = "GB_EMULATOR";

static bool emulator_running = false;
static bool frame_ready = false;
static SemaphoreHandle_t frame_semaphore = NULL;
static TaskHandle_t emulator_task_handle = NULL;

static uint16_t *output_buffer = NULL;

static void gb_blit_callback(void)
{
    if (!emulator_running) return;

    // ★ Gnuboy уже нарисовал в host.video.buffer16 ★
    // Просто копируем в output_buffer для отображения
    extern gb_host_t host;
    if (host.video.buffer16) {
        memcpy(output_buffer, host.video.buffer16, 160 * 144 * sizeof(uint16_t));
        frame_ready = true;
        if (frame_semaphore) {
            xSemaphoreGive(frame_semaphore);
        }
    }
}

static void gb_emulator_task(void *pvParameters)
{
    const char *rom_path = (const char *)pvParameters;

    ESP_LOGI(TAG, "Emulator task started for: %s", rom_path);

    if (gb_port_init(32768, true, GB_PIXEL_565_LE, (void*)gb_blit_callback) != 0) {
        ESP_LOGE(TAG, "Failed to initialize GB port");
        emulator_running = false;
        vTaskDelete(NULL);
        return;
    }

    // ★ ВЫДЕЛЯЕМ ВЫХОДНОЙ БУФЕР ★
    output_buffer = (uint16_t *)heap_caps_calloc(
        160 * 144, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!output_buffer) {
        ESP_LOGE(TAG, "Failed to allocate output buffer");
        emulator_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!hw_init()) {
        ESP_LOGE(TAG, "hw_init failed!");
        emulator_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (gnuboy_load_rom(rom_path) != 0) {
        ESP_LOGE(TAG, "Failed to load ROM: %s", rom_path);
        emulator_running = false;
        vTaskDelete(NULL);
        return;
    }

    gnuboy_reset(true);
    gnuboy_set_palette(GB_PALETTE_CGB);

    emulator_running = true;
    ESP_LOGI(TAG, "Emulator started, running...");

    uint32_t frame_count = 0;
    uint32_t last_fps_check = esp_timer_get_time();

    while (emulator_running) {
        gnuboy_set_pad(gb_port_get_pad());
        gnuboy_run(true);

        frame_count++;

        uint32_t now = esp_timer_get_time();
        if (now - last_fps_check > 1000000) {
            ESP_LOGI(TAG, "FPS: %d", frame_count);
            frame_count = 0;
            last_fps_check = now;
        }

        if (frame_ready) {
            frame_ready = false;
            gb_display_adapter_update_rgb(output_buffer);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "Emulator task finished");
    gnuboy_free_rom();

    if (output_buffer) {
        heap_caps_free(output_buffer);
        output_buffer = NULL;
    }

    emulator_running = false;
    vTaskDelete(NULL);
}

// ============================================================================
//  ПУБЛИЧНЫЕ ФУНКЦИИ
// ============================================================================

bool gb_emulator_init(const char *rom_path)
{
    if (emulator_running) {
        ESP_LOGW(TAG, "Emulator already running");
        return false;
    }

    ESP_LOGI(TAG, "Initializing GameBoy emulator: %s", rom_path);

    output_buffer = (uint16_t *)heap_caps_calloc(
        160 * 144, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!output_buffer) {
        ESP_LOGE(TAG, "Failed to allocate output buffer");
        return false;
    }

    frame_semaphore = xSemaphoreCreateBinary();
    if (!frame_semaphore) {
        ESP_LOGE(TAG, "Failed to create frame semaphore");
        heap_caps_free(output_buffer);
        output_buffer = NULL;
        return false;
    }

    char path_copy[256];
    strncpy(path_copy, rom_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    BaseType_t result = xTaskCreatePinnedToCore(
        gb_emulator_task,
        "gb_emu",
        16384,
        path_copy,
        18,
        &emulator_task_handle,
        1
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create emulator task");
        vSemaphoreDelete(frame_semaphore);
        frame_semaphore = NULL;
        heap_caps_free(output_buffer);
        output_buffer = NULL;
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Emulator initialized successfully");
    return true;
}

void gb_emulator_deinit(void)
{
    if (!emulator_running) return;

    ESP_LOGI(TAG, "Deinitializing emulator...");

    emulator_running = false;

    if (emulator_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        emulator_task_handle = NULL;
    }

    if (frame_semaphore) {
        vSemaphoreDelete(frame_semaphore);
        frame_semaphore = NULL;
    }

    if (output_buffer) {
        heap_caps_free(output_buffer);
        output_buffer = NULL;
    }

    if (gb_host.video.buffer16) {
        heap_caps_free(gb_host.video.buffer16);
        gb_host.video.buffer16 = NULL;
    }
    if (gb_host.video.buffer8) {
        heap_caps_free(gb_host.video.buffer8);
        gb_host.video.buffer8 = NULL;
    }
    if (gb_host.audio.buffer) {
        heap_caps_free(gb_host.audio.buffer);
        gb_host.audio.buffer = NULL;
    }

    ESP_LOGI(TAG, "Emulator deinitialized");
}

void gb_emulator_run_frame(void) {}

bool gb_emulator_is_running(void)
{
    return emulator_running;
}

void gb_emulator_reset(void)
{
    if (emulator_running) {
        gnuboy_reset(false);
    }
}

uint16_t* gb_emulator_get_framebuffer(void)
{
    return output_buffer;
}

void gb_emulator_set_button(uint8_t button, bool pressed)
{
    int pad = gb_port_get_pad();
    if (pressed) {
        pad |= button;
    } else {
        pad &= ~button;
    }
    gb_port_set_pad(pad);
}

void gb_emulator_update_input(void) {}

bool gb_emulator_load_state(const char *path)
{
    return gb_port_load_state(path) == 0;
}

bool gb_emulator_save_state(const char *path)
{
    return gb_port_save_state(path) == 0;
}