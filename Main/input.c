#include "input.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_player.h"
#include "settings_ui.h"
#include "gb_emulator.hpp"
#include "gnuboy.h"   // GB_PAD_* (enum)
static const char *TAG = "INPUT";

// ==== НОВАЯ СХЕМА: 12 кнопок, энкодера нет ====
#define KEY_UP      GPIO_NUM_12
#define KEY_DOWN    GPIO_NUM_10
#define KEY_LEFT    GPIO_NUM_13
#define KEY_RIGHT   GPIO_NUM_11
#define KEY_A       GPIO_NUM_41
#define KEY_B       GPIO_NUM_21
#define KEY_X       GPIO_NUM_40
#define KEY_Y       GPIO_NUM_39
#define KEY_START   GPIO_NUM_48
#define KEY_SELECT  GPIO_NUM_45
#define KEY_MENU    GPIO_NUM_0
#define KEY_OPTION  GPIO_NUM_38

#define DEBOUNCE_US     5000
#define REPEAT_DELAY_US 400000   // задержка перед автоповтором
#define REPEAT_RATE_US  150000   // период автоповтора

// Битовые маски логических кнопок (бит СБРОШЕН = нажата)
#define BM_UP      (1u << 0)
#define BM_DOWN    (1u << 1)
#define BM_LEFT    (1u << 2)
#define BM_RIGHT   (1u << 3)
#define BM_A       (1u << 4)
#define BM_B       (1u << 5)
#define BM_X       (1u << 6)
#define BM_Y       (1u << 7)
#define BM_START   (1u << 8)
#define BM_SELECT  (1u << 9)
#define BM_MENU    (1u << 10)
#define BM_OPTION  (1u << 11)

#define NUM_KEYS   12

static lv_indev_t *kb_indev;
static lv_group_t *input_group;
static input_mode_t current_mode = INPUT_MODE_FILE_EXPLORER;

static volatile uint16_t btn_states = 0x0FFF;   // текущее состояние (1 = отжата)
static volatile uint16_t btn_prev = 0x0FFF;     // состояние в прошлом опросе
static volatile int64_t last_btn_time = 0;

// Автоповтор удерживаемых кнопок
static int64_t press_time[NUM_KEYS]   = {0};    // когда кнопка была нажата
static int64_t repeat_time[NUM_KEYS]  = {0};    // когда был последний автоповтор

volatile bool open_settings_flag = false;
volatile bool exit_settings_flag = false;

static volatile bool in_settings = false;

static lv_obj_t *player_screen_ref = NULL;

void input_set_player_screen(lv_obj_t *screen) {
    player_screen_ref = screen;
}

void input_set_in_settings(bool state) {
    in_settings = state;
    ESP_LOGI(TAG, "Settings mode: %s", state ? "ACTIVE" : "INACTIVE");
    btn_prev = btn_states;   // не генерируем фантомных событий при смене режима
    if (state) {
        open_settings_flag = false;
    }
}

bool input_get_in_settings(void) {
    return in_settings;
}

// Чтение всех кнопок с дребезгом. Возвращает bitmask: бит СБРОШЕН = кнопка нажата.
static uint16_t read_buttons(void)
{
    int64_t now = esp_timer_get_time();
    if (now - last_btn_time < DEBOUNCE_US) return btn_states;
    last_btn_time = now;

    uint16_t s = 0;
    s |= (uint16_t)gpio_get_level(KEY_UP)     << 0;
    s |= (uint16_t)gpio_get_level(KEY_DOWN)   << 1;
    s |= (uint16_t)gpio_get_level(KEY_LEFT)   << 2;
    s |= (uint16_t)gpio_get_level(KEY_RIGHT)  << 3;
    s |= (uint16_t)gpio_get_level(KEY_A)      << 4;
    s |= (uint16_t)gpio_get_level(KEY_B)      << 5;
    s |= (uint16_t)gpio_get_level(KEY_X)      << 6;
    s |= (uint16_t)gpio_get_level(KEY_Y)      << 7;
    s |= (uint16_t)gpio_get_level(KEY_START)  << 8;
    s |= (uint16_t)gpio_get_level(KEY_SELECT) << 9;
    s |= (uint16_t)gpio_get_level(KEY_MENU)   << 10;
    s |= (uint16_t)gpio_get_level(KEY_OPTION) << 11;

    btn_states = s;
    return btn_states;
}

static inline bool pressed(uint16_t cur, uint16_t prev, uint16_t bm) {
    return !(cur & bm) && (prev & bm);      // фронт: была отжата -> нажата
}

static inline bool held(uint16_t cur, uint16_t bm) {
    return !(cur & bm);
}

