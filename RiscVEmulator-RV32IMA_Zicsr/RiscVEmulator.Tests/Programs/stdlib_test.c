#include "libc.h"

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

void _start(void) {
    /* abs / labs */
    printf("abs_pos:%d\n", abs(42));
    printf("abs_neg:%d\n", abs(-42));
    printf("labs_neg:%d\n", (int)labs(-100L));

    /* atoi / atol */
    printf("atoi:%d\n", atoi("12345"));
    printf("atoi_neg:%d\n", atoi("-99"));
    printf("atoi_space:%d\n", atoi("  42"));
    printf("atol:%d\n", (int)atol("999999"));

    /* strtol */
    printf("strtol_dec:%d\n", (int)strtol("12345", NULL, 10));
    printf("strtol_hex:%d\n", (int)strtol("ff", NULL, 16));
    printf("strtol_hex0x:%d\n", (int)strtol("0xff", NULL, 0));
    printf("strtol_oct:%d\n", (int)strtol("077", NULL, 0));
    printf("strtol_neg:%d\n", (int)strtol("-42", NULL, 10));

    /* strtol endptr */
    char *end;
    strtol("123abc", &end, 10);
    printf("strtol_end:%s\n", end);

    /* strtoul */
    printf("strtoul_hex:%u\n", (unsigned int)strtoul("deadbeef", NULL, 16));

    /* div / ldiv */
    div_t d = div(17, 5);
    printf("div_quot:%d\n", d.quot);
    printf("div_rem:%d\n", d.rem);
    ldiv_t ld = ldiv(-100L, 7L);
    printf("ldiv_quot:%d\n", (int)ld.quot);
    printf("ldiv_rem:%d\n", (int)ld.rem);

    /* rand / srand */
    srand(42);
    int r1 = rand();
    int r2 = rand();
    printf("rand_diff:%d\n", r1 != r2);
    srand(42);
    int r3 = rand();
    printf("rand_repeat:%d\n", r1 == r3);
    printf("rand_range:%d\n", r1 >= 0 && r1 <= 32767);

    /* qsort */
    int arr[] = {5, 3, 8, 1, 4, 2, 7, 6};
    qsort(arr, 8, sizeof(int), cmp_int);
    printf("qsort:");
    for (int i = 0; i < 8; i++) {
        if (i > 0) printf(",");
        printf("%d", arr[i]);
    }
    printf("\n");

    /* bsearch */
    int key = 4;
    int *found = (int *)bsearch(&key, arr, 8, sizeof(int), cmp_int);
    printf("bsearch:%d\n", found != NULL && *found == 4);
    key = 99;
    found = (int *)bsearch(&key, arr, 8, sizeof(int), cmp_int);
    printf("bsearch_miss:%d\n", found == NULL);

    printf("PASS\n");
    exit(0);
}
