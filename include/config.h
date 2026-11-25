#ifndef MARKEE_CONFIG_H
#define MARKEE_CONFIG_H

/* Hardware display specification (BUSY Bar front matrix) */
#define MARKEE_WIDTH                 72
#define MARKEE_HEIGHT                16
#define MARKEE_CHANNELS              3
#define MARKEE_FRAME_BYTES           (MARKEE_WIDTH * MARKEE_HEIGHT * MARKEE_CHANNELS)
#define MARKEE_REFRESH_HZ            60

/* Target physical width of the 72x16 LED matrix on screen (inches) */
#define MARKEE_MATRIX_WIDTH_INCHES   6.35f
#define MARKEE_MM_PER_INCH           25.4f

/* Real BUSY Bar physical proportions (mm) used for plastic enclosure ratios:
 *   Body:   168.6 x 55.2 mm
 *   Matrix: 158.4 x 35.2 mm (72x16 @ 2.2 mm pitch)
 */
#define MARKEE_REF_MATRIX_W_MM       158.4f
#define MARKEE_REF_MATRIX_H_MM       35.2f
#define MARKEE_REF_BODY_W_MM         168.6f
#define MARKEE_REF_BODY_H_MM         55.2f

/* Fallback logical DPI if monitor EDID physical width is unavailable */
#define MARKEE_DEFAULT_DPI           110.0f
#define MARKEE_MIN_VALID_DPI         72.0f
#define MARKEE_MAX_VALID_DPI         240.0f

/* Embedded HTTP/TCP API server defaults */
#define MARKEE_DEFAULT_HOST          "127.0.0.1"
#define MARKEE_DEFAULT_PORT          8080
#define MARKEE_MAX_BODY_BYTES        (1024 * 1024) /* 1 MiB max upload */
#define MARKEE_MAX_HEADER_BYTES      16384
#define MARKEE_MAX_CLIENTS           32

#endif /* MARKEE_CONFIG_H */
