#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "sd_card.h"
#include "input.h"
#include "player_ui.h"
#include "audio_player.h"
#include "settings_ui.h"
#include "esp_heap_caps.h"


/* Убрали extern volatile bool in_settings; - теперь управляется через input_get/set_in_settings() */
extern volatile bool player_stopped_for_explorer;
extern volatile bool open_settings_flag;

static const char *TAG = "MAIN";
static lv_obj_t *player_screen = NULL;
lv_obj_t *main_screen = NULL;
static lv_obj_t *path_label = NULL;
static lv_obj_t *settings_screen = NULL;

static bool is_tracker_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    return is_supported_ext(ext); // Убедитесь, что эта функция объявлена где-то (например, в audio_player.h)
}

static void file_explorer_style_setup(lv_obj_t *explorer)
{
    lv_obj_t *header = lv_file_explorer_get_header(explorer);
    lv_obj_t *file_table = lv_file_explorer_get_file_table(explorer);

    lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(file_table, 0, 0);
    lv_obj_set_style_pad_left(file_table, 2, 0);
    lv_obj_set_style_pad_right(file_table, 2, 0);
    lv_obj_set_style_pad_all(file_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(file_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(file_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(file_table, &lv_font_montserrat_10, LV_PART_ITEMS);
    lv_obj_set_style_radius(file_table, 0, 0);
    lv_obj_set_style_radius(file_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(file_table, 0, 0);
    lv_obj_set_style_border_width(file_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(explorer, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_color(file_table, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_color(file_table, lv_color_hex(0x0A0A14), LV_PART_ITEMS);
    lv_obj_set_style_text_color(file_table, lv_color_hex(0x00CC00), 0);
    lv_obj_set_style_text_color(file_table, lv_color_hex(0x00CC00), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(file_table, lv_color_hex(0x004400), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(file_table, lv_color_hex(0x00FF00), LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_size(explorer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollbar_mode(file_table, LV_SCROLLBAR_MODE_OFF);
    
    lv_obj_set_height(file_table, LV_PCT(100));
    lv_obj_set_width(file_table, LV_PCT(100));
    lv_obj_set_style_margin_bottom(file_table, 16, 0);
    
    lv_obj_update_layout(explorer);
    lv_obj_update_layout(file_table);
}

static void path_update_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *explorer = lv_timer_get_user_data(timer);
    if (!explorer || !path_label) return;
    
    const char *cur_path = lv_file_explorer_get_current_path(explorer);
    if (!cur_path) return;
    
    static char last_path[256] = "";
    if (strcmp(last_path, cur_path) != 0) {
        strncpy(last_path, cur_path, sizeof(last_path) - 1);
        lv_label_set_text(path_label, cur_path);
    }
}



/* Функция для автоматического удаления popup */
static void close_msgbox_timer_cb(lv_timer_t *t)
{
    lv_obj_t *msg = (lv_obj_t *)lv_timer_get_user_data(t);
    if (msg) {
        lv_obj_del(msg);
    }
    lv_timer_del(t);
}

/* static void close_msgbox_cb(lv_timer_t *timer) { } // больше не нужна */

static void file_explorer_event_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *explorer = lv_event_get_target_obj(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        const char *cur_path = lv_file_explorer_get_current_path(explorer);
        const char *sel_fn = lv_file_explorer_get_selected_file_name(explorer);

        if (!sel_fn) return;

        const char *ext = strrchr(sel_fn, '.');
        
/* ★ ОБРАБОТКА SF2 ФАЙЛА ★ */
if (ext && strcasecmp(ext, ".sf2") == 0) {
    char sf2_path[256];
    snprintf(sf2_path, sizeof(sf2_path), "%s%s", cur_path, sel_fn);
    
    if (strncmp(sf2_path, "A:/", 3) == 0) {
        strncpy(g_selected_sf2_path, "/sdcard", sizeof(g_selected_sf2_path));
        strncat(g_selected_sf2_path, sf2_path + 2, sizeof(g_selected_sf2_path) - strlen(g_selected_sf2_path) - 1);
    } else {
        strncpy(g_selected_sf2_path, sf2_path, sizeof(g_selected_sf2_path) - 1);
        g_selected_sf2_path[sizeof(g_selected_sf2_path) - 1] = '\0';
    }
    
    ESP_LOGI(TAG, "SF2 selected: %s", g_selected_sf2_path);
    
    /* ★ ПРОСТОЕ СООБЩЕНИЕ БЕЗ ГАЛОЧКИ ★ */
    lv_obj_t *scr = lv_screen_active();
    
    // Контейнер
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A2E), 0);      // Тёмный фон
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x00FF00), 0);   // Зелёная рамка
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_width(box, 150);
    lv_obj_center(box);
    
    // Заголовок "SF2 Selected" (зелёный)
    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "SF2 Selected");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);   // ★ ЗЕЛЁНЫЙ ★
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);    // ★ ШРИФТ 10 ★
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 10);
    
    // Имя файла (серый/белый)
    const char *fname = strrchr(sel_fn, '/');
    if (fname) fname++; else fname = sel_fn;
    
    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text_fmt(label, "%s", fname);
    lv_obj_set_style_text_color(label, lv_color_hex(0xCCCCCC), 0);   // ★ СВЕТЛО-СЕРЫЙ ★
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);    // ★ ШРИФТ 10 ★
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    
    // Автоудаление через 2 секунды
    lv_timer_t *timer = lv_timer_create(close_msgbox_timer_cb, 2000, box);
    lv_timer_set_repeat_count(timer, 1);
    
    return;
}
        
        /* ★ ОБРАБОТКА USB-MIDI ★ */
        if (strcmp(sel_fn, "MidiSynth.usb") == 0) {
            /* Проверяем, выбран ли SF2 */
            if (strlen(g_selected_sf2_path) == 0) {
                lv_obj_t *msg = lv_msgbox_create(NULL);
                lv_msgbox_add_title(msg, "Error");
                lv_msgbox_add_text(msg, "Please select SF2 file first!");
                lv_obj_center(msg);
                return;
            }
            
            ESP_LOGI(TAG, "Starting USB-MIDI Synth with SF2: %s", g_selected_sf2_path);
            
            /* Создаём экран плеера если его нет */
            if (player_screen == NULL) {
                player_screen = player_ui_create();
                input_set_player_screen(player_screen);
            }
            
            /* Переключаемся на экран плеера */
            lv_scr_load(player_screen);
            input_set_mode(INPUT_MODE_PLAYER);
            
            /* Запускаем через audio_player_play */
            char full_path[256];
            snprintf(full_path, sizeof(full_path), "%s%s", cur_path, sel_fn);
            audio_player_play(full_path);
            
            return;
        }
        
        /* ★ ОБЫЧНАЯ ОБРАБОТКА МУЗЫКАЛЬНЫХ ФАЙЛОВ ★ */
        if (is_tracker_file(sel_fn)) {
            char full_path[256];
            snprintf(full_path, sizeof(full_path), "%s%s", cur_path, sel_fn);
            ESP_LOGI(TAG, "Playing: %s", full_path);

            if (player_screen == NULL) {
                player_screen = player_ui_create();
                input_set_player_screen(player_screen);
            }
            lv_scr_load(player_screen);
            input_set_mode(INPUT_MODE_PLAYER);
            audio_player_play(full_path);
        }
    }
}

