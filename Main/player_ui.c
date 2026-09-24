#include <stdio.h>
#include "player_ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "PLAYER_UI";

static lv_obj_t *screen_player;
static lv_obj_t *label_title;
static lv_obj_t *label_tracker;
static lv_obj_t *label_pattern;
static lv_obj_t *label_time;
static lv_obj_t *bar_progress;
static lv_obj_t *label_mode;
static lv_obj_t *label_loading;
static lv_obj_t *spinner_loading;
static lv_obj_t *label_format;
static lv_obj_t *label_volume;
static lv_obj_t *label_ram;      /* ← ДОБАВИТЬ */
static lv_obj_t *label_module_info1;
static lv_obj_t *label_module_info2;
static lv_obj_t *label_module_info3;
static lv_obj_t *label_gme_game;    // Название игры
static lv_obj_t *label_gme_author;  // Автор/композитор
static lv_obj_t *label_gme_system;  // Игровая система
static char title_buf[64];
static char pattern_buf[32];
static char time_buf[32];
static char mode_buf[16];
static int last_volume = -1;

lv_obj_t *player_ui_create(void)
{
    screen_player = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_player, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_pad_all(screen_player, 4, 0);

    /* === НАЗВАНИЕ ТРЕКА (верх, крупно) === */
    label_title = lv_label_create(screen_player);
    lv_label_set_text(label_title, "No Track");
    lv_label_set_long_mode(label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FF00), 0);
    lv_obj_set_width(label_title, 152);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 0);

    /* === АРТИСТ/ТРЕКЕР (под заголовком) === */
    label_tracker = lv_label_create(screen_player);
    lv_label_set_text(label_tracker, "Unknown Artist");
    lv_obj_set_style_text_font(label_tracker, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_tracker, lv_color_hex(0x888888), 0);
    lv_obj_align(label_tracker, LV_ALIGN_TOP_MID, 0, 16);

    /* === ИНФО МОДУЛЯ (3 строки слева) === */
    label_module_info1 = lv_label_create(screen_player);
    lv_label_set_text(label_module_info1, "");
    lv_obj_set_style_text_font(label_module_info1, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_module_info1, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(label_module_info1, LV_ALIGN_TOP_LEFT, 0, 30);

    label_module_info2 = lv_label_create(screen_player);
    lv_label_set_text(label_module_info2, "");
    lv_obj_set_style_text_font(label_module_info2, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_module_info2, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(label_module_info2, LV_ALIGN_TOP_LEFT, 0, 44);

    label_module_info3 = lv_label_create(screen_player);
    lv_label_set_text(label_module_info3, "");
    lv_obj_set_style_text_font(label_module_info3, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_module_info3, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(label_module_info3, LV_ALIGN_TOP_LEFT, 0, 58);

    /* === ПАТТЕРН (справа вверху) === */
    label_pattern = lv_label_create(screen_player);
    lv_label_set_text(label_pattern, "---/---");
    lv_obj_set_style_text_font(label_pattern, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_pattern, lv_color_hex(0x00AAAA), 0);
    lv_obj_align(label_pattern, LV_ALIGN_TOP_RIGHT, -2, 30);

    /* === ФОРМАТ (справа под паттерном) === */
    label_format = lv_label_create(screen_player);
    lv_label_set_text(label_format, "");
    lv_obj_set_style_text_font(label_format, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_format, lv_color_hex(0x666666), 0);
    lv_obj_align(label_format, LV_ALIGN_TOP_RIGHT, -2, 44);

    /* === ПРОГРЕСС-БАР (поднят на 7 пикселей) === */
    bar_progress = lv_bar_create(screen_player);
    lv_obj_set_size(bar_progress, 152, 6);
    lv_obj_align(bar_progress, LV_ALIGN_BOTTOM_MID, 0, -31);  // Было -24, стало -31
    lv_obj_set_style_radius(bar_progress, 3, 0);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x33FFFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 3, LV_PART_INDICATOR);
    lv_bar_set_range(bar_progress, 0, 1000);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);

    /* === ВРЕМЯ (под прогрессом) === */
    label_time = lv_label_create(screen_player);
    lv_label_set_text(label_time, "00:00 / 00:00");
    lv_obj_set_style_text_font(label_time, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_time, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(label_time, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* === РЕЖИМ ПОВТОРА (внизу слева) === */
    label_mode = lv_label_create(screen_player);
    lv_label_set_text(label_mode, "ALL");
    lv_obj_set_style_text_font(label_mode, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_mode, lv_color_hex(0x00AAAA), 0);
    lv_obj_align(label_mode, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    /* === ГРОМКОСТЬ (над режимом) === */
    label_volume = lv_label_create(screen_player);
    lv_label_set_text(label_volume, "VOL:80");
    lv_obj_set_style_text_font(label_volume, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_volume, lv_color_hex(0x666666), 0);
    lv_obj_align_to(label_volume, label_mode, LV_ALIGN_OUT_TOP_LEFT, 0, -2);

    /* === RAM (на одной линии с громкостью, сдвинута влево) === */
    label_ram = lv_label_create(screen_player);
    lv_label_set_text(label_ram, "RAM:OK");
    lv_obj_set_style_text_font(label_ram, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_ram, lv_color_hex(0x00AA00), 0);
    lv_obj_align_to(label_ram, label_volume, LV_ALIGN_OUT_RIGHT_MID, 50, 0);  // Сдвинул влево на 50px

    /* === ИНДИКАТОРЫ ЗАГРУЗКИ === */
    label_loading = lv_label_create(screen_player);
    lv_label_set_text(label_loading, "Loading...");
    lv_obj_set_style_text_font(label_loading, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_loading, lv_color_hex(0x00FF00), 0);
    lv_obj_align(label_loading, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(label_loading, LV_OBJ_FLAG_HIDDEN);

    spinner_loading = lv_spinner_create(screen_player);
    lv_spinner_set_anim_params(spinner_loading, 3000, 20);
    lv_obj_set_size(spinner_loading, 40, 40);
    lv_obj_align(spinner_loading, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(spinner_loading, LV_OBJ_FLAG_HIDDEN);

        /* === ИНФОПАНЕЛЬ ДЛЯ GME (скрыта по умолчанию) === */
    label_gme_game = lv_label_create(screen_player);
    lv_label_set_text(label_gme_game, "");
    lv_obj_set_style_text_font(label_gme_game, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_gme_game, lv_color_hex(0x00AAFF), 0);
    lv_obj_align(label_gme_game, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_add_flag(label_gme_game, LV_OBJ_FLAG_HIDDEN);
    
    label_gme_author = lv_label_create(screen_player);
    lv_label_set_text(label_gme_author, "");
    lv_obj_set_style_text_font(label_gme_author, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_gme_author, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(label_gme_author, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_add_flag(label_gme_author, LV_OBJ_FLAG_HIDDEN);
    
    label_gme_system = lv_label_create(screen_player);
    lv_label_set_text(label_gme_system, "");
    lv_obj_set_style_text_font(label_gme_system, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label_gme_system, lv_color_hex(0x66FF66), 0);
    lv_obj_align(label_gme_system, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_add_flag(label_gme_system, LV_OBJ_FLAG_HIDDEN);


    ESP_LOGI(TAG, "Player UI created");
    return screen_player;
}

void player_ui_set_title(const char *title)
{
    if (!screen_player || !label_title || !title) return;
    ESP_LOGI("PLAYER_UI", "set_title: %s", title);
    snprintf(title_buf, sizeof(title_buf), "%s", title);
    lv_label_set_text(label_title, title_buf);
}

void player_ui_update(const char *title, int pattern, int total_patterns,
                      int row, uint32_t time_ms, uint32_t total_time_ms)
{
    if (!screen_player) return;

        /* Скрываем паттерн для аудио форматов (pattern=0 и total=0) */
    if (pattern == 0 && total_patterns == 0) {
        lv_obj_add_flag(label_pattern, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(label_pattern, LV_OBJ_FLAG_HIDDEN);
    }

    /* Обновляем название только если изменилось */
    if (title && title[0] != '\0') {
        if (strcmp(title_buf, title) != 0) {
            snprintf(title_buf, sizeof(title_buf), "%s", title);
            lv_label_set_text(label_title, title_buf);
        }
    }

    /* Обновляем паттерн только если изменился */
    static int last_pattern = -1, last_total = -1;
    if (pattern != last_pattern || total_patterns != last_total) {
        last_pattern = pattern;
        last_total = total_patterns;
        snprintf(pattern_buf, sizeof(pattern_buf), "%02d/%02d", pattern, total_patterns);
        lv_label_set_text(label_pattern, pattern_buf);
    }

    /* Обновляем время только если изменилось */
    static char last_time_str[32] = "";
    uint32_t sec = time_ms / 1000;
    uint32_t total_sec = total_time_ms / 1000;
    char new_time[32];
    snprintf(new_time, sizeof(new_time), "%02lu:%02lu / %02lu:%02lu",
             sec / 60, sec % 60, total_sec / 60, total_sec % 60);
    if (strcmp(last_time_str, new_time) != 0) {
        strcpy(last_time_str, new_time);
        lv_label_set_text(label_time, new_time);
    }

    /* Обновляем прогресс-бар только если изменился */
    if (total_time_ms > 0) {
        static int32_t last_progress = -1;
        int32_t progress = (int32_t)((time_ms * 1000) / total_time_ms);
        if (progress > 1000) progress = 1000;
        if (progress != last_progress) {
            last_progress = progress;
            lv_bar_set_value(bar_progress, progress, LV_ANIM_OFF);
        }
    }
}


void player_ui_set_pause(bool paused) {}

void player_ui_set_repeat_mode(repeat_mode_t mode)
{
    if (!label_mode) return;
    if (mode == REPEAT_ONE) snprintf(mode_buf, sizeof(mode_buf), "ONE");
    else if (mode == REPEAT_SHUFFLE) snprintf(mode_buf, sizeof(mode_buf), "SHF");
    else snprintf(mode_buf, sizeof(mode_buf), "ALL");
    lv_label_set_text(label_mode, mode_buf);
}

void player_ui_show_loading(bool show)
{
    if (!screen_player) return;
    
    if (show) {
        lv_obj_remove_flag(label_loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(spinner_loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_tracker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_pattern, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_mode, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_format, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_volume, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_module_info1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_module_info2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_module_info3, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_ram, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_gme_game, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_gme_author, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label_gme_system, LV_OBJ_FLAG_HIDDEN);
        
    } else {
        lv_obj_add_flag(label_loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(spinner_loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_tracker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_pattern, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_mode, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_format, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_volume, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_module_info1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_module_info2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_module_info3, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(label_ram, LV_OBJ_FLAG_HIDDEN);                
    }
}

void player_ui_set_format(const char *format)
{
    if (!label_tracker || !format) return;
    lv_label_set_text(label_tracker, format);
}

static void volume_anim_cb(void *var, int32_t val)
{
    lv_obj_set_style_text_color((lv_obj_t *)var, lv_color_hex(val), 0);
}

void player_ui_set_volume(int volume)
{
    if (!label_volume) return;
    static char vol_buf[16];
    snprintf(vol_buf, sizeof(vol_buf), "VOL:%d", volume);
    lv_label_set_text(label_volume, vol_buf);
    if (volume != last_volume) {
        last_volume = volume;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, label_volume);
        lv_anim_set_values(&a, 0x00FF00, 0x666666);
        lv_anim_set_time(&a, 500);
        lv_anim_set_delay(&a, 100);
        lv_anim_set_exec_cb(&a, volume_anim_cb);
        lv_anim_start(&a);
    }
}

void player_ui_set_module_info(int channels, int patterns, int length, 
                                int instruments, int samples, 
                                int speed, int bpm)
{
    if (!screen_player) return;
    
    /* ★ СКРЫВАЕМ GME МЕТКИ (ОНИ НЕ НУЖНЫ ДЛЯ ОБЫЧНЫХ ФОРМАТОВ) ★ */
    lv_obj_add_flag(label_gme_game, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(label_gme_author, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(label_gme_system, LV_OBJ_FLAG_HIDDEN);
    
    /* Показываем стандартные метки модульной информации */
    lv_obj_remove_flag(label_module_info1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(label_module_info3, LV_OBJ_FLAG_HIDDEN);
    
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int ram_kb = psram_free / 1024;
    
    char buf1[64], buf2[64], buf3[64];
    
    if (patterns > 0 || instruments > 0) {
        /* Трекерный формат */
        snprintf(buf1, sizeof(buf1), "Ch:%d  Pat:%d  Len:%d", channels, patterns, length);
        snprintf(buf2, sizeof(buf2), "Ins:%d  Smp:%d", instruments, samples);
        snprintf(buf3, sizeof(buf3), "Spd:%d  BPM:%d", speed, bpm);
        lv_obj_remove_flag(label_module_info2, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Обычное аудио (FLAC/MP3/WAV) */
        snprintf(buf1, sizeof(buf1), "Ch:%d  Rate:%dHz", channels, bpm);
        buf2[0] = '\0';
        snprintf(buf3, sizeof(buf3), "Bitrate:%dkbps", speed);
        lv_obj_add_flag(label_module_info2, LV_OBJ_FLAG_HIDDEN);
    }
    
    lv_label_set_text(label_module_info1, buf1);
    lv_label_set_text(label_module_info2, buf2);
    lv_label_set_text(label_module_info3, buf3);
    
    /* Обновляем RAM */
    if (label_ram) {
        char ram_buf[32];
        snprintf(ram_buf, sizeof(ram_buf), "RAM:%dK", ram_kb);
        lv_label_set_text(label_ram, ram_buf);
    }
}

void player_ui_set_gme_info(const char *game, const char *author, const char *system)
{
    if (!screen_player) return;
    
    ESP_LOGI("PLAYER_UI", "set_gme_info: game='%s', author='%s', system='%s'", 
             game ? game : "NULL", author ? author : "NULL", system ? system : "NULL");
    
    /* Скрываем стандартную модульную информацию */
    lv_obj_add_flag(label_module_info1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(label_module_info2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(label_module_info3, LV_OBJ_FLAG_HIDDEN);
    
    /* Показываем GME метки */
    lv_obj_remove_flag(label_gme_game, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(label_gme_author, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(label_gme_system, LV_OBJ_FLAG_HIDDEN);
    
    /* Устанавливаем текст */
    lv_label_set_text(label_gme_game, game && game[0] ? game : "Unknown Game");
    lv_label_set_text(label_gme_author, author && author[0] ? author : "Unknown Author");
    lv_label_set_text(label_gme_system, system && system[0] ? system : "Unknown System");
    
    /* ★ ОБНОВЛЯЕМ RAM (как в других плеерах) ★ */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int ram_kb = psram_free / 1024;
    if (label_ram) {
        char ram_buf[32];
        snprintf(ram_buf, sizeof(ram_buf), "RAM:%dK", ram_kb);
        lv_label_set_text(label_ram, ram_buf);
    }
    
    /* Принудительно обновляем */
    lv_obj_invalidate(label_gme_game);
    lv_obj_invalidate(label_gme_author);
    lv_obj_invalidate(label_gme_system);
}