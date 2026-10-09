/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_lcd_bands.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_mem.h"

typedef struct {
    const uint8_t *data;   /* NULL: run s_call instead */
    int x1, y1, x2, y2;
} band_t;

static const char *TAG = "lcd_bands";

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static QueueHandle_t s_bands;            /* room for LVGL's band and a call */
static SemaphoreHandle_t s_call_lock;
static SemaphoreHandle_t s_call_done;
static void (*s_call)(void *);
static void *s_call_arg;
static SemaphoreHandle_t s_chunk_free;   /* internal buffers not on the wire */
static uint8_t *s_chunk[2];
static size_t s_chunk_bytes;
static int s_chunks_out;                 /* of the band being sent */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_band_done;    /* the band's last piece has gone */
static uint32_t s_stuck;                 /* bands recovered by the timeout */

/*
 * canelita: where lcd_send is, and since when. A screen that stays stuck past
 * STUCK_REBOOT_US in any step but waiting for work restarts the board: a few
 * seconds of reboot instead of a frozen face (seen with the face and voice
 * dead, the console alive, and lvgl spinning in wait_for_flushing()).
 */
enum { ST_IDLE, ST_CHUNK_FREE, ST_DRAW, ST_BAND_DONE, ST_CALL };
static const char *const STAGE_NAME[] = { "idle", "chunk_free", "draw_bitmap", "band_done", "call" };
static volatile int s_stage = ST_IDLE;
static volatile int64_t s_stage_us;
static volatile int64_t s_wait_us;       /* LVGL waiting on the panel since (0: not) */
#define STUCK_REBOOT_US (3 * 1000000)

static void on_flush_wait(lv_event_t *e)
{
    s_wait_us = lv_event_get_code(e) == LV_EVENT_FLUSH_WAIT_START ? esp_timer_get_time() : 0;
}

static void stage(int st)
{
    s_stage = st;
    s_stage_us = esp_timer_get_time();
}

static void watch_stuck(void *arg)
{
    (void)arg;
    int st = s_stage;
    int64_t now = esp_timer_get_time(), since = s_stage_us, wait = s_wait_us;
    bool sender_stuck = st != ST_IDLE && now - since > STUCK_REBOOT_US;
    bool lvgl_stuck = wait && now - wait > STUCK_REBOOT_US;
    if (sender_stuck || lvgl_stuck) {
        ESP_LOGE(TAG, "screen stuck: lvgl waiting %.1fs, sender in %s for %.1fs (chunks_out=%d free=%u "
                 "recovered=%lu): restarting",
                 wait ? (now - wait) / 1e6 : 0.0, STAGE_NAME[st], (now - since) / 1e6, s_chunks_out,
                 (unsigned)uxSemaphoreGetCount(s_chunk_free), (unsigned long)s_stuck);
        vTaskDelay(pdMS_TO_TICKS(100));   /* let the log out */
        esp_restart();
    }
}

void muse_lcd_bands_status(char *out, size_t cap)
{
    int64_t wait = s_wait_us;
    snprintf(out, cap, "lvgl_wait=%.1fs stage=%s for %.1fs chunks_out=%d free=%u recovered=%lu",
             wait ? (esp_timer_get_time() - wait) / 1e6 : 0.0,
             STAGE_NAME[s_stage], (esp_timer_get_time() - s_stage_us) / 1e6, s_chunks_out,
             s_chunk_free ? (unsigned)uxSemaphoreGetCount(s_chunk_free) : 0, (unsigned long)s_stuck);
}

/* Past a second without word from the panel: log what was left, free the
 * buffers and let LVGL go on (a skipped frame, not a dead screen). */
static void recover(const band_t *b, const char *where)
{
    portENTER_CRITICAL(&s_lock);
    int left = s_chunks_out;
    s_chunks_out = 0;
    portEXIT_CRITICAL(&s_lock);
    s_stuck++;
    ESP_LOGE(TAG, "band %d,%d-%d,%d stuck in %s: %d piece(s) unreported, %u free buffer(s); recovered (%lu so far)",
             b->x1, b->y1, b->x2, b->y2, where, left, (unsigned)uxSemaphoreGetCount(s_chunk_free),
             (unsigned long)s_stuck);
    while (uxSemaphoreGetCount(s_chunk_free) < 2) {
        xSemaphoreGive(s_chunk_free);
    }
    lv_display_flush_ready(s_disp);
}

/* A piece has gone, or failed to; true if it was the band's last. */
static bool IRAM_ATTR chunk_done(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    bool last = s_chunks_out > 0 && --s_chunks_out == 0;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return last;
}

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    bool yield = false;
    if (chunk_done()) {
        xSemaphoreGiveFromISR(s_band_done, &woken);
        yield = esp_lv_adapter_display_notify_color_trans_done_from_isr(s_disp);
    }
    return yield || woken == pdTRUE;
}

