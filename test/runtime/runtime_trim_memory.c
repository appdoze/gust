/* Direct runtime checks: actually decommit unused Windows pages, drop them
   from Linux's resident set, and on macOS mark them reusable and declare
   them reused as growth reopens them; keep live partial pages and other
   stacks, and commit again through the fault handler (Windows and macOS)
   behind a commit mark that follows each trim. This runs beside the Goose
   fixtures in both CI profiles. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#define GS_NEED_THREADS 1
#define GS_STACK_RESERVE (64u << 20)
#define GS_STACK_GAP (64u << 10)
#include "../../src/runtime/runtime.h"
#include "../../src/runtime/runtime_impl.h"
#include "../../src/runtime/runtime_threads.h"

static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "trim memory check failed: %s\n", what); exit(2); }
}

/* The registry entry of the region starting at base. */
static const gs_region *region_of(const uint8_t *base) {
    for (long i = 0; i < gs_nregions; i++)
        if (gs_regions[i].base == base) return &gs_regions[i];
    check(0, "region registered");
    return NULL;
}

#ifdef __linux__
/* Whether the page holding p is resident. */
static int resident(const uint8_t *p) {
    unsigned char v = 0;
    uintptr_t page = (uintptr_t)p / gs_page_size * gs_page_size;
    check(mincore((void *)page, gs_page_size, &v) == 0, "mincore");
    return v & 1;
}
#endif

int main(int argc, char **argv) {
    gs_rt_init(argc, argv, 2, 3, 0);
    uint8_t *base = GS(0)->top;
    /* The platform may have halved the reservation (gs_reserve_region). */
    const gs_region *r0 = region_of(base);
    uint8_t *other = GS(1)->top;
    gs_stack global; gs_stack_init(&global);
    uint8_t *global_base = global.top;
    global.top += 8197; memset(global_base, 37, 8197);
    GS(1)->top += 8197; memset(other, 61, 8197);
    size_t keeps[] = { 0, 1, 4095, 4096, 4097, 8197 };
    for (size_t k = 0; k < sizeof keeps / sizeof *keeps; k++) {
        size_t count = 16u << 20, keep = keeps[k];
        GS(0)->top = base + count;
        memset(base, 85, count);
        check(gs_trim_memory(), "trim with a live large value");
        check(base[0] == 85 && base[count - 1] == 85, "large live value survives");
        GS(0)->top = base + keep;
        check(gs_trim_memory(), "trim after shrink");
        check(gs_trim_memory(), "repeat trim is harmless");
        size_t start = (keep + gs_page_size - 1) / gs_page_size * gs_page_size;
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info;
        check(VirtualQuery(base + start, &info, sizeof info) != 0, "query discarded page");
        check(info.State == MEM_RESERVE, "unused page really decommitted");
        check(VirtualQuery(base + count - 1, &info, sizeof info) != 0 &&
              info.State == MEM_RESERVE, "last used page decommitted");
        check(VirtualQuery(base + r0->usable, &info, sizeof info) != 0 &&
              info.State == MEM_RESERVE, "guard gap remains uncommitted");
#endif
#if defined(_WIN32) || defined(GS_POSIX_COMMIT_ON_FAULT)
        /* The next trim starts its walk from here, not the reservation's end. */
        check(r0->committed == base + start, "commit mark follows the trim");
#else
        (void)r0;
#endif
#ifdef GS_POSIX_COMMIT_ON_FAULT
        check(r0->reuse_lo == base + start && r0->reuse_hi >= base + count,
              "trimmed pages are recorded as reusable");
#endif
#ifdef __linux__
        check(!resident(base + start), "unused page leaves the resident set");
        check(!resident(base + count - 1), "last used page leaves the resident set");
#endif
        for (size_t i = 0; i < keep; i++) check(base[i] == 85, "live prefix survives");
        for (size_t i = 0; i < 8197; i++) {
            check(other[i] == 61, "other local stack survives");
            check(global_base[i] == 37, "global stack survives");
        }
        GS(0)->top = base + count;
        memset(base + keep, 93, count - keep);
        for (size_t i = keep; i < count; i++) check(base[i] == 93, "regrowth recommits");
#if defined(_WIN32) || defined(GS_POSIX_COMMIT_ON_FAULT)
        check(r0->committed >= base + count, "fault handler raises the commit mark");
#endif
#ifdef GS_POSIX_COMMIT_ON_FAULT
        /* Every page growth took back was declared reused (MADV_FREE_REUSE). */
        check(!r0->reuse_hi || r0->reuse_lo >= base + count, "regrowth declares pages reused");
#endif
    }
    /* Growth that skips pages leaves a stretch below the commit mark that
       was never committed (or, on macOS, opened); a trim over it works. */
    GS(0)->top = base + 1;
    check(gs_trim_memory(), "trim before skipping pages");
    GS(0)->top = base + (8u << 20);
    base[(8u << 20) - 1] = 7;
    GS(0)->top = base + 1;
    check(gs_trim_memory(), "trim over skipped pages");
    check(base[0] == 85, "live byte survives a trim over skipped pages");
#if defined(_WIN32) || defined(GS_POSIX_COMMIT_ON_FAULT)
    check(r0->committed == base + gs_page_size, "skipped pages trimmed to the mark");
#endif
    /* As a thread program's end does: its stack block, then its regions. */
    gs_thread_release();
    gs_release_regions();
    puts("runtime trim memory ok");
    return 0;
}
