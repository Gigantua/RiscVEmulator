#include "libc.h"

static int approx(double a, double b, double eps) {
    double diff = a - b;
    if (diff < 0) diff = -diff;
    return diff < eps;
}

void _start(void) {
    /* fabs */
    printf("fabs_pos:%d\n", approx(fabs(3.14), 3.14, 0.001));
    printf("fabs_neg:%d\n", approx(fabs(-2.5), 2.5, 0.001));

    /* floor / ceil / round */
    printf("floor_pos:%d\n", approx(floor(3.7), 3.0, 0.001));
    printf("floor_neg:%d\n", approx(floor(-3.2), -4.0, 0.001));
    printf("ceil_pos:%d\n", approx(ceil(3.2), 4.0, 0.001));
    printf("ceil_neg:%d\n", approx(ceil(-3.7), -3.0, 0.001));
    printf("round_up:%d\n", approx(round(3.5), 4.0, 0.001));
    printf("round_down:%d\n", approx(round(3.4), 3.0, 0.001));

    /* fmod */
    printf("fmod:%d\n", approx(fmod(5.5, 2.0), 1.5, 0.001));

    /* sqrt */
    printf("sqrt_4:%d\n", approx(sqrt(4.0), 2.0, 0.0001));
    printf("sqrt_2:%d\n", approx(sqrt(2.0), 1.41421356, 0.0001));

    /* sin / cos */
    printf("sin_0:%d\n", approx(sin(0.0), 0.0, 0.0001));
    printf("sin_pi2:%d\n", approx(sin(M_PI_2), 1.0, 0.001));
    printf("cos_0:%d\n", approx(cos(0.0), 1.0, 0.0001));
    printf("cos_pi:%d\n", approx(cos(M_PI), -1.0, 0.001));

    /* tan */
    printf("tan_0:%d\n", approx(tan(0.0), 0.0, 0.0001));
    printf("tan_pi4:%d\n", approx(tan(M_PI_4), 1.0, 0.01));

    /* atan / atan2 */
    printf("atan_1:%d\n", approx(atan(1.0), M_PI_4, 0.05));
    printf("atan2_11:%d\n", approx(atan2(1.0, 1.0), M_PI_4, 0.05));

    /* asin / acos */
    printf("asin_1:%d\n", approx(asin(1.0), M_PI_2, 0.01));
    printf("acos_0:%d\n", approx(acos(0.0), M_PI_2, 0.01));

    /* exp / log */
    printf("exp_0:%d\n", approx(exp(0.0), 1.0, 0.0001));
    printf("exp_1:%d\n", approx(exp(1.0), M_E, 0.01));
    printf("log_1:%d\n", approx(log(1.0), 0.0, 0.0001));
    printf("log_e:%d\n", approx(log(M_E), 1.0, 0.01));

    /* pow */
    printf("pow_23:%d\n", approx(pow(2.0, 3.0), 8.0, 0.001));
    printf("pow_sq:%d\n", approx(pow(3.0, 2.0), 9.0, 0.001));

    printf("PASS\n");
    exit(0);
}
