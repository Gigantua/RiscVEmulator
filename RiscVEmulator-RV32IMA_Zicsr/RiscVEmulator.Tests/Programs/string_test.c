#include "libc.h"

void _start(void) {
    // strlen
    printf("strlen_empty:%d\n", (int)strlen(""));
    printf("strlen_hello:%d\n", (int)strlen("hello"));

    // strcmp
    printf("strcmp_eq:%d\n", strcmp("abc", "abc") == 0);
    printf("strcmp_lt:%d\n", strcmp("abc", "abd") < 0);
    printf("strcmp_gt:%d\n", strcmp("abd", "abc") > 0);

    // strncmp
    printf("strncmp_eq:%d\n", strncmp("abcdef", "abcxyz", 3) == 0);
    printf("strncmp_ne:%d\n", strncmp("abcdef", "abcxyz", 4) != 0);

    // strcpy
    char buf[32];
    strcpy(buf, "hello");
    printf("strcpy:%s\n", buf);

    // strncpy
    char buf2[8];
    strncpy(buf2, "hi", 8);
    printf("strncpy:%s\n", buf2);
    printf("strncpy_pad:%d\n", buf2[3] == '\0');  // should be zero-padded

    // strcat
    char cat[32];
    strcpy(cat, "hello");
    strcat(cat, " world");
    printf("strcat:%s\n", cat);

    // strncat
    char ncat[32];
    strcpy(ncat, "foo");
    strncat(ncat, "barbaz", 3);
    printf("strncat:%s\n", ncat);

    // strchr
    printf("strchr:%c\n", *strchr("hello", 'l'));
    printf("strchr_null:%d\n", strchr("hello", 'z') == NULL);

    // strrchr
    printf("strrchr:%c\n", *strrchr("hello", 'l'));

    // strstr
    printf("strstr:%s\n", strstr("hello world", "world"));
    printf("strstr_null:%d\n", strstr("hello", "xyz") == NULL);

    // memcmp
    printf("memcmp_eq:%d\n", memcmp("abc", "abc", 3) == 0);
    printf("memcmp_ne:%d\n", memcmp("abc", "abd", 3) < 0);

    // memchr
    printf("memchr:%c\n", *(char *)memchr("hello", 'l', 5));
    printf("memchr_null:%d\n", memchr("hello", 'z', 5) == NULL);

    // memmove (overlapping)
    char mv[16] = "abcdefgh";
    memmove(mv + 2, mv, 6);  // "ababcdef"
    mv[8] = '\0';
    printf("memmove:%s\n", mv);

    printf("PASS\n");
    exit(0);
}
