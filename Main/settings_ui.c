#include "settings_ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include "audio_player.h"
#include "input.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *TAG = "SETTINGS_UI";

static lv_obj_t *menu_screen = NULL;
static lv_obj_t *menu = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *player_screen_ref = NULL;

/* MP3 EQ */
#define MP3_PRESET_COUNT 7
static lv_obj_t *mp3_preset_switches[MP3_PRESET_COUNT];
static const char *mp3_preset_names[MP3_PRESET_COUNT] = {
    "Flat", "Bass Boost", "Rock", "Pop", "Jazz", "Classical", "Talk"
};

/* GME настройки */
static gme_settings_t current_gme_settings;
static lv_obj_t *gme_treble_slider = NULL;
static lv_obj_t *gme_bass_slider = NULL;
static lv_obj_t *gme_stereo_slider = NULL;
static lv_obj_t *gme_accuracy_sw = NULL;
static lv_obj_t *gme_mute_switches[8] = {NULL}; // Массив для Mute Voices

/* Структура для выбора SF2 */
typedef struct {
    lv_obj_t *list;
    lv_obj_t *parent_page;
    char folder_path[256];
    void (*on_select)(const char *path);
} sf2_selector_t;

static sf2_selector_t sf2_selector;

/* ==================== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==================== */

static void sf2_item_delete_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    char *path = (char *)lv_obj_get_user_data(obj);
    if (path) {
        free(path);
        lv_obj_set_user_data(obj, NULL);
    }
}

static void sf2_item_click_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const char *path = (const char *)lv_obj_get_user_data(obj);
    if (path && sf2_selector.on_select) {
        sf2_selector.on_select(path);
        
        player_mode_t mode = audio_player_get_current_mode();
        if (mode == PLAYER_MODE_USB_MIDI) {
            player_cmd_t cmd = { .type = PLAYER_CMD_RELOAD_SF2, .value = 0 };
            xQueueSend(player_cmd_queue, &cmd, 0);
        }
        
        if (menu && main_page) {
            lv_menu_set_page(menu, main_page);
        }
        
        lv_group_t *group = lv_group_get_default();
        if (group) {
            lv_group_set_editing(group, false);
            lv_group_focus_next(group);
        }
    }
}

static void sf2_item_focused_cb(lv_event_t *e) {
    lv_obj_t *item = lv_event_get_target(e);
    lv_obj_scroll_to_view_recursive(item, LV_ANIM_ON);
}

static void add_obj_to_group(lv_group_t *group, lv_obj_t *obj) {
    if (!group || !obj) return;
    lv_group_add_obj(group, obj);
}

static lv_obj_t *create_compact_menu_item(lv_obj_t *parent, const char *label_text) {
    lv_obj_t *cont = lv_menu_cont_create(parent);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 3, 0);
    lv_obj_set_style_min_height(cont, 22, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    
    lv_obj_t *label = lv_label_create(cont);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(label, 1);
    return cont;
}

static lv_obj_t *create_compact_labeled_slider(lv_obj_t *parent, const char *label_text, 
                                                int32_t min, int32_t max, int32_t value,
                                                lv_obj_t **slider_out, lv_obj_t **value_label_out) {
    lv_obj_t *cont = lv_menu_cont_create(parent);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_style_min_height(cont, 32, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    
    lv_obj_t *header = lv_obj_create(cont);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_t *label = lv_label_create(header);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    
    lv_obj_t *value_label = lv_label_create(header);
    lv_label_set_text_fmt(value_label, "%ld", (long)value);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_10, 0);
    
    lv_obj_t *slider = lv_slider_create(cont);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_size(slider, LV_PCT(100), 12);
    lv_obj_set_style_radius(slider, 4, 0);
    
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x00AA00), LV_PART_KNOB);
    
    if (slider_out) *slider_out = slider;
    if (value_label_out) *value_label_out = value_label;
    return cont;
}

static void close_settings_cb(lv_event_t *e) {
    extern volatile bool exit_settings_flag;
    exit_settings_flag = true;
    
    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_set_editing(group, false);
    }
}

