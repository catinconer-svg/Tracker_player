#include "sd_card.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "SD_CARD";

// --- Пины SD карты (из твоей таблицы подключения) ---
#define PIN_SD_MOSI     13
#define PIN_SD_MISO     12
#define PIN_SD_CLK      10
#define PIN_SD_CS       14

// --- SPI хост для SD карты ---
#define SD_SPI_HOST     SPI2_HOST

// --- Точка монтирования ---
#define MOUNT_POINT     "/sdcard"

esp_err_t sd_card_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");

    // Настройка SPI шины для SD карты
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Настройка хоста SDMMC
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // Настройка SD SPI конфигурации
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    // Опции монтирования FAT
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;

    ESP_LOGI(TAG, "Mounting filesystem...");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
        }
        spi_bus_free(SD_SPI_HOST);
        return ret;
    }

    // Информация о карте
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card initialized successfully!");
    
    return ESP_OK;
}