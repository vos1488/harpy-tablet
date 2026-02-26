/*
 * SD Card File Browser
 * SPI SD card with CH422G-managed CS pin.
 * Full file manager: browse, view text, delete, mkdir, free space, refresh.
 */

#include "ui_sdcard.h"
#include "ui_home.h"
#include "harpy_config.h"
#include "ch422g.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "ui_sd";

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_file_list = NULL;
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_path_lbl = NULL;
static lv_obj_t *s_free_lbl = NULL;

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;
static char s_current_path[256] = SD_MOUNT_POINT;

/* Currently selected file for context actions */
static char s_selected_file[256] = {0};

/* ==================== SD Card Mount/Unmount ==================== */

bool sd_mount(void)
{
    if (s_mounted) return true;

    ESP_LOGI(TAG, "Mounting SD card (SDMMC 1-bit)...");
    esp_log_level_set("gpio", ESP_LOG_WARN);

    /* CH422G IO4 controls SD D3/CS pin.
     * In SDMMC mode D3 must be HIGH (pull-up) to avoid card entering SPI mode. */
    ch422g_set_pins(I2C_BUS_NUM, CH422G_PIN_SD_CS);
    vTaskDelay(pdMS_TO_TICKS(20));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk = GPIO_NUM_12;   /* SD_CLK  */
    slot_cfg.cmd = GPIO_NUM_11;   /* SD_CMD  */
    slot_cfg.d0  = GPIO_NUM_13;   /* SD_D0   */
    slot_cfg.width = 1;           /* 1-bit mode */
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                             &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    s_mounted = true;
    strcpy(s_current_path, SD_MOUNT_POINT);
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

bool sd_is_mounted(void)
{
    return s_mounted;
}

const char *sd_get_mount_point(void)
{
    return SD_MOUNT_POINT;
}

/* ==================== Free Space ==================== */

static void update_free_space(void)
{
    if (!s_mounted || !s_free_lbl) return;

    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
        uint64_t total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
        uint64_t free_bytes = (uint64_t)fre_clust * fs->csize * 512;
        char txt[128];
        snprintf(txt, sizeof(txt), "Free: %llu / %llu MB",
                 (unsigned long long)(free_bytes / (1024 * 1024)),
                 (unsigned long long)(total / (1024 * 1024)));
        lv_label_set_text(s_free_lbl, txt);
    }
}

/* ==================== Forward Declarations ==================== */

static void navigate_to(const char *path);
static void up_dir_cb(lv_event_t *e);

/* ==================== Delete File/Dir ==================== */

static int rmdir_recursive(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return -1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                rmdir_recursive(full);
            } else {
                unlink(full);
            }
        }
    }
    closedir(dir);
    return rmdir(path);
}

static void confirm_delete_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *txt = lv_msgbox_get_active_btn_text(mbox);
    if (txt && strcmp(txt, "Delete") == 0) {
        struct stat st;
        if (stat(s_selected_file, &st) == 0) {
            int r;
            if (S_ISDIR(st.st_mode)) {
                r = rmdir_recursive(s_selected_file);
            } else {
                r = unlink(s_selected_file);
            }
            ESP_LOGI(TAG, "Delete %s: %s", s_selected_file, r == 0 ? "OK" : "FAIL");
        }
        navigate_to(s_current_path);
        update_free_space();
    }
    lv_msgbox_close(mbox);
}

static void delete_file_cb(lv_event_t *e)
{
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;
    strncpy(s_selected_file, path, sizeof(s_selected_file) - 1);

    static const char *btns[] = {"Delete", "Cancel", ""};
    char msg[300];
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    snprintf(msg, sizeof(msg), "Delete \"%s\"?", basename);
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Confirm Delete", msg, btns, true);
    lv_obj_set_size(mbox, 400, 200);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, confirm_delete_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ==================== Create Directory ==================== */

static void mkdir_text_cb(const char *name);

static void mkdir_btn_cb(lv_event_t *e)
{
    extern void ui_keyboard_show(lv_obj_t *parent, const char *title,
                                  const char *init_text,
                                  void (*on_done)(const char *));
    ui_keyboard_show(lv_scr_act(), "New Folder Name", NULL, mkdir_text_cb);
}

static void mkdir_text_cb(const char *name)
{
    if (!name || strlen(name) == 0) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", s_current_path, name);
    int r = mkdir(path, 0775);
    ESP_LOGI(TAG, "mkdir %s: %s", path, r == 0 ? "OK" : "FAIL");
    navigate_to(s_current_path);
}

/* ==================== File Item Callback ==================== */

static void file_item_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = lv_obj_get_user_data(btn);
    if (!name) return;

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", s_current_path, name);

    struct stat st;
    if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        navigate_to(full_path);
    } else {
        /* View text file */
        FILE *f = fopen(full_path, "r");
        if (f) {
            char buf[2048];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            fclose(f);
            lv_obj_t *mbox = lv_msgbox_create(NULL, name, buf, NULL, true);
            lv_obj_set_size(mbox, 650, 380);
            lv_obj_set_style_text_font(mbox, &lv_font_montserrat_14, 0);
            lv_obj_center(mbox);
        }
    }
}