static void lvgl_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
        taskYIELD();

        if (input_get_mode() == INPUT_MODE_PLAYER) {
            audio_player_process_ui();
        }

        /* 1. ОТКРЫТИЕ НАСТРОЕК */
        if (open_settings_flag) {
            open_settings_flag = false;
            
            if (input_get_mode() == INPUT_MODE_PLAYER && !input_get_in_settings()) {
                player_mode_t mode = audio_player_get_current_mode();
                
                /* Если режим NONE - не открываем настройки */
                if (mode == PLAYER_MODE_NONE) {
                    ESP_LOGI(TAG, "No player active, ignoring settings request");
                    open_settings_flag = false;
                } else {
                    /* Удаляем старые настройки, если режим изменился */
                    static player_mode_t last_settings_mode = PLAYER_MODE_NONE;
                    
                    if (settings_screen != NULL && last_settings_mode != mode) {
                        ESP_LOGI(TAG, "Mode changed, recreating settings screen");
                        lv_obj_del(settings_screen);
                        settings_screen = NULL;
                    }
                    
                    if (settings_screen == NULL) {
                        settings_screen = settings_ui_create(mode);
                        last_settings_mode = mode;
                    }
                    
                    settings_ui_set_player_screen(player_screen);
                    lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
                    input_set_in_settings(true);
                    
                    /* Убираем файловый менеджер из группы */
                    lv_group_t *group = lv_group_get_default();
                    if (group && main_screen) {
                        lv_obj_t *explorer = lv_obj_get_child(main_screen, 0);
                        if (explorer) {
                            lv_obj_t *ft = lv_file_explorer_get_file_table(explorer);
                            if (ft) lv_group_remove_obj(ft);
                        }
                    }
                    
                    /* ★ СБРАСЫВАЕМ ФОКУС ПРИ ОТКРЫТИИ НАСТРОЕК ★ */
                    if (group) {
                        lv_group_focus_obj(NULL);
                        lv_group_set_editing(group, false);
                    }
                    
                    settings_ui_reset_focus();
                    ESP_LOGI(TAG, "Settings opened for mode: %d", mode);
                }
            }
        }

        /* 2. ОБРАБОТКА ВЫХОДА ИЗ НАСТРОЕК В ПЛЕЕР */
        extern volatile bool exit_settings_flag;
        if (exit_settings_flag) {
            exit_settings_flag = false;
            
            /* ★ СБРАСЫВАЕМ ФОКУС ПРИ ВЫХОДЕ ★ */
            lv_group_t *group = lv_group_get_default();
            if (group) {
                lv_group_focus_obj(NULL);
                lv_group_set_editing(group, false);
            }
            
            if (player_screen != NULL) {
                lv_scr_load(player_screen);
                input_set_in_settings(false);
                input_set_mode(INPUT_MODE_PLAYER);
                ESP_LOGI(TAG, "Exited settings to Player, focus cleared");
            }
        }

        /* 3. ВОЗВРАТ В FILE EXPLORER (если был остановлен плеер) */
        if (player_stopped_for_explorer) {
            player_stopped_for_explorer = false;
            input_set_in_settings(false);
            audio_player_clear_ui_queue();
            
            if (main_screen) {
                lv_scr_load_anim(main_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
                
                lv_group_t *group = lv_group_get_default();
                if (group) {
                    lv_obj_t *explorer = lv_obj_get_child(main_screen, 0);
                    if (explorer) {
                        lv_obj_t *ft = lv_file_explorer_get_file_table(explorer);
                        if (ft) {
                            lv_group_add_obj(group, ft);
                            lv_group_focus_obj(ft);
                            lv_group_set_editing(group, false);
                        }
                    }
                }
            }
            input_set_mode(INPUT_MODE_FILE_EXPLORER);
            ESP_LOGI(TAG, "Returned to File Explorer");
        }
    }
}

