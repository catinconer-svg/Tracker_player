/**
 * @file lvgl_port.c
 * @brief LVGL port for ST7735 display
 */

#include "lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"

// --- Пины дисплея ---
#define PIN_LCD_MOSI    18
#define PIN_LCD_SCLK    8
#define PIN_LCD_CS      6
#define PIN_LCD_DC      17
#define PIN_LCD_RST     7
#define PIN_LCD_BL      15

// --- Разрешение экрана ---
#define LCD_H_RES       128
#define LCD_V_RES       160

// --- SPI хост ---
#define LCD_SPI_HOST    SPI3_HOST

#define LVGL_TICK_PERIOD_MS     1

static const char *TAG = "LVGL_PORT";

static lv_display_t *disp;
static lv_color_t *buf1;
static lv_color_t *buf2;

// ★ ПРЯМОЙ ДОСТУП К ПАНЕЛИ ДЛЯ GAMEBOY ★
static esp_lcd_panel_handle_t gb_panel_handle = NULL;
static esp_lcd_panel_io_handle_t gb_io_handle = NULL;

// ★ ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ★
static bool gb_direct_mode = false;

// ★ ФУНКЦИЯ ВКЛЮЧЕНИЯ РЕЖИМА ★
void lvgl_port_enable_gb_direct_mode(bool enable) {
    gb_direct_mode = enable;
    ESP_LOGI(TAG, "GB direct mode: %s", enable ? "ON" : "OFF");
}

// ★ ФУНКЦИЯ ПРЯМОЙ ОТРИСОВКИ RGB565 ★
void lvgl_port_render_gb_direct_rgb(uint16_t *rgb_buffer) {
    if (!rgb_buffer || !gb_panel_handle) {
        return;
    }
    
    const int CROP_TOP = 8;
    const int CROP_HEIGHT = 128;
    const int CHUNK_HEIGHT = 32;  // ★ ОПТИМАЛЬНЫЙ РАЗМЕР ★
    
    // ★ РИСУЕМ БОЛЬШИМИ ЧАНКАМИ ★
    for (int y = 0; y < CROP_HEIGHT; y += CHUNK_HEIGHT) {
        int chunk_h = CHUNK_HEIGHT;
        if (y + chunk_h > CROP_HEIGHT) chunk_h = CROP_HEIGHT - y;
        
        const uint16_t *src = rgb_buffer + (CROP_TOP + y) * 160;
        
        esp_lcd_panel_draw_bitmap(gb_panel_handle, 
                                  0, y, 
                                  160, y + chunk_h, 
                                  src);
    }
    
    // ★ ТОЛЬКО ОДИН YIELD ПОСЛЕ ВСЕГО КАДРА ★
    taskYIELD();
}

// ★ FLUSH CALLBACK ★
static void lvgl_port_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    
    // ★ СТАНДАРТНАЯ ОТРИСОВКА ★
    esp_lcd_panel_draw_bitmap(panel_handle,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

static void lvgl_tick_timer_cb(void *arg)
{
    lv_tick_inc((uint32_t)(uintptr_t)arg);
}

lv_display_t *lvgl_port_init(void)
{
    // 1. Инициализация SPI шины
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 160 * 128 * 2,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "SPI bus initialized");

    // 2. Создание Panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 16,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.dc_low_on_data = 0,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "Panel IO created");

    // ★ СОХРАНЯЕМ IO_HANDLE ★
    gb_io_handle = io_handle;

    // 3. Создание панели ST7735
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7735 failed: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "Panel created");

    // ★ СОХРАНЯЕМ PANEL_HANDLE ДЛЯ GAMEBOY ★
    gb_panel_handle = panel_handle;

    // 4. Инициализация панели
    err = esp_lcd_panel_reset(panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_reset failed: %s", esp_err_to_name(err));
        return NULL;
    }
    err = esp_lcd_panel_init(panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_init failed: %s", esp_err_to_name(err));
        return NULL;
    }



    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Panel initialized");

    // 5. Поворот экрана
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, true, false);
    ESP_LOGI(TAG, "Panel rotated to 160x128");

uint8_t madctl_value = 0x60; 
esp_lcd_panel_io_tx_param(io_handle, 0x36, &madctl_value, 1);
ESP_LOGI(TAG, "MADCTL final: 0x%02X (RGB, rotated 160x128)", madctl_value);  
    // 6. Инициализация LVGL
    lv_init();
    
    // Буферы для 160x20
    buf1 = heap_caps_malloc(160 * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    buf2 = heap_caps_malloc(160 * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffers");
        return NULL;
    }

    // Создаем дисплей 160x128
    disp = lv_display_create(160, 128);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(disp, buf1, buf2, 160 * 20 * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lvgl_port_flush_cb);
    lv_display_set_user_data(disp, (void *)panel_handle);
    ESP_LOGI(TAG, "LVGL display created");

    // 7. Таймер для lv_tick_inc
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = lvgl_tick_timer_cb,
        .arg = (void *)(uintptr_t)LVGL_TICK_PERIOD_MS,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // 8. Подсветка
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "LVGL port initialized successfully (160x128)");
    return disp;
}