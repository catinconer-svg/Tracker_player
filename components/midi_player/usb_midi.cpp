#include "usb_midi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "tusb.h"

static const char *TAG = "USB_MIDI";
static bool is_initialized = false;

/* Очередь для MIDI сообщений из USB */
static QueueHandle_t midi_queue = NULL;
#define MIDI_QUEUE_SIZE 32

/* Структура MIDI пакета */
typedef struct {
    uint8_t header;
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
} midi_packet_t;

/* ★ ДЕСКРИПТОР УСТРОЙСТВА ★ */
static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCafe,
    .idProduct = 0x4008,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

/* ★ СТРОКОВЫЕ ДЕСКРИПТОРЫ ★ */
static const char* desc_strings[] = {
    (const char[]) { 0x09, 0x04 },
    "Espressif Systems",
    "ESP32-S3 MIDI Synth",
    "123456",
};

/* ★ ДЕСКРИПТОР КОНФИГУРАЦИИ ★ */
enum {
    ITF_NUM_MIDI = 0,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_TOTAL
};

#define EPNUM_MIDI_OUT  0x01
#define EPNUM_MIDI_IN   0x81
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, EPNUM_MIDI_OUT, (0x80 | EPNUM_MIDI_IN), 64)
};

/* ★ ТОЛЬКО КОЛБЭК ДЛЯ ПОЛУЧЕНИЯ MIDI ★ */
void tud_midi_rx_cb(uint8_t itf) {
    uint8_t packet[4];
    while (tud_midi_available()) {
        if (tud_midi_packet_read(packet)) {
            if (midi_queue) {
                midi_packet_t pkt;
                pkt.header = packet[0];
                pkt.byte1 = packet[1];
                pkt.byte2 = packet[2];
                pkt.byte3 = packet[3];
                xQueueSend(midi_queue, &pkt, 0);
                

            }
        }
    }
}

bool usb_midi_init(void) {
    if (is_initialized) return true;
    
    ESP_LOGI(TAG, "USB-MIDI init...");
    
    /* Создаём очередь */
    midi_queue = xQueueCreate(MIDI_QUEUE_SIZE, sizeof(midi_packet_t));
    if (!midi_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return false;
    }
    
    /* Настраиваем TinyUSB */
    tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 0,
        },
        .descriptor = {
            .device = &desc_device,
            .qualifier = NULL,
            .string = desc_strings,
            .string_count = 4,
            .full_speed_config = desc_fs_configuration,
            .high_speed_config = NULL,
        },
        .event_cb = NULL,
        .event_arg = NULL,
    };
    
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install: %s", esp_err_to_name(err));
        vQueueDelete(midi_queue);
        midi_queue = NULL;
        return false;
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "USB-MIDI ready!");
    return true;
}

bool usb_midi_available(void) {
    if (!is_initialized || !midi_queue) return false;
    return (uxQueueMessagesWaiting(midi_queue) > 0);
}

bool usb_midi_read(uint8_t packet[4]) {
    if (!is_initialized || !packet || !midi_queue) return false;
    
    midi_packet_t pkt;
    if (xQueueReceive(midi_queue, &pkt, 0) == pdTRUE) {
        packet[0] = pkt.header;
        packet[1] = pkt.byte1;
        packet[2] = pkt.byte2;
        packet[3] = pkt.byte3;
        return true;
    }
    return false;
}

static void usb_midi_send(uint8_t header, uint8_t byte1, uint8_t byte2, uint8_t byte3) {
    if (!is_initialized) return;
    if (!tud_midi_mounted()) return;
    
    uint8_t packet[4] = {header, byte1, byte2, byte3};
    tud_midi_packet_write(packet);
}

void usb_midi_send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    usb_midi_send(0x09, 0x90 | (channel & 0x0F), note, velocity);
}

void usb_midi_send_note_off(uint8_t channel, uint8_t note, uint8_t velocity) {
    usb_midi_send(0x08, 0x80 | (channel & 0x0F), note, velocity);
}

void usb_midi_send_control_change(uint8_t channel, uint8_t control, uint8_t value) {
    usb_midi_send(0x0B, 0xB0 | (channel & 0x0F), control, value);
}

void usb_midi_send_program_change(uint8_t channel, uint8_t program) {
    usb_midi_send(0x0C, 0xC0 | (channel & 0x0F), program, 0);
}

void usb_midi_send_pitch_bend(uint8_t channel, int value) {
    usb_midi_send(0x0E, 0xE0 | (channel & 0x0F), value & 0x7F, (value >> 7) & 0x7F);
}

void usb_midi_deinit(void) {
    if (!is_initialized) return;
    
    if (midi_queue) {
        vQueueDelete(midi_queue);
        midi_queue = NULL;
    }
    
    tinyusb_driver_uninstall();
    is_initialized = false;
    ESP_LOGI(TAG, "USB-MIDI deinit");
}