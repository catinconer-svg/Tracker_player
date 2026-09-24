#include "input.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_player.h"
#include "settings_ui.h"
#include "gb_emulator.hpp"
#include "gnuboy.h"   // ← ДОБАВИТЬ ЭТУ СТРОКУ
static const char *TAG = "INPUT";

#define PIN_ENC_A       39
#define PIN_ENC_B       38
#define PIN_ENC_BUTTON  21

#define PIN_BTN_UP      40
#define PIN_BTN_DOWN    5
#define PIN_BTN_LEFT    41
#define PIN_BTN_RIGHT   4
#define PIN_BTN_A       3
#define PIN_BTN_B       9

#define DEBOUNCE_US     5000

static lv_indev_t *encoder_indev;
static lv_indev_t *kb_indev;
static lv_group_t *input_group;
static input_mode_t current_mode = INPUT_MODE_FILE_EXPLORER;

static volatile int32_t enc_diff = 0;
static volatile uint8_t enc_state = 0;
static volatile int enc_button_state = 1;

static volatile uint8_t btn_states = 0xFF;
static volatile uint8_t btn_prev = 0xFF;
static volatile int64_t last_btn_time = 0;

volatile bool open_settings_flag = false;
volatile bool exit_settings_flag = false;

static volatile bool in_settings = false;
static volatile bool btn_b_was_pressed = false;
static volatile int64_t btn_b_first_press = 0;

static lv_obj_t *player_screen_ref = NULL;

// ★ КОНСТАНТЫ КНОПОК GAMEBOY (PAD_* из gnuboy) ★
#define PAD_RIGHT  0x01
#define PAD_LEFT   0x02
#define PAD_UP     0x04
#define PAD_DOWN   0x08
#define PAD_A      0x10
#define PAD_B      0x20
#define PAD_SELECT 0x40
#define PAD_START  0x80

void input_set_player_screen(lv_obj_t *screen) {
    player_screen_ref = screen;
}

void input_set_in_settings(bool state) {
    in_settings = state;
    ESP_LOGI(TAG, "Settings mode: %s", state ? "ACTIVE" : "INACTIVE");
    btn_prev = btn_states;
    if (state) {
        open_settings_flag = false;
    }
}

bool input_get_in_settings(void) {
    return in_settings;
}

