/* math.c — Software math library for bare-metal RV32I.
 * All float/double operations use softfloat.c compiler-rt builtins.
 * Compile with: clang --target=riscv32-unknown-elf -march=rv32i -O1 -fno-builtin -c
 */

#include <math.h>
#include <stdint.h>

/* ── Constants ─────────────────────────────────────────────────────── */
#define PI      3.14159265358979323846
#define TWO_PI  6.28318530717958647692
#define HALF_PI 1.57079632679489661923
#define LN2     0.69314718055994530942

/* ── Helper: reduce angle to [-PI, PI] ─────────────────────────────── */
static double _reduce_angle(double x) {
    x = fmod(x, TWO_PI);
    if (x > PI)  x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    return x;
}

/* ── Absolute value ───────────────────────────────────────────────── */

double fabs(double x) {
    return x < 0 ? -x : x;
}

float fabsf(float x) {
    return x < 0 ? -x : x;
}

/* ── Rounding ─────────────────────────────────────────────────────── */

double floor(double x) {
    if (!isfinite(x)) return x;
    int i = (int)x;
    return (double)(x < (double)i ? i - 1 : i);
}

float floorf(float x) {
    return (float)floor((double)x);
}

double ceil(double x) {
    return -floor(-x);
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

double round(double x) {
    return floor(x + 0.5);
}

float roundf(float x) {
    return (float)round((double)x);
}

/* ── Remainder ────────────────────────────────────────────────────── */

double fmod(double x, double y) {
    if (y == 0.0) return NAN;
    return x - (double)(int)(x / y) * y;
}

float fmodf(float x, float y) {
    return (float)fmod((double)x, (double)y);
}

/* ── Square root ──────────────────────────────────────────────────── */

double sqrt(double x) {
    if (isnan(x) || x < 0.0) return NAN;
    if (x == 0.0) return 0.0;
    if (isinf(x)) return x;
    double guess = x * 0.5;
    for (int i = 0; i < 30; i++) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

float sqrtf(float x) {
    return (float)sqrt((double)x);
}

/* ── Trigonometric ────────────────────────────────────────────────── */

double sin(double x) {
    if (!isfinite(x)) return NAN;
    x = _reduce_angle(x);
    /* Taylor series: sin(x) = sum (-1)^k * x^(2k+1) / (2k+1)! */
    double term = x;
    double sum = x;
    double x2 = x * x;
    for (int k = 1; k <= 12; k++) {
        term *= -x2 / (double)((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) {
    if (!isfinite(x)) return NAN;
    x = _reduce_angle(x);
    /* Taylor series: cos(x) = sum (-1)^k * x^(2k) / (2k)! */
    double term = 1.0;
    double sum = 1.0;
    double x2 = x * x;
    for (int k = 1; k <= 12; k++) {
        term *= -x2 / (double)((2 * k - 1) * (2 * k));
        sum += term;
    }
    return sum;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return (sin(x) > 0) ? HUGE_VAL : -HUGE_VAL;
    return sin(x) / c;
}

float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }

/* ── Inverse trigonometric ────────────────────────────────────────── */

double atan(double x) {
    if (isnan(x)) return NAN;
    /* For |x| > 1, use identity: atan(x) = pi/2 - atan(1/x) */
    if (x > 1.0) return HALF_PI - atan(1.0 / x);
    if (x < -1.0) return -HALF_PI - atan(1.0 / x);
    /* Taylor series for |x| <= 1: atan(x) = x - x^3/3 + x^5/5 - ... */
    double term = x;
    double sum = x;
    double x2 = x * x;
    for (int k = 1; k <= 25; k++) {
        term *= -x2;
        sum += term / (double)(2 * k + 1);
    }
    return sum;
}

double atan2(double y, double x) {
    if (isnan(x) || isnan(y)) return NAN;
    if (x > 0.0) return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + PI;
    if (x < 0.0 && y < 0.0) return atan(y / x) - PI;
    if (x == 0.0 && y > 0.0) return HALF_PI;
    if (x == 0.0 && y < 0.0) return -HALF_PI;
    return 0.0; /* x==0, y==0 */
}

float atan2f(float y, float x) {
    return (float)atan2((double)y, (double)x);
}

double asin(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x < -1.0 || x > 1.0) return NAN;
    return atan2(sqrt(1.0 - x * x), x);
}

/* ── Exponential and logarithmic ──────────────────────────────────── */

/* Internal exp to avoid recursion with the public exp() wrapper. */
static double _exp_impl(double x) {
    if (isnan(x)) return NAN;
    if (x > 709.0) return INFINITY;
    if (x < -709.0) return 0.0;

    /* Range reduction: x = n*ln(2) + r, |r| <= ln(2)/2 */
    int n = (int)(x / LN2 + (x >= 0 ? 0.5 : -0.5));
    double r = x - (double)n * LN2;

    /* Taylor series for exp(r) — 20 terms */
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 20; k++) {
        term *= r / (double)k;
        sum += term;
    }

    /* Multiply by 2^n via repeated doubling/halving */
    if (n >= 0) {
        for (int i = 0; i < n; i++) sum *= 2.0;
    } else {
        for (int i = 0; i < -n; i++) sum *= 0.5;
    }
    return sum;
}

double exp(double x) {
    return _exp_impl(x);
}

float expf(float x) {
    return (float)_exp_impl((double)x);
}

/* Internal log to avoid recursion with the public log() wrapper. */
static double _log_impl(double x) {
    if (isnan(x) || x < 0.0) return NAN;
    if (x == 0.0) return -INFINITY;
    if (isinf(x)) return INFINITY;

    /* Decompose: x = m * 2^e, where m in [1, 2) */
    int e = 0;
    double m = x;
    while (m >= 2.0) { m *= 0.5; e++; }
    while (m < 1.0)  { m *= 2.0; e--; }

    /* log(x) = e*ln(2) + log(m), where m in [1, 2)
     * Let t = (m - 1)/(m + 1), then log(m) = 2*(t + t^3/3 + t^5/5 + ...)
     * This series converges faster than the plain log(1+t) series. */
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double term = t;
    double sum = t;
    for (int k = 1; k <= 25; k++) {
        term *= t2;
        sum += term / (double)(2 * k + 1);
    }
    return (double)e * LN2 + 2.0 * sum;
}

double log(double x) {
    return _log_impl(x);
}

float logf(float x) {
    return (float)_log_impl((double)x);
}

double pow(double base, double exponent) {
    if (exponent == 0.0) return 1.0;
    if (base == 0.0) {
        if (exponent > 0.0) return 0.0;
        return INFINITY;
    }
    if (base == 1.0) return 1.0;

    /* Negative base with integer exponent */
    if (base < 0.0) {
        int iexp = (int)exponent;
        if ((double)iexp != exponent) return NAN; /* fractional exp of negative base */
        double result = _exp_impl(exponent * _log_impl(-base));
        return (iexp & 1) ? -result : result;
    }

    return _exp_impl(exponent * _log_impl(base));
}

float powf(float base, float exponent) {
    return (float)pow((double)base, (double)exponent);
}
