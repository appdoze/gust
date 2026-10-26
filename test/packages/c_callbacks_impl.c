/* The C side of c_callbacks.goose: C calling back into Gust through the
   addresses c_callback gives, as a library walking its own data, the C
   library's qsort with a Gust comparator, and a call back that calls C
   again, which calls back once more (spec 7.11). */
#include <stdint.h>
#include <stdlib.h>

typedef struct { int64_t *data; int64_t len; } cb_i64s;

typedef int64_t (*cb_fold_fn)(int64_t acc, int64_t x, uint64_t user);

int64_t cb_fold(cb_i64s xs, uint64_t f, uint64_t user) {
    int64_t acc = 0;
    for (int64_t i = 0; i < xs.len; i++) acc = ((cb_fold_fn)(uintptr_t)f)(acc, xs.data[i], user);
    return acc;
}

/* The comparator's C type is qsort's: two pointers in, an int out, which a
   Gust export fn of two u64s and an i32 matches in every ABI it targets. */
void cb_sort(cb_i64s xs, uint64_t cmp) {
    qsort(xs.data, (size_t)xs.len, sizeof(int64_t),
          (int (*)(const void *, const void *))(uintptr_t)cmp);
}

typedef void (*cb_void_fn)(void);

void cb_run(uint64_t f) { ((cb_void_fn)(uintptr_t)f)(); }

typedef int64_t (*cb_step_fn)(int64_t x);

int64_t cb_call(uint64_t f, int64_t x) { return ((cb_step_fn)(uintptr_t)f)(x); }
