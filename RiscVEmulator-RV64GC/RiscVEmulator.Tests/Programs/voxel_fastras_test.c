/* voxel_fastras_test.c — Verify draw_triangle: pixel fill, coverage mask.
 *
 * Draws two overlapping triangles using front-to-back coverage:
 *   Pass 1 (close, drawn first):  red  — should land on centre pixel
 *   Pass 2 (far,   drawn second): blue — should be blocked by coverage mask
 *
 * Verifies the incremental edge stepper, coverage bit logic, and
 * flat-colour write via draw_triangle with ao_i=256 (fully lit).
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_fastras_test\n");

    /* Clear s_shadow to black and coverage mask to 0 (0 = nothing drawn yet) */
    for (int i = 0; i < FB_PIXELS; i++) s_shadow[i] = 0u;
    memset(s_cover, 0, sizeof(s_cover));

    /* Put a "close" colour in atlas slot (0,0): mk_rgba(255,0,0)=0xFF0000FF.
     * In memory (little-endian, SDL ABGR8888): byte0=R=255, byte1=G=0, byte2=B=0.
     * C# test checks px[centre]=255 (R channel). */
    for (int py = 0; py < TILE_SIZE; py++)
        for (int px = 0; px < TILE_SIZE; px++)
            s_atlas[py * ATLAS_W + px] = mk_rgba(255, 0, 0);  /* red: byte0=255 */

    /* Triangle covers centre (160,100):
     *   v0=(270,170), v1=(50,170), v2=(160,30)  →  area < 0 (front face)
     */
    PVert v0, v1, v2;
    v0.sx = 270; v0.sy = 170; v0.u = 0.5f; v0.v = 0.9f;
    v1.sx =  50; v1.sy = 170; v1.u = 0.1f; v1.v = 0.9f;
    v2.sx = 160; v2.sy =  30; v2.u = 0.3f; v2.v = 0.1f;
    draw_triangle(&v0, &v1, &v2, 0, 0, 256, 0 /* fog_i: no fog */);

    /* Blit shadow → FB */
    memcpy((void*)FB_BASE, s_shadow, FB_PIXELS * sizeof(uint32_t));

    /* Read centre pixel from MMIO FB */
    volatile uint8_t *fb = (volatile uint8_t*)0x20000000u;
    int cidx = (100 * FB_WIDTH + 160) * 4;
    /* byte0=255 (SDL Abgr8888 R channel) from mk_rgba(0,0,255) */
    printf("fastras_drawn: %s\n", (fb[cidx+0] > 100) ? "OK" : "FAIL");

    /* Coverage bit at centre: pixel should be marked as drawn */
    int centre_pix2 = 100 * FB_WIDTH + 160;
    int cov = ((s_cover[centre_pix2 >> 5] >> (centre_pix2 & 31)) & 1u) != 0;
    printf("fastras_depth: %s\n", cov ? "OK" : "FAIL");

    /* Corner (0,0) outside the triangle must stay black */
    printf("fastras_corner: %s\n",
           (fb[0] == 0 && fb[1] == 0 && fb[2] == 0) ? "OK" : "FAIL");

    /* Z-test: a farther triangle (cw=100) must NOT overwrite the close pixels */
    /* Put a different colour (mk_rgba(0,0,255)) in atlas for the far pass.
     * That stores byte0=0, byte2=255 — opposite of the close colour. */
    for (int py = 0; py < TILE_SIZE; py++)
        for (int px = 0; px < TILE_SIZE; px++)
            s_atlas[py * ATLAS_W + px] = mk_rgba(0, 0, 255);

    PVert w0, w1, w2;
    w0.sx = 270; w0.sy = 170; w0.u = 0.5f; w0.v = 0.9f;
    w1.sx =  50; w1.sy = 170; w1.u = 0.1f; w1.v = 0.9f;
    w2.sx = 160; w2.sy =  30; w2.u = 0.3f; w2.v = 0.1f;
    draw_triangle(&w0, &w1, &w2, 0, 0, 256, 0 /* fog_i: no fog */);

    memcpy((void*)FB_BASE, s_shadow, FB_PIXELS * sizeof(uint32_t));

    /* Centre must still have byte0=255 (close) not overwritten by far (byte0=0) */
    printf("fastras_ztest: %s\n",
           (fb[cidx+0] > 100 && fb[cidx+2] < 50) ? "OK" : "FAIL");

    return 0;
}
