#ifndef MARKEE_INI_H
#define MARKEE_INI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "draw.h"
#include "renderer.h"

typedef struct {
    int width;
    int height;
    float zoom;
    MarkeePixelShape pixel_shape;
    bool mono_enabled;
    MarkeeColor mono_color;
    char host[64];
    uint16_t port;
    char token[256];
    float override_dpi;
    bool always_on_top;
    bool borderless;
    bool has_position;
    bool center_x;
    bool center_y;
    int pos_x;
    int pos_y;
    MarkeeWindowCorner corner;
    MarkeeCaseTheme case_theme;
    double idle_dim_sec;
    uint8_t idle_brightness;
    bool start_demo;
    bool mute;
    bool headless;
    bool save_position;
} MarkeeConfig;

/* Populate cfg with compiled-in defaults. */
void markee_config_defaults(MarkeeConfig *cfg);

/* Resolve default config path: $XDG_CONFIG_HOME/markee/config.ini or ~/.config/markee/config.ini. */
bool markee_config_default_path(char *out, size_t out_size);

/* Parse INI key=value text into cfg (only keys present in text are modified).
 * Returns false on syntax/value error and writes a message into err_buf (if non-NULL). */
bool markee_config_parse_string(const char *text, MarkeeConfig *cfg,
                                char *err_buf, size_t err_size);

/* Load and parse an INI config file into cfg.
 * Returns true on success; on failure sets *out_not_found (if non-NULL) when file does not exist. */
bool markee_config_load_file(const char *path, MarkeeConfig *cfg,
                             bool *out_not_found, char *err_buf, size_t err_size);

/* Update or append `position = X,Y` in the given INI file, creating parent directories if needed
 * and preserving all other keys and comments. */
bool markee_config_save_position(const char *path, int pos_x, int pos_y);

#endif /* MARKEE_INI_H */
