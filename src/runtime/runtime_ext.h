/* Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md. */
/* Goose runtime: extern-fn support, spliced in after the generated type
   declarations, since it is written against them: sl_u8 (a u8 slice: data,
   len) and gs_rref (a reference to a resizable: its header and its data
   stack). The runtime object has no generated types, and lays out the ones
   it uses here as codegen does (CodeGen::EmitCoreTypes, CT). An --include
   header follows this and may use what it defines. The generated program
   calls the OS primitives behind stdlib/os.goose (spec §7.10, defined in
   runtime_os.h) and std's byte search directly from `extern "gs_..." fn`
   declarations; no prototype is emitted for a function declared here. */

#ifdef GS_RUNTIME_OBJECT
#pragma pack(push, 1)
typedef struct { uint8_t *base; int64_t len; } gs_rhdr;
typedef struct { gs_rhdr *hdr; gs_stack *stk; } gs_rref;
typedef struct { uint8_t *data; int64_t len; } sl_u8;
typedef struct { int64_t *data; int64_t len; } sl_i64;   /* Gust: for net's wait */
#pragma pack(pop)
#endif

/* Appends n bytes to the u8[>..] a builder reference points at: the
   resizable's elements top its stack, so the bytes go at the stack top and
   the header's count grows. */
static void gs_bld_append(gs_rref b, const void *p, int64_t n) {
    if (n <= 0) return;
    GS_TOUCHED(b.stk->top, n);   /* runtime.h: a vector copy faults nowhere */
    memcpy(b.stk->top, p, (size_t)n);
    b.stk->top += n;
    b.hdr->len += n;
}

/* ffi's cstr and bytes (stdlib/ffi.goose): the bytes at a pointer a C
   library handed back as a u64, copied into a builder, so nothing foreign
   is a Goose value without a copy. The pointer is trusted as every extern
   call trusts C; a null one appends nothing and is the false result. */
static uint8_t gs_ffi_cstr(uint64_t p, gs_rref out) {
    const char *s = (const char *)(uintptr_t)p;
    if (!s) return 0;
    gs_bld_append(out, s, (int64_t)strlen(s));
    return 1;
}
static uint8_t gs_ffi_bytes(uint64_t p, int64_t n, gs_rref out) {
    if (!p) return 0;
    gs_bld_append(out, (const void *)(uintptr_t)p, n);
    return 1;
}

/* math's sqrt (stdlib/math.goose). Goose has no errno, and C's sqrt has to
   set it for a negative argument: clang and gcc then guard the square root
   instruction with a test and a library call at every use, and will not
   vectorize a loop around one. A compiler that has a square root without
   errno uses that instead; the value is the same correctly rounded root
   either way, and NaN for a negative argument. */
#if defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_sqrt)
#define GS_SQRT_NO_ERRNO 1
#endif
#endif
#ifdef GS_SQRT_NO_ERRNO
static double gs_sqrt(double x) { return __builtin_elementwise_sqrt(x); }
static float gs_sqrtf(float x) { return __builtin_elementwise_sqrt(x); }
#elif defined(__MIRC__)
/* c2mir: sqrt and sqrtf themselves, each call of which src/jit_mir.h makes
   MIR's square root instruction. A call of a function around them stayed a
   call once MIR's inliner had spent what it lets a long function grow by,
   and on x86-64 SysV, where no float register survives a call, n-body's
   inner loop then stored and reloaded each live double around every one. */
#define gs_sqrt sqrt
#define gs_sqrtf sqrtf
#elif (defined(__GNUC__) || defined(_MSC_VER)) && (defined(__x86_64__) || defined(_M_X64)) && \
    !defined(__TINYC__)
/* Gust: gcc and MSVC on x86-64 have SSE2's square root, which sets no
   errno: the instruction alone, and no call into libm, which Linux then
   need not load (gs_fmod, runtime.h). */
