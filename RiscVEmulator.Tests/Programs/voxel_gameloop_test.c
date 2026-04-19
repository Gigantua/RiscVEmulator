/* voxel_gameloop_test.c — Run one full game loop iteration and verify output.
 *
 * Skips gen_world() (expensive) and instead builds a hand-crafted scene:
 *   • Flat stone floor at y=5, x=[62..69], z=[60..66]
 *   • Grass cap at y=6, x=[62..69], z=[60..66]
 *   • Stone pillar at (65,7..9,61) — a wall visible straight ahead
 *
 * Camera: eye=(65.5, 8.2, 65.0), looking toward -Z (pillar is at z=61).
 *
 * Runs: build_atlas → player_init (overridden) → clear_screen + render_world
 * → vsync.  Checks framebuffer has at least one non-sky pixel near centre.
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_gameloop_test\n");

    build_atlas();

    /* Hand-crafted scene */
    for (int tx = 62; tx <= 69; tx++) {
        for (int tz = 60; tz <= 66; tz++) {
            s_world[tx][5][tz] = BLOCK_STONE;
            s_world[tx][6][tz] = BLOCK_GRASS;
        }
    }
    /* Visible wall in front of camera (camera at z=65, looking -Z) */
    for (int ty = 7; ty <= 10; ty++) {
        for (int tx = 63; tx <= 68; tx++) {
            s_world[tx][ty][61] = BLOCK_STONE;
        }
    }

    /* Build heightmap so render_column can cull below-ground Y levels */
    build_hmap();

    /* Set up player position and view directly (no player_init / gen_world) */
    s_player.pos   = (Vec3){ 65.5f, 7.5f, 65.0f };
    s_player.vel   = (Vec3){ 0, 0, 0 };
    s_player.yaw   = 0.0f;   /* looking -Z */
    s_player.pitch = -0.2f;  /* slight downward tilt */
    s_player.on_ground  = 1;
    s_player.held_block = BLOCK_STONE;
    /* Initialize trig cache (normally done by player_update each frame) */
    s_player.sin_yaw   = sinf(0.0f);
    s_player.cos_yaw   = cosf(0.0f);
    s_player.sin_pitch = sinf(-0.2f);
    s_player.cos_pitch = cosf(-0.2f);
    memset(s_keys, 0, sizeof(s_keys));

    /* Build MVP */
    Vec3 eye  = { s_player.pos.x,
                  s_player.pos.y + PLAYER_EYE_H,
                  s_player.pos.z };
    float yaw_cy = cosf(s_player.yaw), yaw_sy = sinf(s_player.yaw);
    float cpi    = cosf(s_player.pitch), spi = sinf(s_player.pitch);
    Vec3 at = { eye.x - yaw_sy*cpi, eye.y + spi, eye.z - yaw_cy*cpi };
    Vec3 up = { 0, 1, 0 };

    Mat4 proj, view, mvp;
    mat4_perspective(proj, 1.0472f, (float)FB_WIDTH / (float)FB_HEIGHT, 0.15f, 200.0f);
    mat4_lookat(view, eye, at, up);
    mat4_mul(mvp, proj, view);
    memcpy(s_mvp, mvp, sizeof(Mat4));

    /* Render one frame */
    init_sky();
    clear_screen();
    render_world();
    draw_crosshair();
    /* Blit shadow buffer → MMIO framebuffer, then signal vsync */
    memcpy((void*)FB_BASE, s_shadow, FB_PIXELS * sizeof(uint32_t));
    DISP_VSYNC_REG = 1;

    /* Count non-sky pixels in a 60×60 region around screen centre */
    volatile uint8_t *fb = (volatile uint8_t*)0x20000000u;
    int cx = FB_WIDTH / 2, cy = FB_HEIGHT / 2;
    float t_sky = (float)cy / FB_HEIGHT;
    int sky_r = (int)(140.0f + t_sky * 60.0f);

    int nonsky = 0;
    for (int dy = -30; dy <= 30; dy++) {
        for (int dx = -30; dx <= 30; dx++) {
            int px = cx + dx, py = cy + dy;
            if (px < 0 || px >= FB_WIDTH || py < 0 || py >= FB_HEIGHT) continue;
            int idx = (py * FB_WIDTH + px) * 4;
            int r = fb[idx];
            if (r < sky_r - 25 || r > sky_r + 25) nonsky++;
        }
    }

    printf("gameloop_nonsky_pixels=%d\n", nonsky);
    printf("gameloop_rendered: %s\n", nonsky > 20 ? "OK" : "FAIL");

    /* Crosshair check: pixels at cx+2, cy should be white (draw_crosshair skips i=0) */
    int arm = ((cy) * FB_WIDTH + (cx+2)) * 4;
    printf("gameloop_crosshair: %s\n",
           (fb[arm] == 255 && fb[arm+1] == 255 && fb[arm+2] == 255) ? "OK" : "FAIL");

    return 0;
}
