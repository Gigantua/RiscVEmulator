/* voxel_clearscreen_test.c — Verify clear_screen writes non-black pixels to the FB.
 *
 * Includes voxel_main.c with voxel_NO_MAIN to get all helper functions,
 * then calls clear_screen() and reads back framebuffer pixels.
 *
 * Expected: pixel (0,0) has non-zero R,G,B (sky gradient at y=0).
 * Expected: pixel (0,199) has different color from pixel (0,0) (gradient varies).
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_clearscreen_test\n");

    /* Precompute sky gradient (normally called once in main() before game loop) */
    init_sky();

    /* clear_screen writes sky gradient to s_shadow and resets depth buffer */
    clear_screen();

    /* Blit shadow buffer → MMIO framebuffer so reads below work */
    memcpy((void*)FB_BASE, s_shadow, FB_PIXELS * sizeof(uint32_t));
    DISP_VSYNC_REG = 1;

    /* Read back pixel (0,0) directly from FB memory */
    volatile uint8_t *fb_bytes = (volatile uint8_t*)0x20000000u;
    uint8_t r0 = fb_bytes[0]; /* R */
    uint8_t g0 = fb_bytes[1]; /* G */
    uint8_t b0 = fb_bytes[2]; /* B */
    uint8_t a0 = fb_bytes[3]; /* A */
    printf("px(0,0): R=%u G=%u B=%u A=%u\n", r0, g0, b0, a0);

    /* Read pixel at bottom row (0, 199) */
    int offset_last = (FB_HEIGHT - 1) * FB_WIDTH * 4;
    uint8_t rL = fb_bytes[offset_last + 0];
    uint8_t gL = fb_bytes[offset_last + 1];
    uint8_t bL = fb_bytes[offset_last + 2];
    printf("px(0,%d): R=%u G=%u B=%u\n", FB_HEIGHT-1, rL, gL, bL);

    /* The sky is a vertical gradient — top and bottom should differ */
    int nonblack = (r0 != 0 || g0 != 0 || b0 != 0);
    int gradient = (r0 != rL || g0 != gL || b0 != bL);
    int alpha_ok = (a0 == 255);

    printf("nonblack: %s\n", nonblack ? "OK" : "FAIL");
    printf("gradient: %s\n", gradient ? "OK" : "FAIL");
    printf("alpha_ok: %s\n", alpha_ok ? "OK" : "FAIL");

    /* Coverage mask is all-zero after clear (0 = pixel not yet drawn) */
    int depth_ok = (s_cover[0] == 0u);
    printf("depth_ok: %s\n", depth_ok ? "OK" : "FAIL");

    return 0;
}
