/* voxel_mkrgba_test.c — Verify mk_rgba produces RGBA8888 with R in byte[0].
 *
 * The framebuffer is RGBA8888: R=byte[0], G=byte[1], B=byte[2], A=byte[3].
 * On little-endian RV32I a uint32 at address X stores byte[0] in bits[7:0],
 * so the correct pack is:  A<<24 | B<<16 | G<<8 | R
 *
 * This test verifies the encoding WITHOUT the framebuffer peripheral.
 */
#include <stdint.h>
#include <stdio.h>

/* Replicate mk_rgba so we can test it in isolation */
static uint32_t mk_rgba(int r, int g, int b) {
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    /* Correct RGBA8888 little-endian pack: R in bits[7:0] */
    return 0xFF000000u | ((uint32_t)b<<16) | ((uint32_t)g<<8) | (uint32_t)r;
}

static void check(const char *label, int pass) {
    printf("%s: %s\n", label, pass ? "OK" : "FAIL");
}

int main(void) {
    printf("voxel_mkrgba_test\n");

    /* Red = RGBA(255,0,0,255).  Word value in LE: byte[0]=R=255 → word=0xFF0000FF */
    uint32_t red = mk_rgba(255, 0, 0);
    uint8_t *rb = (uint8_t*)&red;
    printf("red=0x%08X byte0=%u byte1=%u byte2=%u byte3=%u\n",
           red, rb[0], rb[1], rb[2], rb[3]);
    check("red_word",  red == 0xFF0000FFu);
    check("red_byte0_R", rb[0] == 255);
    check("red_byte1_G", rb[1] == 0);
    check("red_byte2_B", rb[2] == 0);
    check("red_byte3_A", rb[3] == 255);

    /* Green = RGBA(0,255,0,255).  Word = 0xFF00FF00 */
    uint32_t green = mk_rgba(0, 255, 0);
    uint8_t *gb = (uint8_t*)&green;
    printf("green=0x%08X byte0=%u byte1=%u byte2=%u\n", green, gb[0], gb[1], gb[2]);
    check("green_word",  green == 0xFF00FF00u);
    check("green_byte0_R", gb[0] == 0);
    check("green_byte1_G", gb[1] == 255);
    check("green_byte2_B", gb[2] == 0);

    /* Blue = RGBA(0,0,255,255).  Word = 0xFFFF0000 */
    uint32_t blue = mk_rgba(0, 0, 255);
    uint8_t *bb2 = (uint8_t*)&blue;
    printf("blue=0x%08X byte0=%u byte1=%u byte2=%u\n", blue, bb2[0], bb2[1], bb2[2]);
    check("blue_word",   blue == 0xFFFF0000u);
    check("blue_byte0_R", bb2[0] == 0);
    check("blue_byte1_G", bb2[1] == 0);
    check("blue_byte2_B", bb2[2] == 255);

    return 0;
}