/* ==================== Navigate to Directory ==================== */

static void navigate_to(const char *path)
{
    strncpy(s_current_path, path, sizeof(s_current_path) - 1);
    if (s_path_lbl) {
        lv_label_set_text(s_path_lbl, s_current_path);
    }

    if (!s_file_list) return;
    lv_obj_clean(s_file_list);

    /* ".." button to go up */
    if (strcmp(s_current_path, SD_MOUNT_POINT) != 0) {
        lv_obj_t *up_btn = lv_btn_create(s_file_list);
        lv_obj_set_size(up_btn, lv_pct(100), 40);
        lv_obj_set_style_bg_color(up_btn, HARPY_COLOR_CARD, 0);
        lv_obj_set_style_radius(up_btn, 6, 0);
        lv_obj_t *up_lbl = lv_label_create(up_btn);
        lv_label_set_text(up_lbl, LV_SYMBOL_LEFT "  ..");
        lv_obj_set_style_text_color(up_lbl, HARPY_COLOR_PRIMARY, 0);
        lv_obj_center(up_lbl);
        lv_obj_add_event_cb(up_btn, up_dir_cb, LV_EVENT_CLICKED, NULL);
    }

    DIR *dir = opendir(s_current_path);
    if (!dir) {
        lv_obj_t *err = lv_label_create(s_file_list);
        lv_label_set_text(err, "Failed to open directory");
        lv_obj_set_style_text_color(err, HARPY_COLOR_ERROR, 0);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", s_current_path, entry->d_name);

        struct stat st;
        bool is_dir = false;
        long size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = st.st_size;
        }

        lv_obj_t *row = lv_obj_create(s_file_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_style_bg_color(row, HARPY_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Store name for callback */
        char *name_copy = lv_mem_alloc(strlen(entry->d_name) + 1);
        strcpy(name_copy, entry->d_name);

        /* Clickable file/folder area */
        lv_obj_t *click_area = lv_btn_create(row);
        lv_obj_remove_style_all(click_area);
        lv_obj_set_size(click_area, lv_pct(85), lv_pct(100));
        lv_obj_align(click_area, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_user_data(click_area, name_copy);
        lv_obj_add_event_cb(click_area, file_item_cb, LV_EVENT_CLICKED, NULL);

        /* Icon */
        lv_obj_t *icon = lv_label_create(click_area);
        lv_label_set_text(icon, is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
        lv_obj_set_style_text_color(icon, is_dir ? HARPY_COLOR_PRIMARY : HARPY_COLOR_MUTED, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        /* Name */
        lv_obj_t *name_lbl = lv_label_create(click_area);
        lv_label_set_text(name_lbl, entry->d_name);
        lv_obj_set_style_text_color(name_lbl, HARPY_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_max_width(name_lbl, 400, 0);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 28, 0);

        /* Size label */
        if (!is_dir) {
            lv_obj_t *size_lbl = lv_label_create(click_area);
            char sz[32];
            if (size > 1024 * 1024)
                snprintf(sz, sizeof(sz), "%.1f MB", size / (1024.0f * 1024.0f));
            else if (size > 1024)
                snprintf(sz, sizeof(sz), "%.1f KB", size / 1024.0f);
            else
                snprintf(sz, sizeof(sz), "%ld B", size);
            lv_label_set_text(size_lbl, sz);
            lv_obj_set_style_text_color(size_lbl, HARPY_COLOR_MUTED, 0);
            lv_obj_set_style_text_font(size_lbl, &lv_font_montserrat_14, 0);
            lv_obj_align(size_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        }

        /* Delete button — store full path */
        char *full_copy = lv_mem_alloc(strlen(full) + 1);
        strcpy(full_copy, full);
        lv_obj_t *del_btn = lv_btn_create(row);
        lv_obj_set_size(del_btn, 36, 30);
        lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(del_btn, HARPY_COLOR_ERROR, 0);
        lv_obj_set_style_radius(del_btn, 6, 0);
        lv_obj_set_style_pad_all(del_btn, 0, 0);
        lv_obj_add_event_cb(del_btn, delete_file_cb, LV_EVENT_CLICKED, full_copy);
        lv_obj_t *del_icon = lv_label_create(del_btn);
        lv_label_set_text(del_icon, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(del_icon, lv_color_white(), 0);
        lv_obj_center(del_icon);
    }
    closedir(dir);
}

/* ==================== Toolbar Callbacks ==================== */

static void back_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_user_data(e);
    s_file_list = NULL;
    s_info_lbl = NULL;
    s_path_lbl = NULL;
    s_free_lbl = NULL;
    lv_scr_load(ui_home_get_screen());
    if (scr) lv_obj_del(scr);
    s_screen = NULL;
}

static void up_dir_cb(lv_event_t *e)
{
    if (strcmp(s_current_path, SD_MOUNT_POINT) == 0) return;
    char *last_slash = strrchr(s_current_path, '/');
    if (last_slash && last_slash != s_current_path) {
        *last_slash = '\0';
    } else {
        strcpy(s_current_path, SD_MOUNT_POINT);
    }
    navigate_to(s_current_path);
}

static void refresh_cb(lv_event_t *e)
{
    navigate_to(s_current_path);
    update_free_space();
}

/* ==================== Public API ==================== */

void ui_sdcard_open(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, HARPY_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Top bar */
    lv_obj_t *top = lv_obj_create(s_screen);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LCD_H_RES, 50);
    lv_obj_set_style_bg_color(top, HARPY_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    /* Back */
    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(btn_back, HARPY_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_back, 8, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, s_screen);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(lb, lv_color_white(), 0);
    lv_obj_center(lb);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, LV_SYMBOL_DRIVE "  SD Card");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, HARPY_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Right buttons: mkdir, refresh, up */
    lv_obj_t *mkdir_btn = lv_btn_create(top);
    lv_obj_set_size(mkdir_btn, 40, 40);
    lv_obj_align(mkdir_btn, LV_ALIGN_RIGHT_MID, -110, 0);
    lv_obj_set_style_bg_color(mkdir_btn, HARPY_COLOR_SUCCESS, 0);
    lv_obj_set_style_radius(mkdir_btn, 8, 0);
    lv_obj_add_event_cb(mkdir_btn, mkdir_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mk_lbl = lv_label_create(mkdir_btn);
    lv_label_set_text(mk_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(mk_lbl, lv_color_white(), 0);
    lv_obj_center(mk_lbl);

    lv_obj_t *ref_btn = lv_btn_create(top);
    lv_obj_set_size(ref_btn, 40, 40);
    lv_obj_align(ref_btn, LV_ALIGN_RIGHT_MID, -60, 0);
    lv_obj_set_style_bg_color(ref_btn, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(ref_btn, 8, 0);
    lv_obj_add_event_cb(ref_btn, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rf_lbl = lv_label_create(ref_btn);
    lv_label_set_text(rf_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(rf_lbl, lv_color_white(), 0);
    lv_obj_center(rf_lbl);

    lv_obj_t *up_btn = lv_btn_create(top);
    lv_obj_set_size(up_btn, 40, 40);
    lv_obj_align(up_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(up_btn, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(up_btn, 8, 0);
    lv_obj_add_event_cb(up_btn, up_dir_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_lbl = lv_label_create(up_btn);
    lv_label_set_text(up_lbl, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(up_lbl, lv_color_white(), 0);
    lv_obj_center(up_lbl);

    /* Content */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - 50);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(content, 10, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, 0);

    /* Card info line */
    lv_obj_t *info_row = lv_obj_create(content);
    lv_obj_remove_style_all(info_row);
    lv_obj_set_size(info_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_info_lbl = lv_label_create(info_row);
    lv_obj_set_style_text_color(s_info_lbl, HARPY_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_info_lbl, &lv_font_montserrat_14, 0);

    s_free_lbl = lv_label_create(info_row);
    lv_obj_set_style_text_color(s_free_lbl, HARPY_COLOR_SUCCESS, 0);
    lv_obj_set_style_text_font(s_free_lbl, &lv_font_montserrat_14, 0);

    /* Path label */
    s_path_lbl = lv_label_create(content);
    lv_obj_set_style_text_color(s_path_lbl, HARPY_COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_path_lbl, &lv_font_montserrat_14, 0);

    /* File list */
    s_file_list = lv_obj_create(content);
    lv_obj_remove_style_all(s_file_list);
    lv_obj_set_size(s_file_list, lv_pct(100), LCD_V_RES - 155);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_file_list, 3, 0);
    lv_obj_add_flag(s_file_list, LV_OBJ_FLAG_SCROLLABLE);

    /* Mount and show */
    if (sd_mount()) {
        char info[128];
        snprintf(info, sizeof(info), "Card: %s  |  Size: %llu MB",
                 s_card->cid.name,
                 (unsigned long long)(((uint64_t)s_card->csd.capacity) *
                  s_card->csd.sector_size / (1024 * 1024)));
        lv_label_set_text(s_info_lbl, info);
        update_free_space();
        navigate_to(s_current_path);
    } else {
        lv_label_set_text(s_info_lbl, "No SD card detected or mount failed");
        lv_label_set_text(s_free_lbl, "");
        lv_label_set_text(s_path_lbl, "Insert SD card and reopen");
    }

    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "SD card screen opened");
}
