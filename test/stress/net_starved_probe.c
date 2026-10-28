/* Gust: where a slice's bytes are, for the stress's report. */
#include <stdint.h>

typedef struct { const uint8_t *data; int64_t len; } starved_bytes;

int64_t starved_addr(starved_bytes s) { return (int64_t)(uintptr_t)s.data; }