void settings_ui_handle_esc(void) {
    if (!menu) return;
    
    lv_obj_t *current_page = lv_menu_get_cur_main_page(menu);
    
    if (current_page == main_page) {
        close_settings_cb(NULL);
    } else {
        lv_obj_t *back_btn = lv_menu_get_main_header_back_button(menu);
        if (back_btn) {
            lv_obj_send_event(back_btn, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void settings_key_cb(lv_event_t *e) {
    if (lv_event_get_key(e) == LV_KEY_ESC) {
        settings_ui_handle_esc();
        lv_event_stop_processing(e);
    }
}

/* ==================== SF2 ВЫБОР ==================== */

static lv_obj_t *create_sf2_selector_page(lv_obj_t *menu, const char *folder_path, 
                                           const char *title, void (*on_select)(const char *path))
{
    lv_obj_t *page = lv_menu_page_create(menu, title);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    
    strncpy(sf2_selector.folder_path, folder_path, sizeof(sf2_selector.folder_path) - 1);
    sf2_selector.on_select = on_select;
    sf2_selector.parent_page = page;
    
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    
    DIR *dir = opendir(folder_path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open folder: %s", folder_path);
        lv_obj_t *label = lv_label_create(list);
        lv_label_set_text(label, "No SF2 files found");
        lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        return page;
    }
    
    lv_group_t *group = lv_group_get_default();
    struct dirent *entry;
    int file_count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcasecmp(ext, ".sf2") != 0) continue;
        
        lv_obj_t *item = lv_obj_create(list);
        lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 4, 0);
        lv_obj_set_style_radius(item, 0, 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x004400), LV_STATE_FOCUSED);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, entry->d_name);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x00CC00), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), LV_STATE_FOCUSED);
        lv_obj_set_flex_grow(label, 1);
        
        lv_obj_t *arrow = lv_label_create(item);
        lv_label_set_text(arrow, LV_SYMBOL_NEXT);
        lv_obj_set_style_text_font(arrow, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0x00AA00), 0);
        
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s%s", folder_path, entry->d_name);
        lv_obj_set_user_data(item, strdup(full_path));
        
        lv_obj_add_event_cb(item, sf2_item_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(item, sf2_item_focused_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(item, sf2_item_delete_cb, LV_EVENT_DELETE, NULL);
        
        if (group) lv_group_add_obj(group, item);
        file_count++;
    }
    
    closedir(dir);
    
    if (file_count == 0) {
        lv_obj_t *label = lv_label_create(list);
        lv_label_set_text(label, "No SF2 files found");
        lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    }
    
    if (group && file_count > 0) {
        lv_group_focus_next(group);
    }
    
    return page;
}

/* ==================== MP3 НАСТРОЙКИ ==================== */

static void mp3_preset_changed_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target_obj(e);
    if (!lv_obj_has_state(sw, LV_STATE_CHECKED)) return;
    int idx = -1;
    for (int i = 0; i < MP3_PRESET_COUNT; i++) {
        if (mp3_preset_switches[i] == sw) { idx = i; break; }
    }
    if (idx < 0) return;
    for (int i = 0; i < MP3_PRESET_COUNT; i++) {
        if (i != idx) lv_obj_clear_state(mp3_preset_switches[i], LV_STATE_CHECKED);
    }
    audio_player_set_eq((eq_preset_t)idx);
}

static lv_obj_t *create_mp3_settings(lv_obj_t *menu, lv_group_t *group) {
    lv_obj_t *page = lv_menu_page_create(menu, "MP3 EQ");
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    
    lv_obj_t *cont = lv_menu_cont_create(page);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    
    eq_preset_t current = audio_player_get_eq();
    for (int i = 0; i < MP3_PRESET_COUNT; i++) {
        lv_obj_t *item = create_compact_menu_item(cont, mp3_preset_names[i]);
        lv_obj_t *sw = lv_switch_create(item);
        lv_obj_set_size(sw, 35, 16);
        lv_obj_set_style_radius(sw, 8, 0);
        if (i == (int)current) lv_obj_add_state(sw, LV_STATE_CHECKED);
        
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x00FF00), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x00AA00), LV_PART_KNOB);
        
        lv_obj_add_event_cb(sw, mp3_preset_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        mp3_preset_switches[i] = sw;
        
        if (group) add_obj_to_group(group, sw);
    }
    
    return page;
}

/* ==================== AUDIO (FLAC/WAV/OPUS) НАСТРОЙКИ ==================== */

