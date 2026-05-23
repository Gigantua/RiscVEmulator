# Examples/Quake — TyrQuake on the RV32I emulator

Status: **scaffolding + bare-metal port in progress**. 90/91 source files
compile cleanly; the link still pulls a handful of small undefined symbols
that need shimming (see "Remaining link gaps" below). Once link succeeds,
the next step is iterating on first-frame correctness — expect single-digit
FPS without an F-extension because every `float` op is a softfloat libcall.

## Layout

```
Examples/Quake/
  Examples.Quake.csproj      — .NET host that compiles + boots the ELF
  Program.cs                  — clang build orchestration; SoC setup;
                                streams pak0.pak via DiskDevice
  Programs/
    quake_main.c              — _start; spins Host_Init / Host_Frame
    vid_rvemu.c               — palette-converts 320×200 indexed → FB RGBA
    snd_rvemu.c               — DMA-style sound into AudioBufferDevice
    in_rvemu.c                — KeyboardDevice + MouseDevice polling
    sys_rvemu.c               — RTC/UART/HostExit + Sys_File* over DiskDevice
    stubs_rvemu.c             — libc / softfloat / sound-music link shims
    rvemu_config.h            — clang `-include`'d before every TU
    setjmp.h, inttypes.h
    dirent.h, sys/stat.h, sys/types.h, arpa/inet.h
  tyrquake/                   — vendored from https://github.com/sezero/tyrquake
                                (full source; we curate a subset in Program.cs)
```

## Build

```
git clone https://github.com/sezero/tyrquake Examples/Quake/tyrquake
# Build the .NET solution (Native + Quake)
MSBuild RiscVEmulator.sln -p:Platform=x64 -p:Configuration=Release
# First run will:
#  - clang-compile ~90 TyrQuake TUs + our shims + the Runtime
#  - link into Examples/Quake/bin/Release/net10.0/build/quake.elf
#  - boot the emulator
dotnet run --project Examples/Quake -c Release
```

Drop the freely-redistributable Quake shareware `pak0.pak` (≈18.6 MiB) at
`Examples/Quake/bin/Release/net10.0/id1/pak0.pak` before running (or pass
`--pak <path>`).

## Remaining link gaps (1-day grind)

These were the last few undefined symbols when we stopped. All are small
shims of the same shape as the ones already in `stubs_rvemu.c`:

* `__multf3 / __addtf3 / __subtf3 / __divtf3 / __extendsftf2 / __extenddftf2 / __trunctfsf2 / __trunctfdf2`
  — IEEE-754 quad-precision (long double) softfloat helpers. We
  *should* be able to avoid these by ensuring TyrQuake never actually
  uses `long double` (suspect: one of `snd_dma.c` / `mathlib.c`). If
  they're truly needed, route them through doubles (lossy) or pull in
  `compiler-rt`'s quad-prec helpers.
* A handful of misc stubs surface as we close them — keep adding into
  `stubs_rvemu.c` with the exact signatures from the header that
  declares them. ~5-10 remaining when work paused.

After link succeeds, runtime issues to expect:

* `Host_Init` will probe the pak file via `Sys_FileOpenRead`; our
  `sys_rvemu.c` only resolves the basename `pak0.pak` — extend to
  reject everything else cleanly.
* `Host_Frame` is the first place softfloat-by-libcall dominates the
  profile. Expect 1-3 FPS at 320×200 in non-trivial scenes on the
  JIT'd CPU. That's an intrinsic property of "no F-ext" and the user
  has accepted this.

## Why this is harder than DOOM was

DOOM ran via a single-header amalgamation (`PureDOOM.h`) that was already
deliberately port-friendly: fixed-point math, narrow libc surface. Quake
is "real" mid-90s C — it pulls in `inttypes.h`, `setjmp.h`, `dirent.h`,
`sys/stat.h`, `arpa/inet.h`; uses `long double` somewhere; expects a
proper sound mixer architecture (`snd_dma.c` + driver); and the network
loopback driver is not optional even for single-player. Each of those
expectations becomes a stub or a header shim in this directory.
