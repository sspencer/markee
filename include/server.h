#ifndef MARKEE_SERVER_H
#define MARKEE_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "display.h"

typedef struct MarkeeServer MarkeeServer;

/* Start the embedded HTTP/TCP socket server on host:port (port 0 picks an ephemeral port).
 * Returns NULL on failure. */
MarkeeServer *server_start(MarkeeDisplay *disp, const char *host, uint16_t port);

/* Return the actual bound TCP port (useful when port=0 was passed). */
uint16_t server_get_port(const MarkeeServer *srv);

/* Gracefully stop the server and release resources. */
void server_stop(MarkeeServer *srv);

#endif /* MARKEE_SERVER_H */
