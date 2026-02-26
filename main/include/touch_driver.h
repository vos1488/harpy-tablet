#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include "lvgl.h"

/* Initialize GT911 touch and register with LVGL */
lv_indev_t *touch_driver_init(lv_disp_t *disp);

#endif /* TOUCH_DRIVER_H */