static esp_err_t queue_band(lv_display_t *disp, esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2,
                            const void *data, void *ctx)
{
    (void)disp;
    (void)panel;
    (void)ctx;
    band_t b = { data, x1, y1, x2, y2 };
    return xQueueSend(s_bands, &b, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

/*
 * The panel's draw waits for the piece before to finish, so each piece is
 * copied while the one before is on the wire. Even row counts keep every
 * piece's window starting on an even row, which the CO5300 needs.
 */
static void send_bands(void *arg)
{
    (void)arg;
    int k = 0;
    band_t b;
    for (;;) {
        stage(ST_IDLE);
        xQueueReceive(s_bands, &b, portMAX_DELAY);
        if (!b.data) {
            stage(ST_CALL);
            s_call(s_call_arg);
            xSemaphoreGive(s_call_done);
            continue;
        }
        size_t row = (size_t)(b.x2 - b.x1) * 2;
        int rows = s_chunk_bytes / row & ~1;
        /* LVGL sends no more until this band is done, so nothing is still
         * counting down. */
        xSemaphoreTake(s_band_done, 0);  /* a late one from a recovered band */
        s_chunks_out = (b.y2 - b.y1 + rows - 1) / rows;
        bool lost = false;
        for (int y = b.y1; y < b.y2; y += rows) {
            int h = b.y2 - y < rows ? b.y2 - y : rows;
            stage(ST_CHUNK_FREE);
            if (xSemaphoreTake(s_chunk_free, pdMS_TO_TICKS(1000)) != pdTRUE) {
                recover(&b, "chunk_free");   /* a piece before never reported back */
                lost = true;
                break;
            }
            memcpy(s_chunk[k], b.data + (size_t)(y - b.y1) * row, h * row);
            stage(ST_DRAW);
            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, b.x1, y, b.x2, y + h, s_chunk[k]);
            if (err != ESP_OK) {
                xSemaphoreGive(s_chunk_free);
                if (chunk_done()) {
                    xSemaphoreGive(s_band_done);
                    lv_display_flush_ready(s_disp);
                }
            }
            k ^= 1;
        }
        /* canelita: a band whose last piece never reports back used to leave
         * LVGL waiting in wait_for_flushing() for good. */
        stage(ST_BAND_DONE);
        if (!lost && xSemaphoreTake(s_band_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            recover(&b, "band_done");
        }
    }
}

lv_display_t *muse_lcd_bands_register(esp_lv_adapter_display_config_t cfg, int lines, size_t chunk_bytes)
{
    s_panel = cfg.panel;
    s_chunk_bytes = chunk_bytes;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) != MUSE_UI_CORE) {
        ESP_LOGE(TAG, "not pinned to core %d: the panel's SPI interrupt can strand the send task", MUSE_UI_CORE);
    }
    s_bands = xQueueCreate(2, sizeof(band_t));
    s_call_lock = xSemaphoreCreateMutex();
    s_call_done = xSemaphoreCreateBinary();
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    s_band_done = xSemaphoreCreateBinary();
    s_chunk[0] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bands || !s_call_lock || !s_call_done || !s_chunk_free || !s_band_done || !s_chunk[0] || !s_chunk[1] ||
        xTaskCreatePinnedToCore(send_bands, "lcd_send", 2560, NULL, MUSE_UI_PRIORITY + 1, NULL, MUSE_UI_CORE) != pdPASS) {
        return NULL;
    }
    const esp_timer_create_args_t watch = { .callback = watch_stuck, .name = "lcd_watch" };
    esp_timer_handle_t watch_timer;
    if (esp_timer_create(&watch, &watch_timer) == ESP_OK) {
        esp_timer_start_periodic(watch_timer, 500 * 1000);
    }
    const esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_chunk_sent };
    esp_lcd_panel_io_register_event_callbacks(cfg.panel_io, &io_cbs, NULL);
    esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);

    cfg.profile.buffer_height = lines;
    cfg.profile.use_psram = true;
    cfg.profile.require_double_buffer = true;
    s_disp = esp_lv_adapter_register_display(&cfg);
    if (!s_disp) {
        return NULL;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_add_event_cb(s_disp, on_flush_wait, LV_EVENT_FLUSH_WAIT_START, NULL);
    lv_display_add_event_cb(s_disp, on_flush_wait, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
    const esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = { .custom_draw_bitmap = queue_band };
    esp_lv_adapter_set_draw_bitmap_callbacks(s_disp, &draw_cbs, NULL);
    return s_disp;
}

void muse_lcd_bands_run(void (*fn)(void *arg), void *arg)
{
    if (!s_bands) {
        fn(arg);
        return;
    }
    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    s_call = fn;
    s_call_arg = arg;
    const band_t call = { 0 };
    xQueueSend(s_bands, &call, portMAX_DELAY);
    xSemaphoreTake(s_call_done, portMAX_DELAY);
    xSemaphoreGive(s_call_lock);
}
