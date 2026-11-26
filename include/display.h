#ifndef MARKEE_DISPLAY_H
#define MARKEE_DISPLAY_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "gif.h"

typedef enum {
    MARKEE_OK = 0,
    MARKEE_ERR_INVALID_ARG,
    MARKEE_ERR_INVALID_PNG,
    MARKEE_ERR_INVALID_GIF,
    MARKEE_ERR_WRONG_DIMENSIONS,
    MARKEE_ERR_ENCODE_FAILED,
    MARKEE_ERR_OUT_OF_RANGE
} MarkeeResult;

typedef enum {
    MARKEE_TRANS_NONE = 0,
    MARKEE_TRANS_FADE,
    MARKEE_TRANS_SLIDE_LEFT,
    MARKEE_TRANS_SLIDE_UP,
    MARKEE_TRANS_WIPE,              /* wipe-right */
    MARKEE_TRANS_CENTER_OUT,
    MARKEE_TRANS_SLIDE_RIGHT,
    MARKEE_TRANS_SLIDE_DOWN,
    MARKEE_TRANS_WIPE_LEFT,
    MARKEE_TRANS_WIPE_UP,
    MARKEE_TRANS_WIPE_DOWN,
    MARKEE_TRANS_SPLIT,
    MARKEE_TRANS_BLINDS,
    MARKEE_TRANS_DISSOLVE,
    MARKEE_TRANS_DIP
} MarkeeTransitionType;

typedef struct {
    MarkeeTransitionType type;
    double duration_sec;
    bool has_start_time;
    double start_time;
    uint8_t *from_pixels;
} MarkeeTransitionState;

typedef struct {
    bool active;
    char text[512];
    char font[16];
    int start_x;
    int y;
    int text_w;
    uint8_t r, g, b;
    double speed;       /* pixels per second */
    bool has_clip;
    int clip_x, clip_y, clip_w, clip_h;
    bool has_start_time;
    double start_time;
    uint8_t *bg_pixels; /* background snapshot underneath scrolling text */
} MarkeeScrollState;

#define MARKEE_MAX_GIF_LAYERS 8
#define MARKEE_EVENT_RING_CAP 64

typedef enum {
    MARKEE_EVT_NONE = 0,
    MARKEE_EVT_CLICK,
    MARKEE_EVT_BUTTON,
    MARKEE_EVT_FRAME
} MarkeeEventType;

typedef struct {
    uint64_t id;
    MarkeeEventType type;
    int x;
    int y;
    char detail[32];
} MarkeeEvent;

typedef struct {
    MarkeeGif *gif;
    size_t current_frame;
    int loops_done;
    bool finished;
    double accum_ms;
} MarkeeGifLayer;

typedef struct {
    bool active;
    bool paused;
    double speed;
    size_t count;
    MarkeeGifLayer layers[MARKEE_MAX_GIF_LAYERS];
    bool has_last_time;
    double last_time;
    uint8_t *bg_pixels; /* static background snapshot beneath animated GIF layers */
} MarkeeGifAnimState;

typedef struct {
    bool active;
    bool paused;
    double speed;
    int src_width;
    int src_height;
    size_t frame_count;
    size_t current_frame;
    uint64_t total_duration_ms;
    int loop_count;
    uint8_t fill_r;
    uint8_t fill_g;
    uint8_t fill_b;
} MarkeeGifStatus;

typedef enum {
    MARKEE_WIDGET_NONE = 0,
    MARKEE_WIDGET_CLOCK,
    MARKEE_WIDGET_COUNTDOWN
} MarkeeWidgetType;

typedef struct {
    MarkeeWidgetType type;
    int x, y;
    bool has_x;
    int align;          /* 0=left, 1=center, 2=right */
    char font[16];
    char format[16];    /* clock: "24h", "24hm", "12h", "12hm" */
    uint8_t r, g, b;
    uint8_t done_r, done_g, done_b;
    char done_text[32];
    double duration_sec;
    bool has_start_time;
    double start_time;
    bool show_bar;
    uint8_t bar_r, bar_g, bar_b;
    int done_sound_id;  /* 0=none, 1..3=alert sound on countdown finish */
    bool done_fired;
    uint8_t *bg_pixels; /* background snapshot underneath live widget */
} MarkeeWidgetState;

typedef struct {
    bool active;
    double duration_sec;
    bool has_start_time;
    double start_time;
    uint8_t *saved_pixels;
    uint8_t *saved_scroll_bg;
    uint8_t *saved_gif_bg;
    uint8_t *saved_widget_bg;
    bool saved_demo_active;
    MarkeeScrollState saved_scroll;
    MarkeeGifAnimState saved_gif;
    MarkeeWidgetState saved_widget;
} MarkeeNotifyState;