static const int8_t encoder_table[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0
};

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int a = gpio_get_level(PIN_ENC_A);
    int b = gpio_get_level(PIN_ENC_B);
    uint8_t new_state = (a << 1) | b;
    
    if (new_state != enc_state) {
        uint8_t index = (enc_state << 2) | new_state;
        enc_diff += encoder_table[index];
        enc_state = new_state;
    }

    /* ★ GAMEBOY: энкодер уже обрабатывается в keyboard_read_cb ★ */
    if (gb_emulator_is_running()) {
        /* ★ ТОЛЬКО КНОПКА ЭНКОДЕРА ДЛЯ START ★ */
        int btn = gpio_get_level(PIN_ENC_BUTTON);
        if (btn == 0 && enc_button_state == 1) {
            // START уже обрабатывается в keyboard_read_cb
            enc_button_state = 0;
        } else if (btn == 1 && enc_button_state == 0) {
            enc_button_state = 1;
        }
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (current_mode == INPUT_MODE_PLAYER && in_settings) {
        data->enc_diff = enc_diff;
        enc_diff = 0;
        
        int btn = gpio_get_level(PIN_ENC_BUTTON);
        if (btn == 0 && enc_button_state == 1) {
            data->state = LV_INDEV_STATE_PRESSED;
            enc_button_state = 0;
        } else if (btn == 1 && enc_button_state == 0) {
            data->state = LV_INDEV_STATE_RELEASED;
            enc_button_state = 1;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }
    
    if (current_mode == INPUT_MODE_PLAYER && !in_settings) {
        if (enc_diff > 0) {
            for (int i = 0; i < enc_diff; i++) audio_player_volume_up();
            enc_diff = 0;
        } else if (enc_diff < 0) {
            for (int i = 0; i < -enc_diff; i++) audio_player_volume_down();
            enc_diff = 0;
        }

        int btn = gpio_get_level(PIN_ENC_BUTTON);
        if (btn == 0 && enc_button_state == 1) {
            audio_player_toggle_mute();
            enc_button_state = 0;
        } else if (btn == 1) {
            enc_button_state = 1;
        }

        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    data->enc_diff = enc_diff;
    enc_diff = 0;
    
    int btn = gpio_get_level(PIN_ENC_BUTTON);
    if (btn == 0 && enc_button_state == 1) {
        data->state = LV_INDEV_STATE_PRESSED;
        enc_button_state = 0;
    } else if (btn == 1 && enc_button_state == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        enc_button_state = 1;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint8_t read_buttons(void)
{
    int64_t now = esp_timer_get_time();
    if (now - last_btn_time < DEBOUNCE_US) return btn_states;
    last_btn_time = now;

    uint8_t new_state = 0;
    new_state |= (gpio_get_level(PIN_BTN_UP)    << 0);
    new_state |= (gpio_get_level(PIN_BTN_DOWN)  << 1);
    new_state |= (gpio_get_level(PIN_BTN_LEFT)  << 2);
    new_state |= (gpio_get_level(PIN_BTN_RIGHT) << 3);
    new_state |= (gpio_get_level(PIN_BTN_A)     << 4);
    new_state |= (gpio_get_level(PIN_BTN_B)     << 5);

    if (new_state == btn_states) return btn_states;
    btn_states = new_state;
    return btn_states;
}

static void keyboard_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t btns = read_buttons();
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;

    /* ★ ★ ★ СНАЧАЛА GAMEBOY ★ ★ ★ */
/* ★ ★ ★ СНАЧАЛА GAMEBOY ★ ★ ★ */
if (gb_emulator_is_running()) {
    // Кнопка A → GameBoy A
    if (!(btns & (1 << 4)) && (btn_prev & (1 << 4))) {
        gb_emulator_set_button(GB_PAD_A, true);
    }
    if ((btns & (1 << 4)) && !(btn_prev & (1 << 4))) {
        gb_emulator_set_button(GB_PAD_A, false);
    }

    // Кнопка B → GameBoy B
    if (!(btns & (1 << 5)) && (btn_prev & (1 << 5))) {
        gb_emulator_set_button(GB_PAD_B, true);
    }
    if ((btns & (1 << 5)) && !(btn_prev & (1 << 5))) {
        gb_emulator_set_button(GB_PAD_B, false);
    }

    // Кнопки UP/DOWN/LEFT/RIGHT
    // UP
    if (!(btns & (1 << 0))) {
        gb_emulator_set_button(GB_PAD_UP, true);
    } else {
        gb_emulator_set_button(GB_PAD_UP, false);
    }

    // DOWN
    if (!(btns & (1 << 1))) {
        gb_emulator_set_button(GB_PAD_DOWN, true);
    } else {
        gb_emulator_set_button(GB_PAD_DOWN, false);
    }

    // LEFT
    if (!(btns & (1 << 2))) {
        gb_emulator_set_button(GB_PAD_LEFT, true);
    } else {
        gb_emulator_set_button(GB_PAD_LEFT, false);
    }

    // RIGHT
    if (!(btns & (1 << 3))) {
        gb_emulator_set_button(GB_PAD_RIGHT, true);
    } else {
        gb_emulator_set_button(GB_PAD_RIGHT, false);
    }

    // START (кнопка энкодера)
    int enc_btn = gpio_get_level(PIN_ENC_BUTTON);
    if (enc_btn == 0 && enc_button_state == 1) {
        gb_emulator_set_button(GB_PAD_START, true);
        enc_button_state = 0;
    } else if (enc_btn == 1 && enc_button_state == 0) {
        gb_emulator_set_button(GB_PAD_START, false);
        enc_button_state = 1;
    }

    // SELECT (UP + DOWN одновременно)
    if (!(btns & (1 << 0)) && !(btns & (1 << 1))) {
        gb_emulator_set_button(GB_PAD_SELECT, true);
    } else {
        gb_emulator_set_button(GB_PAD_SELECT, false);
    }

    // Обновляем состояние эмулятора
    gb_emulator_update_input();

    btn_prev = btns;
    return;  // ★ ВЫХОДИМ - НИЧЕГО БОЛЬШЕ НЕ ОБРАБАТЫВАЕМ ★
}

    /* ★ НАСТРОЙКИ ★ */
    if (current_mode == INPUT_MODE_PLAYER && in_settings) {
        if (!(btns & (1 << 5)) && (btn_prev & (1 << 5))) {
            settings_ui_handle_esc();
            data->key = LV_KEY_ESC;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if ((btns & (1 << 5)) && !(btn_prev & (1 << 5))) {
            data->key = LV_KEY_ESC;
            data->state = LV_INDEV_STATE_RELEASED;
        }
        else if (!(btns & (1 << 0)) && (btn_prev & (1 << 0))) { data->key = LV_KEY_UP; data->state = LV_INDEV_STATE_PRESSED; }
        else if (!(btns & (1 << 1)) && (btn_prev & (1 << 1))) { data->key = LV_KEY_DOWN; data->state = LV_INDEV_STATE_PRESSED; }
        else if (!(btns & (1 << 4)) && (btn_prev & (1 << 4))) { data->key = LV_KEY_ENTER; data->state = LV_INDEV_STATE_PRESSED; }
        else if (!(btns & (1 << 2)) && (btn_prev & (1 << 2))) { data->key = LV_KEY_LEFT; data->state = LV_INDEV_STATE_PRESSED; }
        else if (!(btns & (1 << 3)) && (btn_prev & (1 << 3))) { data->key = LV_KEY_RIGHT; data->state = LV_INDEV_STATE_PRESSED; }
        
        btn_prev = btns;
        return;
    }

    /* ★ ПЛЕЕР (если не GameBoy) ★ */
    if (current_mode == INPUT_MODE_PLAYER && !in_settings) {
        // Кнопка A: Play/Pause
        if (!(btns & (1 << 4)) && (btn_prev & (1 << 4))) {
            audio_player_toggle_pause();
        }
        
        // Кнопка B: Stop (двойное нажатие)
        if (!(btns & (1 << 5)) && (btn_prev & (1 << 5))) {
            int64_t now = esp_timer_get_time();
            if (btn_b_was_pressed && (now - btn_b_first_press) < 800000) {
                audio_player_stop();
                btn_b_was_pressed = false;
            } else {
                audio_player_stop();
                btn_b_was_pressed = true;
                btn_b_first_press = now;
            }
        }
        if (btn_b_was_pressed && (esp_timer_get_time() - btn_b_first_press) > 1000000) {
            btn_b_was_pressed = false;
        }

        // Кнопка LEFT: пред. трек
        if (!(btns & (1 << 2)) && (btn_prev & (1 << 2))) audio_player_prev_track();
        // Кнопка RIGHT: след. трек
        if (!(btns & (1 << 3)) && (btn_prev & (1 << 3))) audio_player_next_track();
        // Кнопка DOWN: переключение режима повтора
        if (!(btns & (1 << 1)) && (btn_prev & (1 << 1))) audio_player_toggle_repeat();
        // Кнопка UP: открыть настройки
        if (!(btns & (1 << 0)) && (btn_prev & (1 << 0))) open_settings_flag = true;

        btn_prev = btns;
        return;
    }

    /* ★ FILE EXPLORER ★ */
    if (!(btns & (1 << 0))) { data->key = LV_KEY_UP; data->state = LV_INDEV_STATE_PRESSED; }
    else if (!(btn_prev & (1 << 0))) { data->key = LV_KEY_UP; data->state = LV_INDEV_STATE_RELEASED; }
    
    if (!(btns & (1 << 1))) { data->key = LV_KEY_DOWN; data->state = LV_INDEV_STATE_PRESSED; }
    else if (!(btn_prev & (1 << 1))) { data->key = LV_KEY_DOWN; data->state = LV_INDEV_STATE_RELEASED; }
    
    if (!(btns & (1 << 4))) { data->key = LV_KEY_ENTER; data->state = LV_INDEV_STATE_PRESSED; }
    else if (!(btn_prev & (1 << 4))) { data->key = LV_KEY_ENTER; data->state = LV_INDEV_STATE_RELEASED; }
    
    if (!(btns & (1 << 5))) { data->key = LV_KEY_BACKSPACE; data->state = LV_INDEV_STATE_PRESSED; }
    else if (!(btn_prev & (1 << 5))) { data->key = LV_KEY_BACKSPACE; data->state = LV_INDEV_STATE_RELEASED; }

    btn_prev = btns;
}

static void gpio_init(void)
{
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    io_conf.pin_bit_mask = (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B) | (1ULL << PIN_ENC_BUTTON);
    gpio_config(&io_conf);
    enc_state = (gpio_get_level(PIN_ENC_A) << 1) | gpio_get_level(PIN_ENC_B);
    enc_button_state = gpio_get_level(PIN_ENC_BUTTON);

    io_conf.pin_bit_mask = (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN) |
                           (1ULL << PIN_BTN_LEFT) | (1ULL << PIN_BTN_RIGHT) |
                           (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B);
    gpio_config(&io_conf);
}

void input_init(void)
{
    gpio_init();

    input_group = lv_group_create();
    lv_group_set_default(input_group);
    lv_group_set_editing(input_group, false);

    ESP_LOGI(TAG, "input_init: created group at %p, set as default (Navigation Mode)", (void*)input_group);

    encoder_indev = lv_indev_create();
    lv_indev_set_type(encoder_indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(encoder_indev, encoder_read_cb);
    lv_indev_set_group(encoder_indev, input_group);

    kb_indev = lv_indev_create();
    lv_indev_set_type(kb_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kb_indev, keyboard_read_cb);
    lv_indev_set_group(kb_indev, input_group);

    ESP_LOGI(TAG, "Input initialized");
}

void input_set_mode(input_mode_t mode)
{
    current_mode = mode;
    ESP_LOGI(TAG, "Mode: %s", mode == INPUT_MODE_FILE_EXPLORER ? "File Explorer" : "Player");
}

input_mode_t input_get_mode(void)
{
    return current_mode;
}