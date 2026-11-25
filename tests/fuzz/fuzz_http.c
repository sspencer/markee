#include <stddef.h>
#include <stdint.h>

#include "server.h"
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)server_fuzz_parse_http(data, size);
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(int argc, char **argv) {
    static const char *const SEEDS[] = {
        "GET /info HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "POST /fill?color=%23ff8800&r=255&g=128&b=0 HTTP/1.1\r\nContent-Length: 4\r\nConnection: close\r\n\r\ntest",
        "POST /draw?transition=fade&ttl=2.5 HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n0\r\n\r\n",
        "POST /alert/chime?volume=80 HTTP/1.1\r\nExpect: 100-continue\r\nAuthorization: Bearer secret\r\nContent-Length: 0\r\n\r\n",
        "GET /events?since=1&wait=0.5&stream=1 HTTP/1.1\r\nAccept: text/event-stream\r\n\r\n",
    };
    return fuzz_standalone_main(argc, argv, SEEDS, sizeof(SEEDS) / sizeof(SEEDS[0]));
}
#endif
