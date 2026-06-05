/* voxel_physics_test.c — Verify player gravity, landing, and jump.
 *
 * Tests:
 *   1. Gravity:   player in mid-air accelerates downward each update
 *   2. Landing:   player stops at y=6 when floor at y=5
 *   3. on_ground: flag set after landing
 *   4. Jump:      setting SC_SPACE key applies upward velocity from ground
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_physics_test\n");

    /* Place a solid floor one block thick at y=5, x=[60..70], z=[60..70] */
    for (int tx = 60; tx <= 70; tx++)
        for (int tz = 60; tz <= 70; tz++)
            s_world[tx][5][tz] = BLOCK_STONE;

    /* Position player 10 units above the floor */
    s_player.pos   = (Vec3){ 65.5f, 10.0f, 65.5f };
    s_player.vel   = (Vec3){ 0.0f, 0.0f, 0.0f };
    s_player.yaw   = 0.0f;
    s_player.pitch = 0.0f;
    s_player.on_ground = 0;
    memset(s_keys, 0, sizeof(s_keys));

    /* Run physics until player lands (max 30 steps) */
    float start_y = s_player.pos.y;
    int landed = 0;
    for (int i = 0; i < 30 && !landed; i++) {
        player_update(0.1f);
        if (s_player.on_ground) landed = 1;
    }

    printf("physics_gravity: %s\n", s_player.pos.y < start_y ? "OK" : "FAIL");
    printf("physics_landed: %s\n",  landed ? "OK" : "FAIL");

    /* After landing, player should be at y ≈ 6 (one above the floor at y=5) */
    int land_y_ok = (s_player.pos.y >= 5.9f && s_player.pos.y <= 6.1f);
    printf("physics_land_height: %s (y=%.2f)\n", land_y_ok ? "OK" : "FAIL", s_player.pos.y);

    printf("physics_on_ground: %s\n", s_player.on_ground ? "OK" : "FAIL");

    /* Velocity should be near zero after landing */
    int vel_stopped = (s_player.vel.y > -0.1f && s_player.vel.y < 0.1f);
    printf("physics_vel_zero: %s (vel.y=%.2f)\n", vel_stopped ? "OK" : "FAIL", s_player.vel.y);

    /* Jump: press SPACE while on_ground — should give positive upward velocity */
    s_keys[SC_SPACE] = 1;
    player_update(0.01f);
    s_keys[SC_SPACE] = 0;
    printf("physics_jump_vel: %s (vel.y=%.2f)\n",
           s_player.vel.y > 5.0f ? "OK" : "FAIL", s_player.vel.y);
    printf("physics_jump_airborne: %s\n", !s_player.on_ground ? "OK" : "FAIL");

    return 0;
}