#include <emmintrin.h>
static double gs_sqrt(double x) { return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_set_sd(x), _mm_set_sd(x))); }
static float gs_sqrtf(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
#else
static double gs_sqrt(double x) { return sqrt(x); }
static float gs_sqrtf(float x) { return sqrtf(x); }
#endif

/* Gust: math's floor, ceil, trunc and round (stdlib/math.goose) without
   libm, for the same reason. Where the target rounds to an integer in one
   instruction (SSE4.1, arm64) the compiler's builtin is that instruction.
   Elsewhere, below 2^52 in magnitude (2^23 for f32) a float's truncation is
   one conversion to an integer and back, adjusted by one for floor, ceil
   and round; from there on every float is an integer already, and NaN and
   the infinities are their own results. A zero result has x's sign, as C's
   does. round takes halfway cases away from zero, x - trunc(x) being exact. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__) && !defined(__MIRC__) && \
    (defined(__SSE4_1__) || defined(__aarch64__))
static double gs_floor(double x) { return __builtin_floor(x); }
static double gs_ceil(double x) { return __builtin_ceil(x); }
static double gs_trunc(double x) { return __builtin_trunc(x); }
static double gs_round(double x) { return __builtin_round(x); }
static float gs_floorf(float x) { return __builtin_floorf(x); }
static float gs_ceilf(float x) { return __builtin_ceilf(x); }
static float gs_truncf(float x) { return __builtin_truncf(x); }
static float gs_roundf(float x) { return __builtin_roundf(x); }
#else
#define GS_ROUNDING(name, T, I, LIMIT, ADJUST)                                \
    static T name(T x) {                                                      \
        if (!(x > -(LIMIT) && x < (LIMIT))) return x;                         \
        T t = (T)(I)x;                                                        \
        ADJUST                                                                \
        return t == 0 ? x * (T)0 : t;                                         \
    }
#define GS_FLOOR_ADJ if (t > x) t -= 1;
#define GS_CEIL_ADJ if (t < x) t += 1;
#define GS_ROUND_ADJ if (x - t >= 0.5) t += 1; else if (x - t <= -0.5) t -= 1;
GS_ROUNDING(gs_floor, double, int64_t, 4503599627370496.0, GS_FLOOR_ADJ)
GS_ROUNDING(gs_ceil, double, int64_t, 4503599627370496.0, GS_CEIL_ADJ)
GS_ROUNDING(gs_trunc, double, int64_t, 4503599627370496.0, )
GS_ROUNDING(gs_round, double, int64_t, 4503599627370496.0, GS_ROUND_ADJ)
GS_ROUNDING(gs_floorf, float, int32_t, 8388608.0f, GS_FLOOR_ADJ)
GS_ROUNDING(gs_ceilf, float, int32_t, 8388608.0f, GS_CEIL_ADJ)
GS_ROUNDING(gs_truncf, float, int32_t, 8388608.0f, )
GS_ROUNDING(gs_roundf, float, int32_t, 8388608.0f, GS_ROUND_ADJ)
#endif

/* Gust: four doubles side by side, one per lane of a loop the compiler runs
   four iterations at a time (src/codegen_lanes.h), and a condition per lane
   (gs_l4m), all ones where it holds and none where not: which lanes a
   branch stores into. The lanes' C only ever operates on them through the
   macros below, so that each compiler gets its own: clang's vector types,
   or under MSVC and gcc on x86-64 a pair of SSE2 registers each and
   functions inlined into the block of lanes (gcc 15 compiles the vector
   types into code several times slower than the loop one iteration at a
   time: n-body 1.78 s against 0.17 s). Each lane's operations are its
   iteration's, rounded as they are. TinyCC and MIR have neither, and
   there GS_LANES stays undefined and the loops run one iteration at a
   time; -DGS_NO_LANES asks for that anywhere. */
#if defined(__clang__) && !defined(__MIRC__) && !defined(GS_NO_LANES)
#define GS_LANES 4
typedef double gs_l4d __attribute__((vector_size(32)));
typedef long long gs_l4m __attribute__((vector_size(32)));
/* Macros, not functions: a function that takes or returns a vector has an
   ABI that differs with AVX on x86-64 SysV, which clang refuses between a
   function's AVX2 version and the baseline (an error, not just gcc's note),
   though an inlined one passes nothing. */
#define gs_l4d_splat(x)                                                       \
    __extension__({                                                           \
        double gs_l4d_s_ = (x);                                               \
        (gs_l4d){ gs_l4d_s_, gs_l4d_s_, gs_l4d_s_, gs_l4d_s_ };               \
    })
#define gs_l4m_splat(x)                                                       \
    __extension__({                                                           \
        long long gs_l4m_s_ = (x);                                            \
        (gs_l4m){ gs_l4m_s_, gs_l4m_s_, gs_l4m_s_, gs_l4m_s_ };               \
    })
/* A uniform double as an operand: the vector types spread it themselves. */
#define gs_l4d_uni(x) ((double)(x))
#define gs_l4d_make(a, b, c, d) ((gs_l4d){ (a), (b), (c), (d) })
#define gs_l4d_get(v, l) ((v)[l])
#define gs_l4d_add(a, b) ((a) + (b))
#define gs_l4d_sub(a, b) ((a) - (b))
#define gs_l4d_mul(a, b) ((a) * (b))
#define gs_l4d_div(a, b) ((a) / (b))
#define gs_l4d_neg(a) (-(a))
#define gs_l4d_lt(a, b) ((gs_l4m)((a) < (b)))
#define gs_l4d_gt(a, b) ((gs_l4m)((a) > (b)))
#define gs_l4d_le(a, b) ((gs_l4m)((a) <= (b)))
#define gs_l4d_ge(a, b) ((gs_l4m)((a) >= (b)))
#define gs_l4d_eq(a, b) ((gs_l4m)((a) == (b)))
#define gs_l4d_ne(a, b) ((gs_l4m)((a) != (b)))
#define gs_l4m_and(a, b) ((a) & (b))
#define gs_l4m_or(a, b) ((a) | (b))
#define gs_l4m_not(a) (~(a))
/* Each lane's a where m holds there, its b where not. */
#define gs_l4d_blend(m, a, b) ((gs_l4d)(((gs_l4m)(a) & (m)) | ((gs_l4m)(b) & ~(m))))
/* Whether a condition holds in any lane. */
#define gs_l4m_any(m)                                                         \
    __extension__({                                                           \
        gs_l4m gs_l4m_a_ = (m);                                               \
        (gs_l4m_a_[0] | gs_l4m_a_[1] | gs_l4m_a_[2] | gs_l4m_a_[3]) != 0;     \
    })
#ifdef GS_SQRT_NO_ERRNO
#define gs_l4d_sqrt(v) __builtin_elementwise_sqrt(v)
#else
#define gs_l4d_sqrt(v)                                                                   \
    __extension__({                                                                      \
        gs_l4d gs_l4d_v_ = (v);                                                          \
        (gs_l4d){ gs_sqrt(gs_l4d_v_[0]), gs_sqrt(gs_l4d_v_[1]), gs_sqrt(gs_l4d_v_[2]),   \
                  gs_sqrt(gs_l4d_v_[3]) };                                               \
    })
#endif
#elif ((defined(_MSC_VER) && defined(_M_X64)) || (defined(__GNUC__) && defined(__SSE2__))) && \
    !defined(__clang__) && !defined(__TINYC__) && !defined(__MIRC__) && !defined(GS_NO_LANES)
#define GS_LANES 4
#include <emmintrin.h>
#ifdef _MSC_VER
#define GS_L4_INLINE static __forceinline
#else
#define GS_L4_INLINE static inline __attribute__((always_inline))
#endif
/* Each macro names the implementation GS_L4_ISA says: SSE2's (_sse), and
   under gcc AVX's (_avx) in a simd version's AVX2 and AVX-512 bodies, which
   CodeGen::EmitSimdVersions brackets with GS_L4_WIDE and GS_L4_BASE. */
#define GS_L4_CAT2(a, b) a##b
#define GS_L4_CAT(a, b) GS_L4_CAT2(a, b)
#define GS_L4_BASE _sse
#define GS_L4_ISA GS_L4_BASE
#define gs_l4d GS_L4_CAT(gs_l4x_d, GS_L4_ISA)
#define gs_l4m GS_L4_CAT(gs_l4x_m, GS_L4_ISA)
#define gs_l4d_splat GS_L4_CAT(gs_l4x_splat, GS_L4_ISA)
#define gs_l4m_splat GS_L4_CAT(gs_l4x_msplat, GS_L4_ISA)
#define gs_l4d_uni(x) gs_l4d_splat((double)(x))
#define gs_l4d_make GS_L4_CAT(gs_l4x_make, GS_L4_ISA)
#define gs_l4d_get GS_L4_CAT(gs_l4x_get, GS_L4_ISA)
#define gs_l4d_add GS_L4_CAT(gs_l4x_add, GS_L4_ISA)
#define gs_l4d_sub GS_L4_CAT(gs_l4x_sub, GS_L4_ISA)
#define gs_l4d_mul GS_L4_CAT(gs_l4x_mul, GS_L4_ISA)
#define gs_l4d_div GS_L4_CAT(gs_l4x_div, GS_L4_ISA)
#define gs_l4d_neg GS_L4_CAT(gs_l4x_neg, GS_L4_ISA)
#define gs_l4d_lt GS_L4_CAT(gs_l4x_lt, GS_L4_ISA)
#define gs_l4d_gt GS_L4_CAT(gs_l4x_gt, GS_L4_ISA)
#define gs_l4d_le GS_L4_CAT(gs_l4x_le, GS_L4_ISA)
#define gs_l4d_ge GS_L4_CAT(gs_l4x_ge, GS_L4_ISA)
#define gs_l4d_eq GS_L4_CAT(gs_l4x_eq, GS_L4_ISA)
#define gs_l4d_ne GS_L4_CAT(gs_l4x_ne, GS_L4_ISA)
#define gs_l4m_and GS_L4_CAT(gs_l4x_and, GS_L4_ISA)
#define gs_l4m_or GS_L4_CAT(gs_l4x_or, GS_L4_ISA)
#define gs_l4m_not GS_L4_CAT(gs_l4x_not, GS_L4_ISA)
#define gs_l4d_blend GS_L4_CAT(gs_l4x_blend, GS_L4_ISA)
#define gs_l4m_any GS_L4_CAT(gs_l4x_any, GS_L4_ISA)
#define gs_l4d_sqrt GS_L4_CAT(gs_l4x_sqrt, GS_L4_ISA)
/* SSE2: lanes 0 and 1 in lo, 2 and 3 in hi. Every operation is SSE2's,
   which rounds each lane as the scalar instruction does; a comparison's
   unordered lanes are false but for !=, as in C. */
typedef struct { __m128d lo, hi; } gs_l4x_d_sse;
typedef struct { __m128d lo, hi; } gs_l4x_m_sse;
GS_L4_INLINE gs_l4x_d_sse gs_l4x_splat_sse(double x) {
    gs_l4x_d_sse r;
    r.lo = r.hi = _mm_set1_pd(x);
    return r;
}
GS_L4_INLINE gs_l4x_m_sse gs_l4x_msplat_sse(long long x) {
    gs_l4x_m_sse r;
    r.lo = r.hi = _mm_castsi128_pd(_mm_set1_epi64x(x));
    return r;
}
GS_L4_INLINE gs_l4x_d_sse gs_l4x_make_sse(double a, double b, double c, double d) {
    gs_l4x_d_sse r;
    r.lo = _mm_setr_pd(a, b);
    r.hi = _mm_setr_pd(c, d);
    return r;
}
GS_L4_INLINE double gs_l4x_get_sse(gs_l4x_d_sse v, int l) {
    __m128d h = l < 2 ? v.lo : v.hi;
    return _mm_cvtsd_f64(l & 1 ? _mm_unpackhi_pd(h, h) : h);
}
#define GS_L4_OP(name, t, u, op)                                              \
    GS_L4_INLINE t name(u a, u b) {                                           \
        t r;                                                                  \
        r.lo = op(a.lo, b.lo);                                                \
        r.hi = op(a.hi, b.hi);                                                \
        return r;                                                             \
    }
GS_L4_OP(gs_l4x_add_sse, gs_l4x_d_sse, gs_l4x_d_sse, _mm_add_pd)
GS_L4_OP(gs_l4x_sub_sse, gs_l4x_d_sse, gs_l4x_d_sse, _mm_sub_pd)
GS_L4_OP(gs_l4x_mul_sse, gs_l4x_d_sse, gs_l4x_d_sse, _mm_mul_pd)
GS_L4_OP(gs_l4x_div_sse, gs_l4x_d_sse, gs_l4x_d_sse, _mm_div_pd)
GS_L4_OP(gs_l4x_lt_sse, gs_l4x_m_sse, gs_l4x_d_sse, _mm_cmplt_pd)
GS_L4_OP(gs_l4x_gt_sse, gs_l4x_m_sse, gs_l4x_d_sse, _mm_cmpgt_pd)
GS_L4_OP(gs_l4x_le_sse, gs_l4x_m_sse, gs_l4x_d_sse, _mm_cmple_pd)
GS_L4_OP(gs_l4x_ge_sse, gs_l4x_m_sse, gs_l4x_d_sse, _mm_cmpge_pd)
GS_L4_OP(gs_l4x_eq_sse, gs_l4x_m_sse, gs_l4x_d_sse, _mm_cmpeq_pd)
GS_L4_OP(gs_l4x_ne_sse, gs_l4x_m_sse, gs_l4x_d_sse, _mm_cmpneq_pd)
GS_L4_OP(gs_l4x_and_sse, gs_l4x_m_sse, gs_l4x_m_sse, _mm_and_pd)
GS_L4_OP(gs_l4x_or_sse, gs_l4x_m_sse, gs_l4x_m_sse, _mm_or_pd)
GS_L4_INLINE gs_l4x_d_sse gs_l4x_neg_sse(gs_l4x_d_sse a) {
    __m128d s = _mm_set1_pd(-0.0);
    gs_l4x_d_sse r;
    r.lo = _mm_xor_pd(a.lo, s);
    r.hi = _mm_xor_pd(a.hi, s);
    return r;
}
GS_L4_INLINE gs_l4x_m_sse gs_l4x_not_sse(gs_l4x_m_sse a) {
    __m128d s = _mm_castsi128_pd(_mm_set1_epi64x(-1));
    gs_l4x_m_sse r;
    r.lo = _mm_xor_pd(a.lo, s);
    r.hi = _mm_xor_pd(a.hi, s);
    return r;
}
GS_L4_INLINE gs_l4x_d_sse gs_l4x_blend_sse(gs_l4x_m_sse m, gs_l4x_d_sse a, gs_l4x_d_sse b) {
    gs_l4x_d_sse r;
    r.lo = _mm_or_pd(_mm_and_pd(m.lo, a.lo), _mm_andnot_pd(m.lo, b.lo));
    r.hi = _mm_or_pd(_mm_and_pd(m.hi, a.hi), _mm_andnot_pd(m.hi, b.hi));
    return r;
}
GS_L4_INLINE int gs_l4x_any_sse(gs_l4x_m_sse m) { return _mm_movemask_pd(_mm_or_pd(m.lo, m.hi)) != 0; }
/* SSE2's square root, without errno: the correctly rounded root, and NaN
   for a negative argument, as gs_sqrt's. */
GS_L4_INLINE gs_l4x_d_sse gs_l4x_sqrt_sse(gs_l4x_d_sse a) {
    gs_l4x_d_sse r;
    r.lo = _mm_sqrt_pd(a.lo);
    r.hi = _mm_sqrt_pd(a.hi);
    return r;
}
#if defined(__GNUC__) && GS_SIMD >= 1
/* AVX: one 256-bit register per lane vector, for gcc's simd versions,
   whose targets include the helpers' own (AVX2). The same operations lane
   by lane, left unfused by the contraction pragma (runtime.h). */
#include <immintrin.h>
#define GS_L4_WIDE _avx
#define GS_L4A_INLINE static inline __attribute__((always_inline, target("avx2")))
typedef __m256d gs_l4x_d_avx;
typedef __m256d gs_l4x_m_avx;
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_splat_avx(double x) { return _mm256_set1_pd(x); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_msplat_avx(long long x) { return _mm256_castsi256_pd(_mm256_set1_epi64x(x)); }
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_make_avx(double a, double b, double c, double d) {
    return _mm256_setr_pd(a, b, c, d);
}
GS_L4A_INLINE double gs_l4x_get_avx(gs_l4x_d_avx v, int l) {
    __m128d h = l < 2 ? _mm256_castpd256_pd128(v) : _mm256_extractf128_pd(v, 1);
    return _mm_cvtsd_f64(l & 1 ? _mm_unpackhi_pd(h, h) : h);
}
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_add_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_add_pd(a, b); }
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_sub_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_sub_pd(a, b); }
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_mul_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_mul_pd(a, b); }
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_div_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_div_pd(a, b); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_lt_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_cmp_pd(a, b, _CMP_LT_OQ); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_gt_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_cmp_pd(a, b, _CMP_GT_OQ); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_le_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_cmp_pd(a, b, _CMP_LE_OQ); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_ge_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_cmp_pd(a, b, _CMP_GE_OQ); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_eq_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_cmp_pd(a, b, _CMP_EQ_OQ); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_ne_avx(gs_l4x_d_avx a, gs_l4x_d_avx b) { return _mm256_cmp_pd(a, b, _CMP_NEQ_UQ); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_and_avx(gs_l4x_m_avx a, gs_l4x_m_avx b) { return _mm256_and_pd(a, b); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_or_avx(gs_l4x_m_avx a, gs_l4x_m_avx b) { return _mm256_or_pd(a, b); }
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_neg_avx(gs_l4x_d_avx a) { return _mm256_xor_pd(a, _mm256_set1_pd(-0.0)); }
GS_L4A_INLINE gs_l4x_m_avx gs_l4x_not_avx(gs_l4x_m_avx a) {
    return _mm256_xor_pd(a, _mm256_castsi256_pd(_mm256_set1_epi64x(-1)));
}
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_blend_avx(gs_l4x_m_avx m, gs_l4x_d_avx a, gs_l4x_d_avx b) {
    return _mm256_or_pd(_mm256_and_pd(m, a), _mm256_andnot_pd(m, b));
}
GS_L4A_INLINE int gs_l4x_any_avx(gs_l4x_m_avx m) { return _mm256_movemask_pd(m) != 0; }
GS_L4A_INLINE gs_l4x_d_avx gs_l4x_sqrt_avx(gs_l4x_d_avx a) { return _mm256_sqrt_pd(a); }
#else
#define GS_L4_WIDE _sse
#endif
#endif

/* math's mul_add (stdlib/math.goose): x * y + z, fused into one rounding
   where the code is built for FMA, as two roundings elsewhere
   (GS_MUL_ADD_MODE, runtime.h). A call names the macro, which picks the
   mode where the call is written: a simd function's AVX2 and AVX-512
   versions fuse whatever the target. A function value is resolved at
   compile time, so its calls name the macro too. The function of the same
   name, in the target's mode, is only for C that names it without the
   macro. The fused form is inlined into its caller, so that it is built
   with the caller's FMA. */
static inline double gs_mul_add_separate(double x, double y, double z) { return x * y + z; }
static inline float gs_mul_addf_separate(float x, float y, float z) { return x * y + z; }
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)
static inline __attribute__((always_inline)) double gs_mul_add_fused(double x, double y, double z) {
    return __builtin_fma(x, y, z);
}
static inline __attribute__((always_inline)) float gs_mul_addf_fused(float x, float y, float z) {
    return __builtin_fmaf(x, y, z);
}
#else
static inline double gs_mul_add_fused(double x, double y, double z) { return fma(x, y, z); }
static inline float gs_mul_addf_fused(float x, float y, float z) { return fmaf(x, y, z); }
#endif
#define GS_MUL_ADD_CAT2(a, b) a##b
#define GS_MUL_ADD_CAT(a, b) GS_MUL_ADD_CAT2(a, b)
static double gs_mul_add(double x, double y, double z) {
    return GS_MUL_ADD_CAT(gs_mul_add_, GS_MUL_ADD_BASE)(x, y, z);
}
static float gs_mul_addf(float x, float y, float z) {
    return GS_MUL_ADD_CAT(gs_mul_addf_, GS_MUL_ADD_BASE)(x, y, z);
}
#define gs_mul_add(x, y, z) GS_MUL_ADD_CAT(gs_mul_add_, GS_MUL_ADD_MODE)(x, y, z)
#define gs_mul_addf(x, y, z) GS_MUL_ADD_CAT(gs_mul_addf_, GS_MUL_ADD_MODE)(x, y, z)

/* math's other libm functions (stdlib/math.goose). C has them set errno on a
   domain or range error, so a C compiler takes each call for a write to
   memory: a loop around one loads again after it whatever it had hoisted
   out or kept in registers. Goose has no errno, so where the compiler has
   asm labels and the const attribute each is libm's own function declared
   under a name of its own as depending on its arguments alone; elsewhere
   the name forwards to libm. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)
#define GS_LIBM_SYM2(p, f) #p #f
#define GS_LIBM_SYM(p, f) GS_LIBM_SYM2(p, f)
#define GS_LIBM
#define GS_LIBM1(f) __asm__(GS_LIBM_SYM(__USER_LABEL_PREFIX__, f)) __attribute__((const));
#define GS_LIBM2(f) GS_LIBM1(f)
#else
#define GS_LIBM static
#define GS_LIBM1(f) { return f(x); }
#define GS_LIBM2(f) { return f(x, y); }
#endif
GS_LIBM double gs_sin(double x) GS_LIBM1(sin)
GS_LIBM double gs_cos(double x) GS_LIBM1(cos)
GS_LIBM double gs_tan(double x) GS_LIBM1(tan)
GS_LIBM double gs_asin(double x) GS_LIBM1(asin)
GS_LIBM double gs_acos(double x) GS_LIBM1(acos)
GS_LIBM double gs_atan(double x) GS_LIBM1(atan)
GS_LIBM double gs_atan2(double x, double y) GS_LIBM2(atan2)
GS_LIBM double gs_exp(double x) GS_LIBM1(exp)
GS_LIBM double gs_log(double x) GS_LIBM1(log)
GS_LIBM double gs_log2(double x) GS_LIBM1(log2)
GS_LIBM double gs_log10(double x) GS_LIBM1(log10)
GS_LIBM double gs_pow(double x, double y) GS_LIBM2(pow)
GS_LIBM float gs_sinf(float x) GS_LIBM1(sinf)
GS_LIBM float gs_cosf(float x) GS_LIBM1(cosf)
GS_LIBM float gs_tanf(float x) GS_LIBM1(tanf)
GS_LIBM float gs_asinf(float x) GS_LIBM1(asinf)
GS_LIBM float gs_acosf(float x) GS_LIBM1(acosf)
GS_LIBM float gs_atanf(float x) GS_LIBM1(atanf)
GS_LIBM float gs_atan2f(float x, float y) GS_LIBM2(atan2f)
GS_LIBM float gs_expf(float x) GS_LIBM1(expf)
GS_LIBM float gs_logf(float x) GS_LIBM1(logf)
GS_LIBM float gs_log2f(float x) GS_LIBM1(log2f)
GS_LIBM float gs_log10f(float x) GS_LIBM1(log10f)
GS_LIBM float gs_powf(float x, float y) GS_LIBM2(powf)

/* std's find_any and find_pair over a slice (gs_scan_any, gs_scan_pair); a
   set is a pointer to std's ByteSet. */
static int64_t gs_find_any(sl_u8 s, const void *set) { return gs_scan_any(s.data, s.len, set); }
static int64_t gs_find_pair(sl_u8 s, const void *a, int64_t d, const void *b) {
    return gs_scan_pair(s.data, s.len, a, d, b);
}

/* std's popcount, clz and ctz (64 for zero): the instruction where the C
   compiler has one, and counting under TinyCC. MSVC's popcount is an
   instruction past x86-64's baseline, which /arch:AVX and up promise. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
static int64_t gs_popcount(uint64_t x) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)
    return __builtin_popcountll(x);
#elif defined(_MSC_VER) && defined(_M_X64) && defined(__AVX__)
    return (int64_t)__popcnt64(x);
#else
    int64_t n = 0;
    while (x) {
        x &= x - 1;
        n++;
    }
    return n;
#endif
}
static int64_t gs_clz(uint64_t x) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)
    return x ? __builtin_clzll(x) : 64;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long i;
    return _BitScanReverse64(&i, x) ? 63 - (int64_t)i : 64;
#else
    int64_t n = 0;
    if (!x) return 64;
    while (!(x >> 63)) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}
static int64_t gs_ctz(uint64_t x) {
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)
    return x ? __builtin_ctzll(x) : 64;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long i;
    return _BitScanForward64(&i, x) ? (int64_t)i : 64;
#else
    int64_t n = 0;
    if (!x) return 64;
    while (!(x & 1)) {
        x >>= 1;
        n++;
    }
    return n;
#endif
}

GS_API uint8_t gs_os_read_file(sl_u8 path, gs_rref out);
GS_API uint8_t gs_os_write_file(sl_u8 path, sl_u8 data);
GS_API uint8_t gs_os_write_file_atomic(sl_u8 path, sl_u8 data);
GS_API uint8_t gs_os_append_file(sl_u8 path, sl_u8 data);
GS_API uint8_t gs_os_file_exists(sl_u8 path);
GS_API uint8_t gs_os_remove_file(sl_u8 path);
GS_API uint8_t gs_os_rename_file(sl_u8 from, sl_u8 to);
GS_API uint8_t gs_os_make_dir(sl_u8 path);
GS_API uint8_t gs_os_is_dir(sl_u8 path);
GS_API uint8_t gs_os_remove_dir(sl_u8 path);
GS_API uint8_t gs_os_list_dir(sl_u8 path, gs_rref out);
GS_API void gs_os_write_stdout(sl_u8 s);
GS_API void gs_os_write_stderr(sl_u8 s);
GS_API void gs_os_flush_stdout(void);
GS_API uint8_t gs_os_read_line(gs_rref out);
GS_API void gs_os_read_stdin(gs_rref out);
GS_API int64_t gs_os_arg_count(void);
GS_API void gs_os_arg(int64_t i, gs_rref out);
GS_API uint8_t gs_os_getenv(sl_u8 name, gs_rref out);
GS_API uint8_t gs_os_resource_dir(gs_rref out);
GS_API int64_t gs_os_time_ns(void);
GS_API int64_t gs_os_clock_ns(void);
GS_API void gs_os_sleep_ms(int64_t ms);
GS_API uint64_t gs_os_random_u64(void);
/* os.trim_memory: discards unused complete pages of this thread program's
   data stacks. Defined with the region registry it walks (runtime_impl.h),
   but declared here like the other os primitives, so that no prototype is
   emitted for it. For generated code only, whose callers write back their
   cached tops first; C called from Goose must not call it. 0 means an OS
   request failed; a top outside its region panics. */
GS_API uint8_t gs_trim_memory(void);
/* Gust: the network behind stdlib/net.goose (runtime_net.h): handles are
   checked int64_t values, and a negative result is an error. */
GS_API int64_t gs_net_listen(int64_t port, int64_t backlog, uint8_t local_only);
GS_API int64_t gs_net_listener_port(int64_t listener);
GS_API int64_t gs_net_close_listener(int64_t listener);
GS_API int64_t gs_net_poller(void);
GS_API int64_t gs_net_close_poller(int64_t poller);
GS_API int64_t gs_net_count(int64_t poller);
GS_API int64_t gs_net_watch(int64_t poller, int64_t listener, uint8_t on);
GS_API int64_t gs_net_accept(int64_t poller, int64_t listener);
GS_API int64_t gs_net_listener_open(void);
GS_API int64_t gs_net_connect(int64_t poller, sl_u8 host, int64_t port, int64_t timeout_ms);
GS_API int64_t gs_net_recv(int64_t poller, int64_t conn, sl_u8 buf);
GS_API int64_t gs_net_send(int64_t poller, int64_t conn, sl_u8 bytes);
GS_API int64_t gs_net_want(int64_t poller, int64_t conn, int64_t events);
GS_API int64_t gs_net_shutdown(int64_t poller, int64_t conn);
GS_API int64_t gs_net_close(int64_t poller, int64_t conn);
GS_API int64_t gs_net_wait(int64_t poller, int64_t ms, sl_i64 handles, sl_i64 events);
GS_API int64_t gs_net_detach(int64_t poller, int64_t conn);
GS_API int64_t gs_net_adopt(int64_t poller, int64_t detached);
GS_API int64_t gs_net_os_error(void);
GS_API void gs_net_catch_signals(void);
GS_API void gs_net_request_stop(void);
GS_API uint8_t gs_net_stop_requested(void);
