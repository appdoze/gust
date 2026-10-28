/* Gust: does a vectored handler that commits a reserved page and resumes
   keep the faulting code's AVX registers whole? The data stacks commit on
   fault that way (runtime_impl.h, gs_fault_filter), and net_serve.goose's
   buffer came back with 16 zero bytes in every 32 from a commit boundary
   on: 32-byte stores whose upper halves were lost. This copies with AVX2
   into a reserved region the handler commits 16 KB at a time, under load,
   and counts what came out wrong, with the handler as the runtime's
   (mode 0) and with the AVX state saved before its VirtualAlloc and
   restored after (mode 1). Modes 2 and 3 block for a moment before each
   copy, as a program waiting on its sockets does (a yield, and every 16th
   time a wait), so that the thread was switched out before its fault, and copy into a fresh 16 KB chunk each
   time: mode 2 with the loop below, mode 3 with the C library's memcpy.
   It also reports whether the context record the handler gets carries the
   AVX state.

       avx_commit_probe mode seconds */
#include <windows.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESERVE ((size_t)1 << 30)
#define PASS ((size_t)256 << 20)
#define PIECE 8192
#define SRC ((size_t)1 << 20)

static uint8_t *base;
static int mode;
static volatile LONG faults, no_xstate, xstate_init, xstate_avx;

__attribute__((target("xsave"))) static void save_avx(uint8_t *area) { _xsave64(area, 4); }
__attribute__((target("xsave"))) static void restore_avx(uint8_t *area) { _xrstor64(area, 4); }

static LONG WINAPI handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *hit = (uint8_t *)ep->ExceptionRecord->ExceptionInformation[1];
    if (hit < base || hit >= base + RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    DWORD64 mask = 0;
    if ((c->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE && GetXStateFeaturesMask(c, &mask))
        InterlockedIncrement((mask & XSTATE_MASK_AVX) ? &xstate_avx : &xstate_init);
    else
        InterlockedIncrement(&no_xstate);
    /* The standard XSAVE layout: legacy area, header at 512, AVX at 576. */
    __declspec(align(64)) uint8_t area[1024];
    if (mode == 1) {
        memset(area, 0, sizeof area);
        save_avx(area);
    }
    uint8_t *page = (uint8_t *)((uintptr_t)hit & ~(uintptr_t)4095);
    size_t n = 16384;
    if (page + n > base + RESERVE) n = (size_t)(base + RESERVE - page);
    if (!VirtualAlloc(page, n, MEM_COMMIT, PAGE_READWRITE)) ExitProcess(3);
    if (mode == 1) restore_avx(area);
    InterlockedIncrement(&faults);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Four 32-byte loads, then four stores, as a library memcpy does. */
__attribute__((target("avx2"), noinline)) static void copy_avx(uint8_t *d, const uint8_t *s, size_t n) {
    for (size_t i = 0; i + 128 <= n; i += 128) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(s + i + 32));
        __m256i e = _mm256_loadu_si256((const __m256i *)(s + i + 64));
        __m256i f = _mm256_loadu_si256((const __m256i *)(s + i + 96));
        _mm256_storeu_si256((__m256i *)(d + i), a);
        _mm256_storeu_si256((__m256i *)(d + i + 32), b);
        _mm256_storeu_si256((__m256i *)(d + i + 64), e);
        _mm256_storeu_si256((__m256i *)(d + i + 96), f);
    }
}

static volatile LONG stop;
static HANDLE idle;   /* never set: a wait of a millisecond */
static DWORD WINAPI burn(void *arg) {
    (void)arg;
    volatile uint64_t x = 1;
    while (!stop) x = x * 6364136223846793005ull + 1;
    return 0;
}

int main(int argc, char **argv) {
    mode = argc > 1 ? atoi(argv[1]) : 0;
    int seconds = argc > 2 ? atoi(argv[2]) : 30;
    base = (uint8_t *)VirtualAlloc(NULL, RESERVE, MEM_RESERVE, PAGE_READWRITE);
    uint8_t *src = (uint8_t *)malloc(SRC);
    if (!base || !src) return 2;
    for (size_t i = 0; i < SRC; i++) src[i] = (uint8_t)(i * 7 + i / 251 + 1);
    AddVectoredExceptionHandler(1, handler);
    HANDLE burner = CreateThread(NULL, 0, burn, NULL, 0, NULL);
    idle = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    long long pieces = 0, bad = 0, passes = 0;
    while (GetTickCount64() < end) {
        size_t step = mode >= 2 ? 16384 : PIECE;
        for (size_t off = 0; off < PASS && GetTickCount64() < end; off += step) {
            const uint8_t *s = src + (off % (SRC - PIECE));
            if (mode >= 2) {
                if (pieces % 16 == 0) WaitForSingleObject(idle, 1);
                else Sleep(0);
            }
            if (mode == 3) memcpy(base + off, s, PIECE);
            else copy_avx(base + off, s, PIECE);
            pieces++;
            if (memcmp(base + off, s, PIECE) != 0) {
                size_t k = 0;
                while (base[off + k] == s[k]) k++;
                if (bad < 3) {
                    printf("bad piece at %zu (+%zu into a 16 KB chunk): first wrong byte %zu:", off, off % 16384, k);
                    for (size_t j = k - k % 16; j < k - k % 16 + 64 && j < PIECE; j++)
                        printf("%s%02x", j % 16 ? " " : "\n   ", base[off + j]);
                    printf("\n");
                }
                bad++;
            }
        }
        VirtualFree(base, PASS, MEM_DECOMMIT);
        passes++;
    }
    stop = 1;
    WaitForSingleObject(burner, INFINITE);
    printf("mode %d: %lld passes, %lld pieces, %lld bad; %ld faults; the context record: no xstate %ld, "
           "xstate with AVX in init %ld, with AVX %ld\n",
           mode, passes, pieces, bad, faults, no_xstate, xstate_init, xstate_avx);
    return bad ? 1 : 0;
}
