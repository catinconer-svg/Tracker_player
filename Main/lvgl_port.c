/**
 * @file lvgl_port.c
 * @brief LVGL port for ST7789 display (320x240)
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
#include "esp_lcd_st7789.h"
#include "esp_log.h"

// --- Пины дисплея (новая схема, ESP32-S3) ---
#define PIN_LCD_MOSI    GPIO_NUM_18
#define PIN_LCD_SCLK    GPIO_NUM_17
#define PIN_LCD_CS      GPIO_NUM_9
#define PIN_LCD_DC      GPIO_NUM_8
#define PIN_LCD_RST     GPIO_NUM_3
#define PIN_LCD_BL      GPIO_NUM_16

// --- Разрешение экрана (ST7789, ландшафт 320x240) ---
#define LCD_H_RES       320
#define LCD_V_RES       240

// --- SPI хост ---
#define LCD_SPI_HOST    SPI2_HOST

#define LVGL_TICK_PERIOD_MS     1

static const char *TAG = "LVGL_PORT";

// --- Таблица инициализации ST7789 (320x240) ---
// MADCTL (0x36) = 0xA0 задан здесь же, ниже в таблице
static const st7789_lcd_init_cmd_t st7789_init_cmds[] = {
    // cmd, data, bytes, delay_ms
    {0xB2, (uint8_t[]){0x05, 0x05, 0x00, 0x11, 0x11}, 5, 0},   // Porch Setting (минимум для разгона)
    {0xBB, (uint8_t[]){0x32}, 1, 0},                           // VCOMS control
    {0xC0, (uint8_t[]){0x2C}, 1, 0},                           // LCM control
    {0xC2, (uint8_t[]){0x01}, 1, 0},                           // VDV and VRH command enable
    {0xC3, (uint8_t[]){0x12}, 1, 0},                           // VRH set
    {0xC4, (uint8_t[]){0x20}, 1, 0},                           // VDV set
    {0xC6, (uint8_t[]){0x01}, 1, 0},                           // Frame Rate Control (0x01 = 111Hz)
    {0xD0, (uint8_t[]){0xA4, 0xA1}, 2, 0},                     // Power Control 1
    {0x3A, (uint8_t[]){0x05}, 1, 0},                           // 16-bit/pixel
    {0x36, (uint8_t[]){0xA0}, 1, 0},                           // MADCTL: MY|MX — ландшафт
    {0xE0, (uint8_t[]){0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                      0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14, 0}, // Positive Gamma
    {0xE1, (uint8_t[]){0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                      0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14, 0}, // Negative Gamma
    {0x21, NULL, 0, 0},                                        // Display Inversion ON (для ST7789)
};

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
        .max_transfer_sz = LCD_H_RES * (LCD_V_RES / 3) * 2,
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

    // 3. Создание панели ST7789 с кастомной таблицей инициализации
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &(esp_lcd_panel_vendor_st7789_t){
            .reset_sequence_us = 5000,
            .init_sequence_us = 5000,
            .invert_on = true,          // инверсия включена (ST7789 требует 0x21)
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .cmds_to_submit = st7789_init_cmds,
            .num_of_cmds = sizeof(st7789_init_cmds) / sizeof(st7789_init_cmds[0]),
        },
    };
    err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
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
    // Инверсия цвета уже выставлена через vendor_config (.invert_on = true)
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Panel initialized");

    // 5. Ориентация: MADCTL=0xA0 из таблицы -> нативный ландшафт 320x240,
    //    дополнительные swap/mirror не требуются.

    // 6. Инициализация LVGL
    lv_init();

    // Буферы на треть экрана для частичной отрисовки
    const size_t buf_pixels = LCD_H_RES * (LCD_V_RES / 3);
    buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffers");
        return NULL;
    }

    // Создаем дисплей 320x240
    disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(disp, buf1, buf2, buf_pixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
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

    ESP_LOGI(TAG, "LVGL port initialized successfully (320x240 ST7789)");
    return disp;
}