static lv_obj_t *create_audio_settings(lv_obj_t *menu, lv_group_t *group, const char *title) {
    lv_obj_t *page = lv_menu_page_create(menu, title);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    
    lv_obj_t *cont = lv_menu_cont_create(page);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    
    lv_obj_t *item = lv_menu_cont_create(cont);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(item, 3, 0);
    lv_obj_set_style_min_height(item, 22, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x000000), 0);
    
    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, "Volume");
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(label, 1);
    
    lv_obj_t *vol_label = lv_label_create(item);
    int vol = audio_player_get_volume();
    lv_label_set_text_fmt(vol_label, "%d%%", vol);
    lv_obj_set_style_text_color(vol_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(vol_label, &lv_font_montserrat_10, 0);
    
    lv_obj_t *info = lv_menu_cont_create(cont);
    lv_obj_set_style_pad_all(info, 4, 0);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x000000), 0);
    lv_obj_t *info_label = lv_label_create(info);
    lv_label_set_text(info_label, "Use Left/Right to adjust");
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_10, 0);
    
    return page;
}

/* ==================== GME НАСТРОЙКИ ==================== */

static void gme_treble_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target_obj(e);
    lv_obj_t *value_label = (lv_obj_t *)lv_event_get_user_data(e);
    int32_t val = lv_slider_get_value(slider);
    current_gme_settings.treble = (double)val / 10.0;
    if (value_label) lv_label_set_text_fmt(value_label, "%.1f", current_gme_settings.treble);
    audio_player_set_gme_settings(&current_gme_settings);
}

static void gme_bass_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target_obj(e);
    lv_obj_t *value_label = (lv_obj_t *)lv_event_get_user_data(e);
    int32_t val = lv_slider_get_value(slider);
    int32_t real_bass = val * 10;
    current_gme_settings.bass = (double)real_bass;
    if (value_label) lv_label_set_text_fmt(value_label, "%ld", (long)real_bass);
    audio_player_set_gme_settings(&current_gme_settings);
}

static void gme_stereo_changed_cb(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target_obj(e);
    lv_obj_t *value_label = (lv_obj_t *)lv_event_get_user_data(e);
    int32_t val = lv_slider_get_value(slider);
    int32_t real_percent = val * 10;
    current_gme_settings.stereo_depth = (double)real_percent / 100.0;
    if (value_label) lv_label_set_text_fmt(value_label, "%ld%%", (long)real_percent);
    audio_player_set_gme_settings(&current_gme_settings);
}

static void gme_accuracy_changed_cb(lv_event_t *e) {
    current_gme_settings.accuracy = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    audio_player_set_gme_settings(&current_gme_settings);
}

static void gme_mute_changed_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target_obj(e);
    int voice_idx = (int)(intptr_t)lv_obj_get_user_data(sw);
    current_gme_settings.mute_voices[voice_idx] = lv_obj_has_state(sw, LV_STATE_CHECKED);
    audio_player_set_gme_settings(&current_gme_settings);
}

