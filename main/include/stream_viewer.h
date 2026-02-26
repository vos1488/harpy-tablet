#ifndef STREAM_VIEWER_H
#define STREAM_VIEWER_H

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    STREAM_STATE_IDLE = 0,
    STREAM_STATE_CONNECTING,
    STREAM_STATE_STREAMING,
    STREAM_STATE_ERROR,
} stream_state_t;

typedef void (*stream_state_cb_t)(stream_state_t state);

/* Initialize stream viewer */
void stream_viewer_init(void);

/* Start MJPEG stream from given URL (http://ip:port/path) */
void stream_viewer_start(const char *url);

/* Stop stream */
void stream_viewer_stop(void);

/* Get current state */
stream_state_t stream_viewer_get_state(void);

/* Get decoded frame as LVGL image descriptor (RGB565) */
lv_img_dsc_t *stream_viewer_get_frame(void);

/* Get current measured FPS */
float stream_viewer_get_fps(void);

/* Set state callback */
void stream_viewer_set_state_cb(stream_state_cb_t cb);

#endif /* STREAM_VIEWER_H */