typedef struct {
    bool active;
    int fps;
    size_t max_frames;
    size_t frame_count;
    size_t frame_cap;
    uint8_t *frames;     /* Captured RGB24 frames (with monochrome applied) */
    bool has_last_capture;
    double last_capture_time;
} MarkeeRecordState;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t event_cond;
    pthread_mutex_t png_lock;
    int width;
    int height;
    size_t frame_bytes;                 /* width * height * 3 */
    uint8_t *pixels;                    /* Row-major WxH 24-bit RGB */
    uint8_t brightness;                 /* 0..100 */
    bool mono_enabled;                  /* Non-destructive BT.709 monochrome tint filter */
    uint8_t mono_r;
    uint8_t mono_g;
    uint8_t mono_b;
    bool demo_active;                   /* Startup animation enabled until API write */
    uint64_t frames_received;
    uint64_t frame_seq;
    MarkeeScrollState scroll;
    MarkeeGifAnimState gif;
    MarkeeWidgetState widget;
    MarkeeTransitionState trans;
    int pending_alert_sound;            /* 0=none, 1..3=pending sound from autonomous widget */
    MarkeeNotifyState notify;
    MarkeeRecordState rec;
    MarkeeEvent events[MARKEE_EVENT_RING_CAP];
    size_t event_head;
    size_t event_count;
    uint64_t next_event_id;
    bool shutting_down;
    double idle_dim_sec;                /* 0 = disabled; >0 = seconds before dimming GUI */
    uint8_t idle_brightness;            /* Target dimmed GUI brightness (0..100, default 20) */
    bool has_tick_time;
    double last_tick_time;
    double last_activity_time;
} MarkeeDisplay;

/* Initialize and destroy display state for the given resolution (width x height). */
bool display_init(MarkeeDisplay *disp, int width, int height, bool start_demo);
void display_destroy(MarkeeDisplay *disp);

/* Thread-safe raw 24-bit RGB framebuffer operations. */
void display_clear(MarkeeDisplay *disp, bool stop_demo);
void display_fill(MarkeeDisplay *disp, uint8_t r, uint8_t g, uint8_t b, bool stop_demo);
MarkeeResult display_set_pixel(MarkeeDisplay *disp, int x, int y,
                               uint8_t r, uint8_t g, uint8_t b, bool stop_demo);
MarkeeResult display_get_pixel(MarkeeDisplay *disp, int x, int y,
                               uint8_t *r, uint8_t *g, uint8_t *b);
MarkeeResult display_set_raw_rgb(MarkeeDisplay *disp, const uint8_t *rgb, size_t len,
                                 bool stop_demo);
bool display_get_raw_rgb(MarkeeDisplay *disp, uint8_t *out_rgb, size_t out_cap);

/* Fast snapshot for the 60 Hz renderer. */
bool display_snapshot(MarkeeDisplay *disp, uint8_t *out_rgb, size_t out_cap,
                      uint8_t *out_brightness);

/* Brightness (0..100), monochrome filter, and demo controls. */
void display_set_brightness(MarkeeDisplay *disp, uint8_t brightness);
uint8_t display_get_brightness(MarkeeDisplay *disp);
void display_set_monochrome(MarkeeDisplay *disp, bool enabled,
                            uint8_t r, uint8_t g, uint8_t b);
void display_get_monochrome(MarkeeDisplay *disp, bool *enabled,
                            uint8_t *r, uint8_t *g, uint8_t *b);
void display_set_demo(MarkeeDisplay *disp, bool active);
bool display_is_demo_active(MarkeeDisplay *disp);
uint64_t display_get_frames_received(MarkeeDisplay *disp);

/* PNG I/O: decode an exact WxH PNG into the framebuffer, decode any PNG to RGBA32 (caller frees
 * *out_rgba with free()), or encode current pixels as a WxH 24-bit PNG (caller frees *out_png
 * with display_free_buffer()). */
MarkeeResult display_load_png(MarkeeDisplay *disp, const uint8_t *png_data, size_t png_len,
                              int *actual_w, int *actual_h);
bool display_decode_png_rgba(MarkeeDisplay *disp, const uint8_t *png_data, size_t png_len,
                             uint8_t **out_rgba, int *out_w, int *out_h);
MarkeeResult display_encode_png(MarkeeDisplay *disp, uint8_t **out_png, size_t *out_len);
void display_free_buffer(uint8_t *buf);

