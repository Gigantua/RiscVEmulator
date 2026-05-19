/* voxel_worldgen_test.c — Verify gen_world() places correct block types.
 *
 * After generation, we scan a vertical column near the world centre and verify:
 *   • Some column has BLOCK_GRASS as its topmost block
 *   • The block directly below BLOCK_GRASS is BLOCK_DIRT
 *   • Stone exists somewhere deeper in the world
 *   • The world is not all-air
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_worldgen_test\n");

    gen_world();

    /* Scan multiple columns around (64,z,64) to find one with a grass top */
    int found_grass_col = 0;
    int grass_x = -1, grass_z = -1, grass_y = -1;

    for (int tx = 56; tx <= 72 && !found_grass_col; tx++) {
        for (int tz = 56; tz <= 72 && !found_grass_col; tz++) {
            /* Find topmost non-air block */
            for (int ty = WY - 1; ty >= 1; ty--) {
                if (s_world[tx][ty][tz] == BLOCK_GRASS) {
                    found_grass_col = 1;
                    grass_x = tx; grass_y = ty; grass_z = tz;
                    break;
                }
                if (s_world[tx][ty][tz] != BLOCK_AIR) break; /* different block on top */
            }
        }
    }

    printf("worldgen_grass: %s\n", found_grass_col ? "OK" : "FAIL");
    if (found_grass_col)
        printf("  grass at (%d,%d,%d)\n", grass_x, grass_y, grass_z);

    /* Block below grass should be BLOCK_DIRT */
    int dirt_ok = 0;
    if (found_grass_col && grass_y >= 1)
        dirt_ok = (s_world[grass_x][grass_y - 1][grass_z] == BLOCK_DIRT);
    printf("worldgen_dirt_below: %s\n", dirt_ok ? "OK" : "FAIL");

    /* Stone should exist somewhere below */
    int found_stone = 0;
    for (int tx = 56; tx <= 72 && !found_stone; tx++)
        for (int tz = 56; tz <= 72 && !found_stone; tz++)
            for (int ty = 0; ty < 10; ty++)
                if (s_world[tx][ty][tz] == BLOCK_STONE) { found_stone = 1; break; }
    printf("worldgen_stone: %s\n", found_stone ? "OK" : "FAIL");

    /* World must not be all-air */
    int any_solid = 0;
    for (int ty = 0; ty < WY && !any_solid; ty++)
        if (s_world[64][ty][64] != BLOCK_AIR) any_solid = 1;
    printf("worldgen_not_empty: %s\n", any_solid ? "OK" : "FAIL");

    return 0;
}
