/* softfp_shim.c — translate libgcc/compiler-rt soft-float ABI
 * (__addsf3, __mulsf3, __ltsf2, ...) into Bellard's softfp API
 * (add_sf32, mul_sf32, ...). Rounding mode RM_RNE (IEEE default).
 * Flag pointer is required by Bellard's API but the libgcc ABI has
 * no slot for it, so we use a local sink. */
#include <stdint.h>
#include "softfp.h"

typedef union { float    f; uint32_t u; } f32_u;
typedef union { double   d; uint64_t u; } f64_u;

static inline uint32_t f2u32(float    f) { f32_u x; x.f = f; return x.u; }
static inline uint64_t f2u64(double   d) { f64_u x; x.d = d; return x.u; }
static inline float    u32f(uint32_t  u) { f32_u x; x.u = u; return x.f; }
static inline double   u64f(uint64_t  u) { f64_u x; x.u = u; return x.d; }

/* Bellard's softfp_template.h defines `isnan_sfN(F_UINT)` as a
 * (non-static) function, but doesn't declare it in softfp.h. The
 * amalgamated build (Runtime/softfloat.c -> softfp.c -> template ->
 * shim) makes the definitions visible to us. */
int isnan_sf32(uint32_t a);
int isnan_sf64(uint64_t a);

/* ── f32 arithmetic ─────────────────────────────────────────────── */
float __addsf3(float a, float b) { uint32_t f = 0; return u32f(add_sf32(f2u32(a), f2u32(b), RM_RNE, &f)); }
float __subsf3(float a, float b) { uint32_t f = 0; return u32f(sub_sf32(f2u32(a), f2u32(b), RM_RNE, &f)); }
float __mulsf3(float a, float b) { uint32_t f = 0; return u32f(mul_sf32(f2u32(a), f2u32(b), RM_RNE, &f)); }
float __divsf3(float a, float b) { uint32_t f = 0; return u32f(div_sf32(f2u32(a), f2u32(b), RM_RNE, &f)); }

/* ── f32 comparisons ─────────────────────────────────────────────
 * libgcc convention (compiler-rt agrees):
 *   __ltsf2 / __lesf2 → POSITIVE on unordered
 *   __gtsf2 / __gesf2 → NEGATIVE on unordered
 *   __eqsf2 / __nesf2 → NONZERO on unordered (i.e. "not equal") */
static int sf32_cmp(uint32_t a, uint32_t b) {
    uint32_t f = 0;
    if (eq_quiet_sf32(a, b, &f)) return 0;
    if (isnan_sf32(a) || isnan_sf32(b)) return 2;
    if (lt_sf32(a, b, &f)) return -1;
    return 1;
}
int __eqsf2(uint32_t a, uint32_t b) { int c = sf32_cmp(a, b); return c == 0 ? 0 : 1; }
int __nesf2(uint32_t a, uint32_t b) { int c = sf32_cmp(a, b); return c != 0 ? 1 : 0; }
int __ltsf2(uint32_t a, uint32_t b) { int c = sf32_cmp(a, b); return c == 2 ?  1 : c; }
int __lesf2(uint32_t a, uint32_t b) { int c = sf32_cmp(a, b); return c == 2 ?  1 : c; }
int __gtsf2(uint32_t a, uint32_t b) { int c = sf32_cmp(a, b); return c == 2 ? -1 : c; }
int __gesf2(uint32_t a, uint32_t b) { int c = sf32_cmp(a, b); return c == 2 ? -1 : c; }
int __unordsf2(uint32_t a, uint32_t b) { return (isnan_sf32(a) || isnan_sf32(b)) ? 1 : 0; }

/* ── f32 conversions ────────────────────────────────────────────── */
int32_t  __fixsfsi(float a)         { uint32_t f = 0; return cvt_sf32_i32(f2u32(a), RM_RTZ, &f); }
uint32_t __fixunssfsi(float a)      { uint32_t f = 0; return cvt_sf32_u32(f2u32(a), RM_RTZ, &f); }
int64_t  __fixsfdi(float a)         { uint32_t f = 0; return cvt_sf32_i64(f2u32(a), RM_RTZ, &f); }
uint64_t __fixunssfdi(float a)      { uint32_t f = 0; return cvt_sf32_u64(f2u32(a), RM_RTZ, &f); }
float    __floatsisf(int32_t a)     { uint32_t f = 0; return u32f(cvt_i32_sf32(a, RM_RNE, &f)); }
float    __floatunsisf(uint32_t a)  { uint32_t f = 0; return u32f(cvt_u32_sf32(a, RM_RNE, &f)); }
float    __floatdisf(int64_t a)     { uint32_t f = 0; return u32f(cvt_i64_sf32(a, RM_RNE, &f)); }
float    __floatundisf(uint64_t a)  { uint32_t f = 0; return u32f(cvt_u64_sf32(a, RM_RNE, &f)); }

