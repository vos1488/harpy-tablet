/*
 * MJPEG Stream Viewer — High-Performance
 *
 * Double-buffered RGB565 output, 16KB HTTP reads,
 * direct JPEG decode from network buffer, auto-scaling,
 * frame-skip for low-latency 15-25 FPS.
 */

#include "stream_viewer.h"
#include "harpy_config.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* JPEG decoder from ESP-IDF (jpeg_decoder component) */
#include "jpeg_decoder.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "stream";

/* ==================== Configuration ==================== */
#define HTTP_READ_CHUNK     16384           /* Read 16 KB at a time         */
#define NET_BUF_SIZE        STREAM_BUF_SIZE /* Network accumulation buffer  */
#define RGB_BUF_SIZE        (LCD_H_RES * LCD_V_RES * 2) /* Max RGB565 buf  */

/* ==================== State ==================== */
static stream_state_t   s_state         = STREAM_STATE_IDLE;
static stream_state_cb_t s_state_cb     = NULL;
static TaskHandle_t      s_stream_task  = NULL;
static SemaphoreHandle_t s_frame_mutex  = NULL;
static char              s_url[256]     = {};
static volatile bool     s_running      = false;

/* Double-buffered RGB565 output in PSRAM */
static uint8_t *s_rgb_buf[2]   = {NULL, NULL};
static int      s_buf_front    = 0;           /* LVGL reads from front     */
static int      s_buf_back     = 1;           /* Decoder writes to back    */
static int      s_frame_width  = 0;
static int      s_frame_height = 0;
static volatile bool s_has_new_frame = false;

/* FPS measurement */
static volatile float s_fps   = 0.0f;

static lv_img_dsc_t s_frame_dsc = {
    .header.always_zero = 0,
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data = NULL,
    .data_size = 0,
};

/* ==================== JPEG → RGB565 Decoder ==================== */

/*
 * Choose downscale factor so decoded image fits in LCD_H_RES x LCD_V_RES.
 * JPEG decoder supports 1/1, 1/2, 1/4, 1/8 scales.
 * We peek at SOF0 marker for width/height before full decode.
 */
static esp_jpeg_image_scale_t pick_scale(int w, int h)
{
    if (w <= LCD_H_RES && h <= LCD_V_RES)
        return JPEG_IMAGE_SCALE_0;              /* 1:1 */
    if (w <= LCD_H_RES * 2 && h <= LCD_V_RES * 2)
        return JPEG_IMAGE_SCALE_1_2;            /* 1:2 */
    if (w <= LCD_H_RES * 4 && h <= LCD_V_RES * 4)
        return JPEG_IMAGE_SCALE_1_4;            /* 1:4 */
    return JPEG_IMAGE_SCALE_1_8;                /* 1:8 */
}

/* Extract width/height from JPEG SOF0 (0xFFC0) marker */
static bool jpeg_peek_size(const uint8_t *data, size_t len, int *w, int *h)
{
    for (size_t i = 0; i + 8 < len; i++) {
        if (data[i] == 0xFF && (data[i + 1] == 0xC0 || data[i + 1] == 0xC2)) {
            /* SOF0/SOF2: skip marker(2) + length(2) + precision(1) */
            *h = (data[i + 5] << 8) | data[i + 6];
            *w = (data[i + 7] << 8) | data[i + 8];
            return true;
        }
    }
    return false;
}

