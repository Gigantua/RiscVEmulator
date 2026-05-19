/* voxel_render_test.c — Smoke test: render one frame, verify non-sky pixels.
 *
 * Sets up a minimal world (one grass block directly in front of the camera),
 * renders a single frame via clear_screen + emit_face, then checks that at
 * least one pixel in the centre region differs from the sky colour.
 *
 * This tests the full pipeline: MVP → vert_shader → draw_triangle → frag_shader.
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_render_test\n");

    /* ── Build a minimal scene ── */
    build_atlas();

    /* Place one grass block at (64, 32, 64) */
    s_world[64][32][64] = BLOCK_GRASS;
    /* Air above it (already zero) */

    /* ── Camera: positioned above+behind the block, looking at its top face ──
     *  eye  = (64.5, 35, 64.5)  — 3 blocks above block top (y=33)
     *  look = (64.5, 32, 64.5)  — aimed at block top
     */
    Vec3 eye  = { 64.5f, 35.0f, 64.5f };
    Vec3 look = { 64.5f, 33.0f, 64.5f };
    Vec3 up   = {  0.0f,  0.0f, -1.0f }; /* -Z up because we're looking straight down */

    Mat4 proj, view, mvp;
    mat4_perspective(proj, 1.0472f /* 60° FOV */,
                     (float)FB_WIDTH / (float)FB_HEIGHT,
                     0.1f, 100.0f);
    mat4_lookat(view, eye, look, up);
    mat4_mul(mvp, proj, view);
    memcpy(s_mvp, mvp, sizeof(Mat4));

    /* ── Render ── */
    clear_screen();
    emit_face(64, 32, 64, FACE_TOP, BLOCK_GRASS, 0 /* fog_i: no fog */);

    /* Blit shadow buffer → MMIO framebuffer so pixel reads below work */
    memcpy((void*)FB_BASE, s_shadow, FB_PIXELS * sizeof(uint32_t));
    DISP_VSYNC_REG = 1;

    /* ── Sample pixels around screen centre ── */
    volatile uint8_t *fb = (volatile uint8_t*)0x20000000u;
    int cx = FB_WIDTH / 2, cy = FB_HEIGHT / 2;

    /* Sky colour at the centre row (from clear_screen) */
    float t_sky = (float)cy / FB_HEIGHT;
    int sky_r = (int)(140 + t_sky * 60);
    /* sky_g = (int)(180 + t_sky * 40); */

    /* Count pixels in a 40×40 window around centre that differ from sky */
    int block_pixels = 0;
    for (int dy = -20; dy <= 20; dy++) {
        for (int dx = -20; dx <= 20; dx++) {
            int px = cx + dx, py = cy + dy;
            if (px < 0 || px >= FB_WIDTH || py < 0 || py >= FB_HEIGHT) continue;
            int idx = (py * FB_WIDTH + px) * 4;
            uint8_t r = fb[idx + 0];
            uint8_t g = fb[idx + 1];
            /* Grass top is ~greenish; sky_r at y=100 is ~170 which is clearly different */
            int is_sky_like = (r > sky_r - 20 && r < sky_r + 20);
            if (!is_sky_like) block_pixels++;
        }
    }

    printf("block_pixels_in_centre=%d\n", block_pixels);
    printf("render_nonsky: %s\n", block_pixels > 10 ? "OK" : "FAIL");

    /* Print raw centre pixel for diagnostics */
    int cidx = (cy * FB_WIDTH + cx) * 4;
    printf("centre_px: R=%u G=%u B=%u\n",
           fb[cidx+0], fb[cidx+1], fb[cidx+2]);

    return 0;
}
