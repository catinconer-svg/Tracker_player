#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"

/**
 * @brief Initialize SD card on SPI bus and mount FAT filesystem.
 * @return ESP_OK on success, otherwise error code.
 */
esp_err_t sd_card_init(void);

#endif /* SD_CARD_H */