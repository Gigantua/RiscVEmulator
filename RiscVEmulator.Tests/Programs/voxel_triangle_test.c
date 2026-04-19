/* voxel_triangle_test.c — Verify the rasterizer draws a triangle.
 *
 * Sets up a large screen-space triangle that clearly covers the screen center
 * (160, 100), then checks that pixel is non-black after draw_triangle.
 *
 * Also checks winding order convention (negative-area triangles are drawn).
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_triangle_test\n");

    /* Clear s_shadow to black and reset coverage mask */
    for (int i = 0; i < FB_PIXELS; i++) s_shadow[i] = 0u;
    memset(s_cover, 0, sizeof(s_cover));

    /* Put a solid green tile in atlas slot (0,0) so we know what colour to expect */
    for (int py = 0; py < TILE_SIZE; py++)
        for (int px = 0; px < TILE_SIZE; px++)
            s_atlas[py * ATLAS_W + px] = mk_rgba(0, 200, 0); /* green */

    /* Screen-space triangle with negative area (draw_triangle expects area < 0).
     * edge_fn(v0, v1, v2) with these points:
     *   v0=(50,170), v1=(270,170), v2=(160,30)
     * area = (160-50)*(170-170) - (30-170)*(270-50)
     *       = 110*0 - (-140)*220 = 30800  ← positive!
     * We need negative area, so swap v0/v1:
     *   v0=(270,170), v1=(50,170), v2=(160,30)
     * area = (160-270)*(170-170) - (30-170)*(50-270)
     *       = -110*0 - (-140)*(-220) = -30800  ← negative ✓
     */
    PVert v0, v1, v2;
    /* v0 = (270, 170) */
    v0.sx=270; v0.sy=170; v0.u=0.9f; v0.v=0.9f;
    /* v1 = (50, 170) */
    v1.sx= 50; v1.sy=170; v1.u=0.1f; v1.v=0.9f;
    /* v2 = (160, 30) */
    v2.sx=160; v2.sy= 30; v2.u=0.5f; v2.v=0.1f;

    draw_triangle(&v0, &v1, &v2, 0, 0, 256 /* ao_i: fully lit */, 0 /* fog_i: no fog */);

    /* Blit shadow buffer → MMIO framebuffer */
    memcpy((void*)FB_BASE, s_shadow, FB_PIXELS * sizeof(uint32_t));

    /* Check centre pixel (160, 100) — should be green */
    volatile uint8_t *fb_bytes = (volatile uint8_t*)0x20000000u;
    int centre_idx = (100 * FB_WIDTH + 160) * 4;
    uint8_t cr = fb_bytes[centre_idx + 0];
    uint8_t cg = fb_bytes[centre_idx + 1];
    uint8_t cb = fb_bytes[centre_idx + 2];
    printf("centre_pixel: R=%u G=%u B=%u\n", cr, cg, cb);

    int drawn = (cg > 100); /* green channel should be high */
    printf("triangle_drawn: %s\n", drawn ? "OK" : "FAIL");

    /* Check a corner pixel outside the triangle — should still be black */
    uint8_t corner_r = fb_bytes[0];
    uint8_t corner_g = fb_bytes[1];
    printf("corner_black: %s\n", (corner_r == 0 && corner_g == 0) ? "OK" : "FAIL");

    /* Check coverage bit was set at centre: pixel 100*FB_WIDTH+160 should be covered */
    int centre_pix = 100 * FB_WIDTH + 160;
    int depth_written = ((s_cover[centre_pix >> 5] >> (centre_pix & 31)) & 1u) != 0;
    printf("depth_written: %s\n", depth_written ? "OK" : "FAIL");

    return 0;
}