static inline bool released_edge(uint16_t cur, uint16_t prev, uint16_t bm) {
    return (cur & bm) && !(prev & bm);      // спад: была нажата -> отжата
}

// Обновить таймеры автоповтора и вернуть true, если в этом кадре нужно
// сгенерировать повторное событие для удерживаемой кнопки bm.
static bool key_repeat(uint16_t cur, uint16_t prev, uint16_t bm, int idx)
{
    int64_t now = esp_timer_get_time();

    if (pressed(cur, prev, bm)) {           // только что нажата
        press_time[idx] = now;
        repeat_time[idx] = 0;
        return false;                       // первое событие даёт вызывающий код
    }

    if (!held(cur, bm)) {                   // отжата — сброс
        press_time[idx] = 0;
        repeat_time[idx] = 0;
        return false;
    }

    if (press_time[idx] == 0) return false;

    if (repeat_time[idx] == 0) {            // ждём начальную задержку
        if (now - press_time[idx] >= REPEAT_DELAY_US) {
            repeat_time[idx] = now;
            return true;
        }
        return false;
    }

    if (now - repeat_time[idx] >= REPEAT_RATE_US) {
        repeat_time[idx] = now;
        return true;
    }
    return false;
}

static void keyboard_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t btns = read_buttons();
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;

    /* ★ ★ ★ GAMEBOY (приоритет) ★ ★ ★ */
    if (gb_emulator_is_running()) {
        // D-pad: активна всё время нажатия
        gb_emulator_set_button(GB_PAD_UP,    held(btns, BM_UP));
        gb_emulator_set_button(GB_PAD_DOWN,  held(btns, BM_DOWN));
        gb_emulator_set_button(GB_PAD_LEFT,  held(btns, BM_LEFT));
        gb_emulator_set_button(GB_PAD_RIGHT, held(btns, BM_RIGHT));

        // A/X → GameBoy A, B/Y → GameBoy B
        if (pressed(btns, btn_prev, BM_A) || pressed(btns, btn_prev, BM_X))
            gb_emulator_set_button(GB_PAD_A, true);
        if (released_edge(btns, btn_prev, BM_A) && !held(btns, BM_X))
            gb_emulator_set_button(GB_PAD_A, false);

        if (pressed(btns, btn_prev, BM_B) || pressed(btns, btn_prev, BM_Y))
            gb_emulator_set_button(GB_PAD_B, true);
        if (released_edge(btns, btn_prev, BM_B) && !held(btns, BM_Y))
            gb_emulator_set_button(GB_PAD_B, false);

        // START / SELECT — теперь это отдельные физические кнопки
        if (pressed(btns, btn_prev, BM_START))
            gb_emulator_set_button(GB_PAD_START, true);
        if (released_edge(btns, btn_prev, BM_START))
            gb_emulator_set_button(GB_PAD_START, false);

        if (pressed(btns, btn_prev, BM_SELECT))
            gb_emulator_set_button(GB_PAD_SELECT, true);
        if (released_edge(btns, btn_prev, BM_SELECT))
            gb_emulator_set_button(GB_PAD_SELECT, false);

        gb_emulator_update_input();

        btn_prev = btns;
        return;  // ★ ВЫХОДИМ - НИЧЕГО БОЛЬШЕ НЕ ОБРАБАТЫВАЕМ ★
    }

    /* ★ НАСТРОЙКИ ★ */
    if (current_mode == INPUT_MODE_PLAYER && in_settings) {
        if (pressed(btns, btn_prev, BM_B) || pressed(btns, btn_prev, BM_MENU)) {
            settings_ui_handle_esc();
            data->key = LV_KEY_ESC;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (pressed(btns, btn_prev, BM_A)) {
            data->key = LV_KEY_ENTER;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (pressed(btns, btn_prev, BM_UP) || key_repeat(btns, btn_prev, BM_UP, 0)) {
            data->key = LV_KEY_UP;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (pressed(btns, btn_prev, BM_DOWN) || key_repeat(btns, btn_prev, BM_DOWN, 1)) {
            data->key = LV_KEY_DOWN;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (pressed(btns, btn_prev, BM_LEFT) || key_repeat(btns, btn_prev, BM_LEFT, 2)) {
            data->key = LV_KEY_LEFT;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (pressed(btns, btn_prev, BM_RIGHT) || key_repeat(btns, btn_prev, BM_RIGHT, 3)) {
            data->key = LV_KEY_RIGHT;
            data->state = LV_INDEV_STATE_PRESSED;
        }

        btn_prev = btns;
        return;
    }

    /* ★ ПЛЕЕР (если не GameBoy) ★ */
    if (current_mode == INPUT_MODE_PLAYER && !in_settings) {
        // A: Play/Pause
        if (pressed(btns, btn_prev, BM_A)) audio_player_toggle_pause();

        // B: Stop
        if (pressed(btns, btn_prev, BM_B)) audio_player_stop();

        // LEFT: пред. трек, RIGHT: след. трек (с автоповтором)
        if (pressed(btns, btn_prev, BM_LEFT) || key_repeat(btns, btn_prev, BM_LEFT, 2))
            audio_player_prev_track();
        if (pressed(btns, btn_prev, BM_RIGHT) || key_repeat(btns, btn_prev, BM_RIGHT, 3))
            audio_player_next_track();

        // DOWN: режим повтора
        if (pressed(btns, btn_prev, BM_DOWN)) audio_player_toggle_repeat();

        // UP или MENU: открыть настройки
        if (pressed(btns, btn_prev, BM_UP) || pressed(btns, btn_prev, BM_MENU))
            open_settings_flag = true;

        // X: громкость вниз, Y: громкость вверх (с автоповтором)
        // (раньше громкость крутилась энкодером)
        if (pressed(btns, btn_prev, BM_X) || key_repeat(btns, btn_prev, BM_X, 6))
            audio_player_volume_down();
        if (pressed(btns, btn_prev, BM_Y) || key_repeat(btns, btn_prev, BM_Y, 7))
            audio_player_volume_up();

        // START: mute (как кнопка энкодера раньше)
        if (pressed(btns, btn_prev, BM_START)) audio_player_toggle_mute();

        btn_prev = btns;
        return;
    }

    /* ★ FILE EXPLORER ★ */
    // Up/Down — навигация по списку с автоповтором (энкодера больше нет)
    if (pressed(btns, btn_prev, BM_UP) || key_repeat(btns, btn_prev, BM_UP, 0)) {
        data->key = LV_KEY_UP; data->state = LV_INDEV_STATE_PRESSED;
    } else if (released_edge(btns, btn_prev, BM_UP)) {
        data->key = LV_KEY_UP; data->state = LV_INDEV_STATE_RELEASED;
    }

    if (pressed(btns, btn_prev, BM_DOWN) || key_repeat(btns, btn_prev, BM_DOWN, 1)) {
        data->key = LV_KEY_DOWN; data->state = LV_INDEV_STATE_PRESSED;
    } else if (released_edge(btns, btn_prev, BM_DOWN)) {
        data->key = LV_KEY_DOWN; data->state = LV_INDEV_STATE_RELEASED;
    }

    // Enter: A или START
    if (pressed(btns, btn_prev, BM_A) || pressed(btns, btn_prev, BM_START)) {
        data->key = LV_KEY_ENTER; data->state = LV_INDEV_STATE_PRESSED;
    } else if (released_edge(btns, btn_prev, BM_A) || released_edge(btns, btn_prev, BM_START)) {
        data->key = LV_KEY_ENTER; data->state = LV_INDEV_STATE_RELEASED;
    }

    // Backspace (наверх по каталогу): B или MENU
    if (pressed(btns, btn_prev, BM_B) || pressed(btns, btn_prev, BM_MENU)) {
        data->key = LV_KEY_BACKSPACE; data->state = LV_INDEV_STATE_PRESSED;
    } else if (released_edge(btns, btn_prev, BM_B) || released_edge(btns, btn_prev, BM_MENU)) {
        data->key = LV_KEY_BACKSPACE; data->state = LV_INDEV_STATE_RELEASED;
    }

    btn_prev = btns;
}

static void gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY_UP) | (1ULL << KEY_DOWN) |
                        (1ULL << KEY_LEFT) | (1ULL << KEY_RIGHT) |
                        (1ULL << KEY_A) | (1ULL << KEY_B) |
                        (1ULL << KEY_X) | (1ULL << KEY_Y) |
                        (1ULL << KEY_START) | (1ULL << KEY_SELECT) |
                        (1ULL << KEY_MENU) | (1ULL << KEY_OPTION),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    btn_states = read_buttons();
    btn_prev = btn_states;
}

void input_init(void)
{
    gpio_init();

    input_group = lv_group_create();
    lv_group_set_default(input_group);
    lv_group_set_editing(input_group, false);

    ESP_LOGI(TAG, "input_init: created group at %p, set as default (Navigation Mode)", (void*)input_group);

    kb_indev = lv_indev_create();
    lv_indev_set_type(kb_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kb_indev, keyboard_read_cb);
    lv_indev_set_group(kb_indev, input_group);

    ESP_LOGI(TAG, "Input initialized (12 buttons, no encoder)");
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
