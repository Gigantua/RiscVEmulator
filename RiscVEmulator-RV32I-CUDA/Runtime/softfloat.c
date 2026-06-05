/* softfloat.c — IEEE 754 software floating-point for rvemu guests.
 *
 * Thin amalgamation: pulls in Bellard's softfp library (vendored from
 * TinyEMU under Runtime/softfp/) and a libgcc/compiler-rt ABI shim.
 * Bellard's softfp is ~1500 lines across 4 files, IEEE 754 conformant,
 * and is the floating-point backend used by TinyEMU + several RISC-V
 * reference emulators. It replaces a 600-line hand-rolled impl that
 * had at least three IEEE violations (__gesf2(NaN, .) sign,
 * inf - inf, inf / inf) which produced subtle corruption downstream
 * — notably hanging TyrQuake's R_ScanEdges active-surface sort.
 *
 * The amalgamation pattern keeps the single-file contract every build
 * (EmulatorTestBase, Examples/Quake/Program.cs, examples) depends on:
 * compile Runtime/softfloat.c to one .o and link it. Callers must also
 * pass `-IRuntime/softfp -include Runtime/softfp/rvemu_softfp_force.h`
 * so the bare-metal build sees Bellard's local headers and gets NDEBUG
 * defined before <assert.h>. */
#include "softfp/softfp.c"
#include "softfp/softfp_shim.c"
