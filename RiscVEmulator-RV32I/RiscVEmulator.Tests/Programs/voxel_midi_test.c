/* voxel_midi_test.c — Verify MIDI sound-effect helpers run without crashing.
 *
 * Calls each sfx_* function and verifies clean exit.
 * In the test harness no MidiDevice is registered, so MMIO writes are silently
 * dropped — this tests that the MIDI code path itself is correct and doesn't
 * cause a fault, hang, or assert failure.
 */
#define VOXEL_NO_MAIN
#include "voxel_main.c"

int main(void) {
    printf("voxel_midi_test\n");

    /* Exercise all MIDI helpers */
    sfx_block_break();
    printf("midi_break: OK\n");

    sfx_block_place();
    printf("midi_place: OK\n");

    sfx_jump();
    printf("midi_jump: OK\n");

    sfx_footstep();
    printf("midi_footstep: OK\n");

    /* Raw note on/off cycle */
    midi_program(SFX_CH, 40); /* Violin */
    midi_note_on(SFX_CH, 60, 100);
    midi_note_off(SFX_CH, 60);
    printf("midi_raw: OK\n");

    return 0;
}