/* Decode JPEG data in-place (no extra copy) into the BACK rgb buffer */
static bool decode_jpeg_frame(const uint8_t *jpeg_data, size_t jpeg_len)
{
    if (jpeg_len < 64 || jpeg_len > NET_BUF_SIZE) return false;

    /* Determine source resolution and pick scale */
    int src_w = 0, src_h = 0;
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    if (jpeg_peek_size(jpeg_data, jpeg_len > 512 ? 512 : jpeg_len, &src_w, &src_h)) {
        scale = pick_scale(src_w, src_h);
        if (scale != JPEG_IMAGE_SCALE_0)
            ESP_LOGD(TAG, "Auto-scale %dx%d -> scale %d", src_w, src_h, (int)scale);
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata      = jpeg_data,
        .indata_size = jpeg_len,
        .outbuf      = s_rgb_buf[s_buf_back],
        .outbuf_size = RGB_BUF_SIZE,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = scale,
        .flags.swap_color_bytes = 0,
    };

    esp_jpeg_image_output_t out;
    esp_err_t ret = esp_jpeg_decode(&cfg, &out);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "JPEG decode fail: %s (len=%u)", esp_err_to_name(ret), (unsigned)jpeg_len);
        return false;
    }

    /* Publish decoded frame — swap front/back under mutex */
    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    s_frame_width  = out.width;
    s_frame_height = out.height;
    /* Swap buffers */
    int tmp      = s_buf_front;
    s_buf_front  = s_buf_back;
    s_buf_back   = tmp;
    s_has_new_frame = true;
    xSemaphoreGive(s_frame_mutex);

    return true;
}

/* ==================== JPEG SOI/EOI Finder ==================== */

/*
 * Scan buf[0..len) for a complete JPEG frame (SOI…EOI).
 * Returns 1 if found; *jpeg_start / *jpeg_end set.
 * Uses 2-byte-step optimisation: 0xFF is rare.
 */
static int find_jpeg_in_buffer(const uint8_t *buf, int len,
                               int *jpeg_start, int *jpeg_end)
{
    *jpeg_start = -1;
    *jpeg_end   = -1;

    for (int i = 0; i < len - 1; i++) {
        if (buf[i] != 0xFF) continue;

        uint8_t marker = buf[i + 1];
        if (marker == 0xD8 && *jpeg_start == -1) {
            *jpeg_start = i;
        } else if (marker == 0xD9 && *jpeg_start != -1) {
            *jpeg_end = i + 2;
            return 1;
        }
    }
    return 0;
}

/* ==================== Stream Task ==================== */

