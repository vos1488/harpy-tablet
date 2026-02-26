#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include "lvgl.h"

typedef void (*keyboard_done_cb_t)(const char *text);

/**
 * Show on-screen keyboard with a text area.
 * @param parent   Parent screen
 * @param title    Prompt text
 * @param initial  Initial value (can be NULL)
 * @param callback Called when user presses OK
 */
void ui_keyboard_show(lv_obj_t *parent, const char *title, 
                       const char *initial, keyboard_done_cb_t callback);

/** Hide and destroy keyboard */
void ui_keyboard_hide(void);

#endif /* UI_KEYBOARD_H */
