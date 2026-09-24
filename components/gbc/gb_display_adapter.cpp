#include "gb_display_adapter.h"
#include "../Main/lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "GB_DISPLAY_ADAPTER";

#define GB_WIDTH  160
#define GB_HEIGHT 144

static bool is_initialized = false;
static lv_obj_t *gb_container = NULL;

bool gb_display_adapter_init(lv_obj_t *parent)
{
    if (is_initialized) {
        ESP_LOGW(TAG, "Display adapter already initialized");
        return true;
    }

    if (!parent) {
        ESP_LOGE(TAG, "Parent is NULL!");
        return false;
    }

    ESP_LOGI(TAG, "Initializing GameBoy display adapter...");

    gb_container = lv_obj_create(parent);
    if (!gb_container) {
        ESP_LOGE(TAG, "Failed to create LVGL container");
        return false;
    }

    lv_obj_set_size(gb_container, GB_WIDTH, GB_HEIGHT);
    lv_obj_set_pos(gb_container, 0, 0);
    lv_obj_set_style_bg_color(gb_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(gb_container, 0, 0);
    lv_obj_set_style_radius(gb_container, 0, 0);
    lv_obj_set_style_pad_all(gb_container, 0, 0);

    lvgl_port_enable_gb_direct_mode(true);

    is_initialized = true;
    ESP_LOGI(TAG, "GameBoy display adapter initialized");
    return true;
}

void gb_display_adapter_update_rgb(uint16_t *rgb_buffer)
{
    if (!is_initialized || !rgb_buffer) {
        return;
    }

    lvgl_port_render_gb_direct_rgb(rgb_buffer);
}

void gb_display_adapter_deinit(void)
{
    if (!is_initialized) return;

    ESP_LOGI(TAG, "Deinitializing display adapter");

    if (gb_container) {
        lv_obj_del(gb_container);
        gb_container = NULL;
    }

    lvgl_port_enable_gb_direct_mode(false);

    is_initialized = false;
    ESP_LOGI(TAG, "Display adapter deinitialized");
}

bool gb_display_adapter_is_ready(void)
{
    return is_initialized;
}