/* GIF animation I/O: decode an animated/static GIF (center-cropped if too big, centered if too
 * small), compositing transparent pixels and margins over (fill_r, fill_g, fill_b).
 * Displays frame 0 immediately and advances frames in display_tick().
 * Pass loop_override < 0 to honor the GIF's own loop setting (0 = infinite, >0 = total plays). */
MarkeeResult display_load_gif(MarkeeDisplay *disp, const uint8_t *gif_data, size_t gif_len,
                              int loop_override,
                              uint8_t fill_r, uint8_t fill_g, uint8_t fill_b,
                              MarkeeGifStatus *out_status,
                              char *err_buf, size_t err_size);
MarkeeResult display_load_gif_ex(MarkeeDisplay *disp, const uint8_t *gif_data, size_t gif_len,
                                 int loop_override, MarkeeGifFitMode fit, double speed,
                                 uint8_t fill_r, uint8_t fill_g, uint8_t fill_b,
                                 MarkeeGifStatus *out_status,
                                 char *err_buf, size_t err_size);
void display_set_gif_paused(MarkeeDisplay *disp, bool paused);
void display_set_gif_speed(MarkeeDisplay *disp, double speed);
void display_stop_gif(MarkeeDisplay *disp);
void display_stop_gif_unlocked(MarkeeDisplay *disp);
bool display_get_gif_status(MarkeeDisplay *disp, MarkeeGifStatus *out_status);
bool display_is_gif_active(MarkeeDisplay *disp);

/* Scene transitions ("fade", "dip", "slide-left", "slide-right", "slide-up", "slide-down", "wipe", "wipe-left", "wipe-up", "wipe-down", "split", "center-out", "blinds", "dissolve"). */
bool display_parse_transition(const char *s, MarkeeTransitionType *out);
void display_capture_transition_from(MarkeeDisplay *disp, uint8_t *out_from);
void display_activate_transition(MarkeeDisplay *disp, const uint8_t *from_pixels,
                                 MarkeeTransitionType type, int duration_ms);

/* Transient notification TTL support: save current state before drawing a temporary
 * overlay, automatically restore after ttl_sec in display_tick(), or dismiss early. */
void display_notify_begin_unlocked(MarkeeDisplay *disp, double ttl_sec);
void display_notify_cancel_unlocked(MarkeeDisplay *disp);
bool display_notify_dismiss(MarkeeDisplay *disp);
bool display_get_notify_status(MarkeeDisplay *disp, double *out_ttl, double *out_remaining);

/* Idle dimming / night mode (GUI brightness dims after timeout_sec of no writes). */
void display_set_idle_dim(MarkeeDisplay *disp, double timeout_sec, uint8_t idle_brightness);
void display_get_idle_dim(MarkeeDisplay *disp, double *timeout_sec,
                          uint8_t *idle_brightness, bool *is_dimmed);
void display_wake_activity(MarkeeDisplay *disp);

/* Event ring buffer & long-poll / SSE wakeup helpers. */
const char *display_event_type_name(MarkeeEventType type);
void display_push_event_unlocked(MarkeeDisplay *disp, MarkeeEventType type,
                                 int x, int y, const char *detail);
void display_push_event(MarkeeDisplay *disp, MarkeeEventType type,
                        int x, int y, const char *detail);
uint64_t display_get_last_event_id(MarkeeDisplay *disp);
size_t display_get_events_since(MarkeeDisplay *disp, uint64_t since_id,
                                MarkeeEvent *out, size_t max_out);
size_t display_wait_for_events(MarkeeDisplay *disp, uint64_t since_id, int timeout_ms,
                               MarkeeEvent *out, size_t max_out);
void display_signal_shutdown(MarkeeDisplay *disp);

/* Live animated GIF recording (captures frames during display_tick()). */
bool display_record_start(MarkeeDisplay *disp, int fps, size_t max_frames);
bool display_record_stop(MarkeeDisplay *disp, uint8_t **out_gif, size_t *out_len);
void display_get_record_status(MarkeeDisplay *disp, bool *active, size_t *frames,
                               int *fps, size_t *max_frames);

/* Drain an optional alert sound ID (1..3, or 0 if none) queued by a finished countdown widget. */
int display_take_pending_alert(MarkeeDisplay *disp);

/* Advance 60 Hz animations (startup demo, active GIF animation, widget, or scrolling text). */
void display_tick(MarkeeDisplay *disp, double time_sec);

/* Backward-compatible alias for display_tick(). */
void display_tick_demo(MarkeeDisplay *disp, double time_sec);

#endif /* MARKEE_DISPLAY_H */