static lv_obj_t *create_gme_settings(lv_obj_t *menu, lv_group_t *group) {
    lv_obj_t *page = lv_menu_page_create(menu, "GME Settings");
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    
    current_gme_settings = audio_player_get_gme_settings();
    lv_obj_t *cont = lv_menu_cont_create(page);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    
    /* Treble */
    lv_obj_t *treble_value = NULL;
    create_compact_labeled_slider(cont, "Treble (dB)", -500, 50, 
                          (int32_t)(current_gme_settings.treble * 10),
                          &gme_treble_slider, &treble_value);
    if (gme_treble_slider) {
        lv_obj_add_event_cb(gme_treble_slider, gme_treble_changed_cb, LV_EVENT_VALUE_CHANGED, treble_value);
        if (group) add_obj_to_group(group, gme_treble_slider);
    }
    
    /* Bass */
    lv_obj_t *bass_value = NULL;
    lv_obj_t *bass_slider = NULL;
    create_compact_labeled_slider(cont, "Bass (Hz)", 0, 1600, 
                          (int32_t)(current_gme_settings.bass / 10),
                          &bass_slider, &bass_value);
    gme_bass_slider = bass_slider;
    if (gme_bass_slider) {
        lv_obj_add_event_cb(gme_bass_slider, gme_bass_changed_cb, LV_EVENT_VALUE_CHANGED, bass_value);
        if (group) add_obj_to_group(group, gme_bass_slider);
    }
    
    /* Stereo */
    lv_obj_t *stereo_value = NULL;
    lv_obj_t *stereo_slider = NULL;
    create_compact_labeled_slider(cont, "Stereo (%)", 0, 10, 
                          (int32_t)(current_gme_settings.stereo_depth * 10),
                          &stereo_slider, &stereo_value);
    gme_stereo_slider = stereo_slider;
    if (gme_stereo_slider) {
        lv_obj_add_event_cb(gme_stereo_slider, gme_stereo_changed_cb, LV_EVENT_VALUE_CHANGED, stereo_value);
        if (group) add_obj_to_group(group, gme_stereo_slider);
    }
    
    /* Разделитель */
    lv_obj_t *sep = lv_obj_create(cont);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);
    lv_obj_set_style_min_height(sep, 1, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Accuracy */
    lv_obj_t *acc_item = create_compact_menu_item(cont, "Accurate Emulation");
    gme_accuracy_sw = lv_switch_create(acc_item);
    lv_obj_set_size(gme_accuracy_sw, 35, 16);
    lv_obj_set_style_radius(gme_accuracy_sw, 8, 0);
    lv_obj_set_style_bg_color(gme_accuracy_sw, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(gme_accuracy_sw, lv_color_hex(0x00FF00), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(gme_accuracy_sw, lv_color_hex(0x00AA00), LV_PART_KNOB);
    if (current_gme_settings.accuracy) lv_obj_add_state(gme_accuracy_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(gme_accuracy_sw, gme_accuracy_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (group) add_obj_to_group(group, gme_accuracy_sw);
    
    /* Разделитель */
    lv_obj_t *sep2 = lv_obj_create(cont);
    lv_obj_set_size(sep2, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep2, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(sep2, 0, 0);
    lv_obj_set_style_radius(sep2, 0, 0);
    lv_obj_set_style_pad_all(sep2, 0, 0);
    lv_obj_set_style_min_height(sep2, 1, 0);
    lv_obj_clear_flag(sep2, LV_OBJ_FLAG_SCROLLABLE);
    
    /* Mute Voices */
    lv_obj_t *mute_header = lv_menu_cont_create(cont);
    lv_obj_set_flex_flow(mute_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(mute_header, 2, 0);
    lv_obj_set_style_bg_color(mute_header, lv_color_hex(0x000000), 0);
    
    lv_obj_t *mute_label = lv_label_create(mute_header);
    lv_label_set_text(mute_label, "Mute Voices");
    lv_obj_set_style_text_color(mute_label, lv_color_hex(0x00AAFF), 0);
    lv_obj_set_style_text_font(mute_label, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(mute_label, 1);
    
    for (int i = 0; i < 8; i++) {
        lv_obj_t *item = lv_menu_cont_create(cont);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(item, 1, 0);
        lv_obj_set_style_min_height(item, 20, 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x000000), 0);
        
        lv_obj_t *voice_label = lv_label_create(item);
        lv_label_set_text_fmt(voice_label, "Voice %d", i);
        lv_obj_set_style_text_font(voice_label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(voice_label, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_flex_grow(voice_label, 1);
        
        lv_obj_t *sw = lv_switch_create(item);
        lv_obj_set_size(sw, 35, 14);
        lv_obj_set_style_radius(sw, 7, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x00FF00), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x00AA00), LV_PART_KNOB);
        if (current_gme_settings.mute_voices[i]) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        
        lv_obj_set_user_data(sw, (void *)(intptr_t)i);
        lv_obj_add_event_cb(sw, gme_mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        gme_mute_switches[i] = sw;
        if (group) add_obj_to_group(group, sw);
    }
    
    return page;
}

/* ==================== MIDI НАСТРОЙКИ ==================== */

static lv_obj_t *create_midi_settings(lv_obj_t *menu, lv_group_t *group, const char *title, 
                                       const char *sf2_folder, const char *current_sf2_path,
                                       void (*set_sf2_cb)(const char *path),
                                       const char *selector_title)
{
    lv_obj_t *page = lv_menu_page_create(menu, title);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    
    lv_obj_t *cont = lv_menu_cont_create(page);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    
    lv_obj_t *sf2_item = lv_menu_cont_create(cont);
    lv_obj_set_flex_flow(sf2_item, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(sf2_item, 3, 0);
    lv_obj_set_style_min_height(sf2_item, 22, 0);
    lv_obj_set_style_bg_color(sf2_item, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(sf2_item, LV_OBJ_FLAG_CLICKABLE);
    
    lv_obj_t *sf2_icon = lv_label_create(sf2_item);
    lv_label_set_text(sf2_icon, LV_SYMBOL_FILE);
    lv_obj_set_style_text_font(sf2_icon, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(sf2_icon, lv_color_hex(0x00FF00), 0);
    
    lv_obj_t *sf2_label = lv_label_create(sf2_item);
    lv_label_set_text(sf2_label, "Select SF2");
    lv_obj_set_style_text_color(sf2_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(sf2_label, &lv_font_montserrat_10, 0);
    lv_obj_set_flex_grow(sf2_label, 1);
    
    lv_obj_t *sf2_selector_page = create_sf2_selector_page(menu, sf2_folder, 
                                                             selector_title, set_sf2_cb);
    lv_menu_set_load_page_event(menu, sf2_item, sf2_selector_page);
    
    if (group) add_obj_to_group(group, sf2_item);
    
    lv_obj_t *info_item = lv_menu_cont_create(cont);
    lv_obj_set_style_pad_all(info_item, 3, 0);
    lv_obj_set_style_bg_color(info_item, lv_color_hex(0x000000), 0);
    lv_obj_t *info_label = lv_label_create(info_item);
    
    const char *fname = strrchr(current_sf2_path, '/');
    if (fname) fname++; else fname = current_sf2_path;
    lv_label_set_text_fmt(info_label, "Current: %s", fname);
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_10, 0);
    
    return page;
}

/* ==================== ГЛАВНАЯ ФУНКЦИЯ ==================== */

lv_obj_t *settings_ui_create(player_mode_t mode)
{
    ESP_LOGI(TAG, "Creating settings for mode: %d", mode);
    
    if (menu_screen != NULL) {
        lv_obj_del(menu_screen);
        menu_screen = NULL;
        menu = NULL;
        main_page = NULL;
    }
    
    menu_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(menu_screen, lv_color_hex(0x000000), 0);
    
    menu = lv_menu_create(menu_screen);
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_center(menu);
    lv_obj_set_style_bg_color(menu, lv_color_hex(0x000000), 0);
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_DISABLED);
    
    lv_obj_t *main_header = lv_menu_get_main_header(menu);
    if (main_header) {
        lv_obj_set_style_height(main_header, 18, 0);
        lv_obj_set_style_pad_all(main_header, 2, 0);
        lv_obj_set_style_bg_color(main_header, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_color(main_header, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(main_header, &lv_font_montserrat_10, 0);
    }
    
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *settings_page = NULL;
    
    switch (mode) {
        case PLAYER_MODE_MP3:
            settings_page = create_mp3_settings(menu, group);
            break;
        case PLAYER_MODE_FLAC:
        case PLAYER_MODE_WAV:
        case PLAYER_MODE_OPUS:
        case PLAYER_MODE_XMP:
            settings_page = create_audio_settings(menu, group, "Audio Settings");
            break;
        case PLAYER_MODE_GME:
            settings_page = create_gme_settings(menu, group);
            break;
        case PLAYER_MODE_MIDI:
            settings_page = create_midi_settings(menu, group, "MIDI Settings",
                                         audio_player_get_midi_sf2_folder(),
                                         audio_player_get_midi_sf2_path(),
                                         audio_player_set_midi_sf2_path,
                                         "Select MIDI SF2");
            break;
        case PLAYER_MODE_USB_MIDI:
            settings_page = create_midi_settings(menu, group, "USB-MIDI Settings",
                                         audio_player_get_usb_synth_sf2_folder(),
                                         audio_player_get_usb_synth_sf2_path(),
                                         audio_player_set_usb_synth_sf2_path,
                                         "Select USB-Synth SF2");
            break;
        default:
            ESP_LOGE(TAG, "Unknown mode: %d", mode);
            return menu_screen;
    }
    
    if (settings_page) {
        lv_menu_set_page(menu, settings_page);
        
        if (group) {
            lv_group_set_editing(group, false);
            lv_group_focus_next(group);
        }
    }
    
    main_page = settings_page;
    
    lv_obj_add_event_cb(menu, settings_key_cb, LV_EVENT_KEY, NULL);
    
    ESP_LOGI(TAG, "Settings created for mode: %d", mode);
    return menu_screen;
}

void settings_ui_set_player_screen(lv_obj_t *screen) {
    player_screen_ref = screen;
}

void settings_ui_reset_focus(void) {
    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_set_editing(group, false);
        lv_group_focus_next(group);
    }
}