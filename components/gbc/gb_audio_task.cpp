#include "gb_emulator.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"

static const char *TAG = "GB_AUDIO";

// ★ АУДИО-ОЧЕРЕДЬ ★
static QueueHandle_t audio_queue = NULL;
static bool audio_task_running = false;

// ★ АУДИО-БУФЕРЫ (двойная буферизация) ★
#define AUDIO_BUFFER_SAMPLES 4096
static int16_t audio_buffers[2][AUDIO_BUFFER_SAMPLES];
static volatile int audio_buffer_index = 0;

// ★ ВНЕШНИЙ УКАЗАТЕЛЬ НА I2S ★
extern i2s_chan_handle_t tx_chan;

// ★ АУДИО-ЗАДАЧА ★
static void gb_audio_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started");
    audio_task_running = true;
    
    uint8_t *buffer_ptr;
    
    while (audio_task_running) {
        // ★ ЖДЁМ НОВЫЙ БУФЕР ★
        if (xQueueReceive(audio_queue, &buffer_ptr, portMAX_DELAY) == pdTRUE) {
            if (buffer_ptr == NULL) break;
            
            // ★ ОТПРАВЛЯЕМ В I2S ★
            if (tx_chan) {
                size_t bytes_written;
                esp_err_t ret = i2s_channel_write(tx_chan, buffer_ptr, 
                                                  AUDIO_BUFFER_SAMPLES * sizeof(int16_t),
                                                  &bytes_written, portMAX_DELAY);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(ret));
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "Audio task stopped");
    audio_task_running = false;
    vTaskDelete(NULL);
}

// ★ ИНИЦИАЛИЗАЦИЯ АУДИО ★
void gb_audio_init(void) {
    if (audio_queue == NULL) {
        audio_queue = xQueueCreate(2, sizeof(uint8_t*));
        if (audio_queue) {
            xTaskCreatePinnedToCore(gb_audio_task, "gb_audio", 4096, NULL, 10, NULL, 1);
            ESP_LOGI(TAG, "Audio initialized");
        }
    }
}

// ★ ОТПРАВКА ЗВУКА В ОЧЕРЕДЬ ★
void gb_audio_submit(int16_t *buffer, size_t samples) {
    if (!audio_queue || !audio_task_running || samples == 0) return;
    
    // ★ КОПИРУЕМ В БУФЕР ★
    size_t copy_samples = (samples < AUDIO_BUFFER_SAMPLES) ? samples : AUDIO_BUFFER_SAMPLES;
    int current = audio_buffer_index;
    memcpy(audio_buffers[current], buffer, copy_samples * sizeof(int16_t));
    
    // ★ ОТПРАВЛЯЕМ В ОЧЕРЕДЬ ★
    uint8_t *ptr = (uint8_t*)audio_buffers[current];
    if (xQueueSend(audio_queue, &ptr, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Audio queue full, dropping frame");
    }
    
    // ★ ПЕРЕКЛЮЧАЕМ БУФЕР ★
    audio_buffer_index = (audio_buffer_index + 1) % 2;
}