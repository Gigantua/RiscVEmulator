// Examples.Linux — RV64GC Linux boot harness.
//
// The emulator is RV64-only. Everything dispatches to the RV64GC boot path
// (Rv64Boot): a real Alpine riscv64 rootfs, or a bare-metal RV64 flat binary
// via `--rv64-bin <file>`. See Rv64Boot.cs for the supported options.
//
// The former RV32 mini-rv32ima nommu machine and the Sv32 MMU boot (MmuBoot)
// were removed together with the RV32 native core — the RV64GC core with its
// Sv39 MMU and SBI firmware is the single supported target.
return Examples.Linux.Rv64Boot.Run(args);