static void stream_task(void *arg)
{
    ESP_LOGI(TAG, "Stream task started: %s", s_url);

    esp_http_client_config_t config = {
        .url            = s_url,
        .timeout_ms     = 10000,
        .buffer_size    = HTTP_READ_CHUNK,  /* 16 KB receive buffer */
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        s_state = STREAM_STATE_ERROR;
        if (s_state_cb) s_state_cb(s_state);
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        s_state = STREAM_STATE_ERROR;
        if (s_state_cb) s_state_cb(s_state);
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Connected, content_length=%d", content_len);

    s_state = STREAM_STATE_STREAMING;
    if (s_state_cb) s_state_cb(s_state);

    /* Network read buffer in PSRAM */
    uint8_t *read_buf = heap_caps_malloc(NET_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!read_buf) {
        ESP_LOGE(TAG, "Failed to alloc read buffer (%d bytes)", NET_BUF_SIZE);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_state = STREAM_STATE_ERROR;
        if (s_state_cb) s_state_cb(s_state);
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int buf_pos = 0;

    /* FPS counter */
    int64_t fps_start   = esp_timer_get_time();
    int     fps_frames  = 0;

    while (s_running) {
        /* ---- Read network data ---- */
        int remain = NET_BUF_SIZE - buf_pos;
        if (remain <= 0) {
            ESP_LOGW(TAG, "Buffer overflow, resetting");
            buf_pos = 0;
            continue;
        }
        int want = remain > HTTP_READ_CHUNK ? HTTP_READ_CHUNK : remain;

        int read_len = esp_http_client_read(client,
                                            (char *)(read_buf + buf_pos), want);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Read error");
            break;
        }
        if (read_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        buf_pos += read_len;

        /* ---- Extract & decode JPEG frames ---- */
        int jpeg_start, jpeg_end;
        while (find_jpeg_in_buffer(read_buf, buf_pos, &jpeg_start, &jpeg_end)) {
            int jpeg_len = jpeg_end - jpeg_start;

            /* Check if there is ANOTHER complete frame after this one.
             * If yes, skip the current (stale) frame for lower latency. */
            int remaining_after = buf_pos - jpeg_end;
            int next_start, next_end;
            bool skip = false;
            if (remaining_after > 4 &&
                find_jpeg_in_buffer(read_buf + jpeg_end, remaining_after,
                                    &next_start, &next_end)) {
                /* There's a newer frame — skip this one */
                skip = true;
                ESP_LOGD(TAG, "Skipping stale frame (%d bytes)", jpeg_len);
            }

            if (!skip && jpeg_len >= 64 && jpeg_len < NET_BUF_SIZE) {
                if (decode_jpeg_frame(read_buf + jpeg_start, jpeg_len)) {
                    fps_frames++;
                }
            }

            /* Shift remaining data to front of buffer */
            int tail = buf_pos - jpeg_end;
            if (tail > 0) {
                memmove(read_buf, read_buf + jpeg_end, tail);
            }
            buf_pos = tail;
        }

        /* ---- FPS measurement (every 2 seconds) ---- */
        int64_t now = esp_timer_get_time();
        int64_t elapsed_us = now - fps_start;
        if (elapsed_us >= 2000000) {
            s_fps = (float)fps_frames * 1000000.0f / (float)elapsed_us;
            ESP_LOGI(TAG, "FPS: %.1f", s_fps);
            fps_frames = 0;
            fps_start  = now;
        }

        taskYIELD();   /* Let other tasks run without adding delay */
    }

    free(read_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    s_state = STREAM_STATE_IDLE;
    if (s_state_cb) s_state_cb(s_state);

    ESP_LOGI(TAG, "Stream task ended");
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

/* ==================== Public API ==================== */

void stream_viewer_init(void)
{
    if (!s_frame_mutex) {
        s_frame_mutex = xSemaphoreCreateMutex();
    }

    /* Double RGB565 buffers in PSRAM */
    for (int i = 0; i < 2; i++) {
        if (!s_rgb_buf[i]) {
            s_rgb_buf[i] = heap_caps_malloc(RGB_BUF_SIZE, MALLOC_CAP_SPIRAM);
            if (s_rgb_buf[i]) memset(s_rgb_buf[i], 0, RGB_BUF_SIZE);
        }
    }

    ESP_LOGI(TAG, "Stream viewer initialised (double-buffered, %dKB net buf)",
             NET_BUF_SIZE / 1024);
}

void stream_viewer_start(const char *url)
{
    if (s_stream_task) {
        stream_viewer_stop();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_running = true;
    s_has_new_frame = false;
    s_fps = 0.0f;
    s_state = STREAM_STATE_CONNECTING;
    if (s_state_cb) s_state_cb(s_state);

    ESP_LOGI(TAG, "Starting stream: %s", s_url);
    xTaskCreatePinnedToCore(stream_task, "stream", STREAM_TASK_STACK, NULL,
                            STREAM_TASK_PRIORITY, &s_stream_task, 1);
}

void stream_viewer_stop(void)
{
    ESP_LOGI(TAG, "Stopping stream");
    s_running = false;
    /* Task will self-delete */
}

stream_state_t stream_viewer_get_state(void)
{
    return s_state;
}

float stream_viewer_get_fps(void)
{
    return s_fps;
}

lv_img_dsc_t *stream_viewer_get_frame(void)
{
    if (!s_has_new_frame || !s_rgb_buf[s_buf_front] || s_frame_width <= 0)
        return NULL;

    xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(5));
    s_frame_dsc.header.cf   = LV_IMG_CF_TRUE_COLOR;
    s_frame_dsc.header.w    = s_frame_width;
    s_frame_dsc.header.h    = s_frame_height;
    s_frame_dsc.data        = s_rgb_buf[s_buf_front];
    s_frame_dsc.data_size   = s_frame_width * s_frame_height * 2;
    s_has_new_frame = false;
    xSemaphoreGive(s_frame_mutex);

    return &s_frame_dsc;
}

void stream_viewer_set_state_cb(stream_state_cb_t cb)
{
    s_state_cb = cb;
}
