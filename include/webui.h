#ifndef MARKEE_WEBUI_H
#define MARKEE_WEBUI_H

#include <stddef.h>

/* Return the embedded single-page HTML/JS Web Playground served at GET /ui. */
const char *webui_html(size_t *out_len);

#endif /* MARKEE_WEBUI_H */
