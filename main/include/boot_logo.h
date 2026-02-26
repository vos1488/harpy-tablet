#ifndef BOOT_LOGO_H
#define BOOT_LOGO_H

#include "lvgl.h"

/**
 * Show the HARPY boot logo with animation.
 * Calls on_complete callback when animation finishes.
 */
void boot_logo_show(lv_obj_t *parent, void (*on_complete)(void));

#endif /* BOOT_LOGO_H */