float    __extendsfsf2(float a)     { return a; }
double   __extendsfdf2(float a)     { uint32_t f = 0; return u64f(cvt_sf32_sf64(f2u32(a), &f)); }
float    __truncdfsf2(double a)     { uint32_t f = 0; return u32f(cvt_sf64_sf32(f2u64(a), RM_RNE, &f)); }

/* ── f64 arithmetic ─────────────────────────────────────────────── */
double __adddf3(double a, double b) { uint32_t f = 0; return u64f(add_sf64(f2u64(a), f2u64(b), RM_RNE, &f)); }
double __subdf3(double a, double b) { uint32_t f = 0; return u64f(sub_sf64(f2u64(a), f2u64(b), RM_RNE, &f)); }
double __muldf3(double a, double b) { uint32_t f = 0; return u64f(mul_sf64(f2u64(a), f2u64(b), RM_RNE, &f)); }
double __divdf3(double a, double b) { uint32_t f = 0; return u64f(div_sf64(f2u64(a), f2u64(b), RM_RNE, &f)); }

/* ── f64 comparisons ────────────────────────────────────────────── */
static int sf64_cmp(uint64_t a, uint64_t b) {
    uint32_t f = 0;
    if (eq_quiet_sf64(a, b, &f)) return 0;
    if (isnan_sf64(a) || isnan_sf64(b)) return 2;
    if (lt_sf64(a, b, &f)) return -1;
    return 1;
}
int __eqdf2(uint64_t a, uint64_t b) { int c = sf64_cmp(a, b); return c == 0 ? 0 : 1; }
int __nedf2(uint64_t a, uint64_t b) { int c = sf64_cmp(a, b); return c != 0 ? 1 : 0; }
int __ltdf2(uint64_t a, uint64_t b) { int c = sf64_cmp(a, b); return c == 2 ?  1 : c; }
int __ledf2(uint64_t a, uint64_t b) { int c = sf64_cmp(a, b); return c == 2 ?  1 : c; }
int __gtdf2(uint64_t a, uint64_t b) { int c = sf64_cmp(a, b); return c == 2 ? -1 : c; }
int __gedf2(uint64_t a, uint64_t b) { int c = sf64_cmp(a, b); return c == 2 ? -1 : c; }
int __unorddf2(uint64_t a, uint64_t b) { return (isnan_sf64(a) || isnan_sf64(b)) ? 1 : 0; }

/* ── f64 conversions ────────────────────────────────────────────── */
int32_t  __fixdfsi(double a)        { uint32_t f = 0; return cvt_sf64_i32(f2u64(a), RM_RTZ, &f); }
uint32_t __fixunsdfsi(double a)     { uint32_t f = 0; return cvt_sf64_u32(f2u64(a), RM_RTZ, &f); }
int64_t  __fixdfdi(double a)        { uint32_t f = 0; return cvt_sf64_i64(f2u64(a), RM_RTZ, &f); }
uint64_t __fixunsdfdi(double a)     { uint32_t f = 0; return cvt_sf64_u64(f2u64(a), RM_RTZ, &f); }
double   __floatsidf(int32_t a)     { uint32_t f = 0; return u64f(cvt_i32_sf64(a, RM_RNE, &f)); }
double   __floatunsidf(uint32_t a)  { uint32_t f = 0; return u64f(cvt_u32_sf64(a, RM_RNE, &f)); }
double   __floatdidf(int64_t a)     { uint32_t f = 0; return u64f(cvt_i64_sf64(a, RM_RNE, &f)); }
double   __floatundidf(uint64_t a)  { uint32_t f = 0; return u64f(cvt_u64_sf64(a, RM_RNE, &f)); }
