/* math.h — Math functions for bare-metal RV32I */
#pragma once

/* ── Special-value macros ─────────────────────────────────────────── */
#define HUGE_VAL  __builtin_huge_val()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")

/* ── Classification macros ────────────────────────────────────────── */
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)

/* ── Mathematical constants ───────────────────────────────────────── */
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* ── Absolute value ───────────────────────────────────────────────── */
double fabs(double x);
float  fabsf(float x);

/* ── Rounding ─────────────────────────────────────────────────────── */
double floor(double x);
float  floorf(float x);
double ceil(double x);
float  ceilf(float x);
double round(double x);
float  roundf(float x);

/* ── Remainder ────────────────────────────────────────────────────── */
double fmod(double x, double y);
float  fmodf(float x, float y);

/* ── Square root ──────────────────────────────────────────────────── */
double sqrt(double x);
float  sqrtf(float x);

/* ── Trigonometric ────────────────────────────────────────────────── */
double sin(double x);
double cos(double x);
double tan(double x);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);

/* ── Inverse trigonometric ────────────────────────────────────────── */
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
float  atan2f(float y, float x);

/* ── Exponential and logarithmic ──────────────────────────────────── */
double exp(double x);
double log(double x);
double pow(double base, double exponent);
float  expf(float x);
float  logf(float x);
float  powf(float base, float exponent);
