/* Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md. */
/* Gust: how the fault handler commits a data stack on Windows and macOS
   (gs_commit_size): a fresh region commits 16 KB at its first touch, then
   as much as it holds already, doubling up to 1 MB chunks; every commit
   mark is a whole number of pages; a first touch high up a region commits
   only around it; the last chunk stops at the region's end and the guard gap
   behind it stays uncommitted; contents never move. A system call writing
   into stack memory that user-mode code has written succeeds across chunk
   boundaries, which is what Gust gives C: everything it passes is
   initialized. Into pages never touched it fails on Windows and macOS
   rather than faulting them in (docs/implementation.md, "Data stacks").
   Linux commits on touch with no handler: there only the contents and the
   system calls are checked. */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#define GS_NEED_THREADS 1
#define GS_STACK_RESERVE (64u << 20)
#define GS_STACK_GAP (64u << 10)
#include "../../src/runtime/runtime.h"
#include "../../src/runtime/runtime_impl.h"
#include "../../src/runtime/runtime_threads.h"

#if defined(_WIN32) || defined(GS_POSIX_COMMIT_ON_FAULT)
#define COMMITS_ON_FAULT 1
#endif

static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "commit chunk check failed: %s\n", what); exit(2); }
}

static const gs_region *region_of(const uint8_t *base) {
    for (long i = 0; i < gs_nregions; i++)
        if (gs_regions[i].base == base) return &gs_regions[i];
    check(0, "region registered");
    return NULL;
}

#ifdef _WIN32
static int committed_page(const uint8_t *p) {
    MEMORY_BASIC_INFORMATION info;
    check(VirtualQuery(p, &info, sizeof info) != 0, "VirtualQuery");
    return info.State == MEM_COMMIT;
}
#endif

/* A file of n bytes, each its offset mod 251, for the system call checks. */
static const char *make_file(size_t n) {
    static char path[512];
#ifdef _WIN32
    char dir[MAX_PATH];
    check(GetTempPathA(sizeof dir, dir) > 0, "temp path");
    snprintf(path, sizeof path, "%sgust_commit_chunks_%lu.bin", dir, (unsigned long)GetCurrentProcessId());
#else
    snprintf(path, sizeof path, "/tmp/gust_commit_chunks_%ld.bin", (long)getpid());
#endif
    FILE *f = fopen(path, "wb");
    check(f != NULL, "create the input file");
    for (size_t i = 0; i < n; i++) fputc((int)(i % 251), f);
    fclose(f);
    return path;
}

/* Reads n bytes of the file into p with the system call itself, so that the
   kernel writes the buffer: whether all n arrived. */
static int sys_read(const char *path, uint8_t *p, size_t n) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    check(h != INVALID_HANDLE_VALUE, "open the input file");
    DWORD got = 0;
    BOOL ok = ReadFile(h, p, (DWORD)n, &got, NULL);
    CloseHandle(h);
    return ok && got == n;
#else
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "open the input file");
    ssize_t got = read(fd, p, n);
    close(fd);
    return got == (ssize_t)n;
#endif
}

int main(int argc, char **argv) {
    gs_rt_init(argc, argv, 3, 4, 0);
    const size_t K = 1024, M = 1024 * 1024;

    /* 1. The chunk sequence of a region grown from its base. */
    uint8_t *base = GS(0)->top;
    const gs_region *r = region_of(base);
#ifdef COMMITS_ON_FAULT
    check(r->committed == base, "a fresh region has nothing committed");
    size_t want[] = { 16 * K, 32 * K, 64 * K, 128 * K, 256 * K, 512 * K, 1 * M, 2 * M, 3 * M, 4 * M };
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        size_t at = (size_t)(r->committed - base);
        GS(0)->top = base + at + 1;
        base[at] = (uint8_t)i;
        check((size_t)(r->committed - base) == want[i], "chunk doubles from 16 KB to 1 MB, then stays");
        check((size_t)(r->committed - base) % gs_page_size == 0, "commit mark on a page boundary");
#ifdef _WIN32
        check(committed_page(base + want[i] - 1), "the whole chunk is committed");
        check(!committed_page(base + want[i]), "nothing past the chunk is");
#endif
    }
#else
    (void)r;
#endif
    /* Contents across every boundary, written and read back. */
    GS(0)->top = base + 6 * M;
    for (size_t i = 0; i < 6 * M; i++) base[i] = (uint8_t)(i % 253);
    for (size_t i = 0; i < 6 * M; i++) check(base[i] == (uint8_t)(i % 253), "contents survive growth");

    /* 2. A first touch high up a fresh region commits a chunk there only;
       touching its base afterwards commits from there, sized by the mark. */
    uint8_t *up = GS(1)->top;
    const gs_region *ru = region_of(up);
    GS(1)->top = up + 5 * M + 1;
    up[5 * M] = 42;
#ifdef COMMITS_ON_FAULT
    check(ru->committed == up + 5 * M + 16 * K, "a first touch high up commits 16 KB around it");
#ifdef _WIN32
    check(!committed_page(up), "the stretch below it stays uncommitted");
#endif
    up[0] = 43;
    check(ru->committed == up + 5 * M + 16 * K, "a touch below the mark leaves the mark");
#ifdef _WIN32
    check(committed_page(up) && committed_page(up + M - 1) && !committed_page(up + M),
          "the touch below commits a chunk sized by the mark, 1 MB");
#endif
#else
    (void)ru;
    up[0] = 43;
#endif
    check(up[0] == 43 && up[5 * M] == 42, "sparse contents survive");

    /* 3. The last chunk stops at the region's end; the guard gap stays
       reserved. A third fresh region, touched in its last page only. */
    uint8_t *end = GS(2)->top;
    const gs_region *re = region_of(end);
    GS(2)->top = end + re->usable;
    end[re->usable - 1] = 7;
#ifdef COMMITS_ON_FAULT
    check(re->committed == end + re->usable, "the last chunk is clamped to the region");
#endif
#ifdef _WIN32
    check(!committed_page(end + re->usable), "the guard gap is not committed");
#endif
    check(end[re->usable - 1] == 7, "the last byte holds");

    /* 4. Addresses never move: the bases are where they were. */
    check(GS(0)->top == base + 6 * M && region_of(base) == r, "regions stay in place");

    /* 5. The kernel writing into stack memory user-mode code initialized,
       across chunk boundaries: what Gust hands to C. */
    const char *path = make_file(3 * M);
    memset(base, 0, 3 * M);
    check(sys_read(path, base + 100, 3 * M - 100), "a system call writes into initialized stack memory");
    for (size_t i = 0; i < 3 * M - 100; i++) check(base[100 + i] == (uint8_t)(i % 251), "it wrote every byte");
    /* Into pages never touched (above the mark of a fresh stack), it fails
       on Windows and macOS rather than committing them, the rule runtime C
       and C code given Gust memory both follow. */
    gs_stack untouched; gs_stack_init(&untouched);
    uint8_t *u = untouched.top;
#ifdef COMMITS_ON_FAULT
    check(!sys_read(path, u + 64 * K, 4096), "a system call does not commit an untouched page");
#else
    check(sys_read(path, u + 64 * K, 4096), "Linux commits on the kernel's touch too");
#endif
    remove(path);

    /* 6. A thread program's end gives the address space back. */
    gs_thread_release();
    gs_release_regions();
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    check(VirtualQuery(base, &info, sizeof info) != 0 && info.State == MEM_FREE, "released region is free");
#endif
    puts("runtime commit chunks ok");
    return 0;
}