void app_main(void)
{
    lv_display_t *disp = lvgl_port_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL port init failed");
        return;
    }

    main_screen = lv_screen_active();
    input_init();
    audio_player_init();

    if (sd_card_init() != ESP_OK) {
        ESP_LOGE(TAG, "SD Card init failed");
        lv_obj_t *label = lv_label_create(main_screen);
        lv_label_set_text(label, "SD Card Error!");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_t *explorer = lv_file_explorer_create(main_screen);
        lv_obj_add_flag(lv_file_explorer_get_quick_access_area(explorer), LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(explorer);
        file_explorer_style_setup(explorer);
        lv_file_explorer_set_sort(explorer, LV_EXPLORER_SORT_KIND);
        lv_file_explorer_open_dir(explorer, "A:/");

        path_label = lv_label_create(main_screen);
        lv_obj_set_style_text_font(path_label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(path_label, lv_color_hex(0x00AA00), 0);
        lv_obj_set_style_bg_color(path_label, lv_color_hex(0x0A0A14), 0);
        lv_obj_set_style_pad_all(path_label, 2, 0);
        lv_obj_set_width(path_label, LV_PCT(100));
        lv_obj_set_scrollbar_mode(path_label, LV_SCROLLBAR_MODE_OFF);
        lv_label_set_long_mode(path_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_label_set_text(path_label, "A:/");
        lv_obj_align(path_label, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_update_layout(path_label);
        
        lv_timer_create(path_update_timer_cb, 200, explorer);
        lv_obj_add_event_cb(explorer, file_explorer_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
        
        /* ★ ИСПРАВЛЕНИЕ: Добавляем в группу именно file_table, а не весь контейнер explorer ★ */
        lv_group_t *group = lv_group_get_default();
        if (group) {
            lv_obj_t *ft = lv_file_explorer_get_file_table(explorer);
            if (ft) {
                lv_group_add_obj(group, ft);
                lv_group_focus_obj(ft);
            }
        }
    }

    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 24576, NULL, 5, NULL, 0);
    
    // Пустой цикл, вся работа идет в lvgl_task
    while (1) { 
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}