#include "libc.h"

void _start(void) {
    // isdigit
    printf("isdigit_0:%d\n", isdigit('0') != 0);
    printf("isdigit_9:%d\n", isdigit('9') != 0);
    printf("isdigit_a:%d\n", isdigit('a') == 0);

    // isalpha
    printf("isalpha_A:%d\n", isalpha('A') != 0);
    printf("isalpha_z:%d\n", isalpha('z') != 0);
    printf("isalpha_5:%d\n", isalpha('5') == 0);

    // isalnum
    printf("isalnum_a:%d\n", isalnum('a') != 0);
    printf("isalnum_5:%d\n", isalnum('5') != 0);
    printf("isalnum_bang:%d\n", isalnum('!') == 0);

    // isspace
    printf("isspace_sp:%d\n", isspace(' ') != 0);
    printf("isspace_tab:%d\n", isspace('\t') != 0);
    printf("isspace_a:%d\n", isspace('a') == 0);

    // isupper/islower
    printf("isupper_A:%d\n", isupper('A') != 0);
    printf("isupper_a:%d\n", isupper('a') == 0);
    printf("islower_a:%d\n", islower('a') != 0);
    printf("islower_A:%d\n", islower('A') == 0);

    // toupper/tolower
    printf("toupper_a:%c\n", toupper('a'));
    printf("tolower_A:%c\n", tolower('A'));
    printf("toupper_5:%c\n", toupper('5'));  // unchanged

    // isxdigit
    printf("isxdigit_f:%d\n", isxdigit('f') != 0);
    printf("isxdigit_F:%d\n", isxdigit('F') != 0);
    printf("isxdigit_g:%d\n", isxdigit('g') == 0);

    // isprint/iscntrl
    printf("isprint_a:%d\n", isprint('a') != 0);
    printf("isprint_nul:%d\n", isprint('\0') == 0);
    printf("iscntrl_nul:%d\n", iscntrl('\0') != 0);
    printf("iscntrl_a:%d\n", iscntrl('a') == 0);

    printf("PASS\n");
    exit(0);
}
