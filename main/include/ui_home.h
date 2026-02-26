#ifndef UI_HOME_H
#define UI_HOME_H

#include "lvgl.h"

/* Create home screen (tablet launcher) */
void ui_home_create(lv_obj_t *parent);

/* Get home screen object (for sub-screen back navigation) */
lv_obj_t *ui_home_get_screen(void);

/* Update WiFi status on home screen */
void ui_home_update_wifi_status(bool connected, const char *ip);

/* Update BT status on home screen */
void ui_home_update_bt_status(bool enabled);

/* Update clock */
void ui_home_update_time(int hour, int min);

#endif /* UI_HOME_H */
