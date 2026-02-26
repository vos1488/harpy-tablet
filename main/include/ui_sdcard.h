#ifndef UI_SDCARD_H
#define UI_SDCARD_H

#include <stdbool.h>

/* Open the SD Card file browser screen */
void ui_sdcard_open(void);

/* Mount SD card (safe to call multiple times) */
bool sd_mount(void);

/* Check if SD card is mounted */
bool sd_is_mounted(void);

/* Get mount point path */
const char *sd_get_mount_point(void);

#endif
