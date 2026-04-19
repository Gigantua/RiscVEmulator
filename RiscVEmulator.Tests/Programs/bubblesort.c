/* bubblesort.c
 * Sort a 20-element array and print it before and after.
 *
 * Exercises: LW/SW with computed offsets (a[i], a[j]), global .data section
 * (tests ELF loader + LUI/ADDI for absolute address), all 6 branch types
 * (BEQ inner-loop exit, BNE outer-loop, BLT/BGE comparison, BLTU/BGEU for
 * unsigned index bounds), SUB and ADD for comparisons.
 *
 * The unsorted array deliberately contains values that force every branch path:
 * duplicates (BEQ path), ascending and descending runs.
 */

#include "libc.h"

/* Global array in .data — exercises ELF segment loading */
static int arr[20] = {
    64, -3, 17, 0, 99, -50, 42, 7, 7, 100,
    -1, 28, 55, 13, -99, 6, 88, 33, 21, 0
};
#define N 20

static void print_arr(void)
{
    for (unsigned int i = 0u; i < N; i++) {
        printf("%d", arr[i]);
        printf(i == N - 1u ? "\n" : " ");
    }
}

static void bubble_sort(void)
{
    /* BLT / BGE: signed comparison of array elements */
    for (unsigned int i = 0u; i < N - 1u; i++) {        /* BNE outer */
        for (unsigned int j = 0u; j < N - 1u - i; j++) { /* BLTU inner */
            if (arr[j] > arr[j + 1]) {                   /* BGE swap trigger */
                int tmp   = arr[j];
                arr[j]    = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }
    /* BGEU: used by clang for the loop-exit condition on unsigned counters */
}

void _start(void)
{
    printf("Before: ");
    print_arr();
    bubble_sort();
    printf("After:  ");
    print_arr();

    exit(0);
}
