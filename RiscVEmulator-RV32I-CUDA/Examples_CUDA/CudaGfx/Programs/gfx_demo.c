// gfx_demo.c — freestanding RV32I framebuffer demo. Draws an animated plasma
// directly to the MMIO framebuffer at 0x20000000 every frame using only
// integer add/xor/shift (no mul/div/float → no libcalls, no runtime needed),
// so it renders fast enough to be visibly smooth even on a single GPU thread.

#include <stdint.h>

#define FBP   ((volatile uint32_t*)(uintptr_t)0x20000000u)  // framebuffer (ABGR8888)
#define DISPP ((volatile uint32_t*)(uintptr_t)0x20100000u)  // display control
#define W 320
#define H 200

void _start(void) {
    DISPP[7] = 0;            // DISP_FB_ADDR (+0x1C) = 0 → present the FB directly
    uint32_t f = 0;
    for (;;) {
        uint32_t off = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint8_t r = (uint8_t)(x + f);
                uint8_t g = (uint8_t)(y + (f >> 1));
                uint8_t b = (uint8_t)((x ^ y) + f);
                FBP[off + x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
            }
            off += W;        // avoid y*W (keeps it multiply-free)
        }
        DISPP[3] = 1;        // DISP_VSYNC (+0x0C) = 1 → present this frame
        f++;
    }
}
