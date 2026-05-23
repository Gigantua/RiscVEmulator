# Build the arx_smoke.c guest with the new clang (must include the +arx
# patches) and run it on the host emulator. Runs in WSL.
$ErrorActionPreference = "Stop"

$src    = "C:\work\RiscV\RiscVEmulator\RiscVEmulator-RC4\RiscVEmulator.Tests\Programs\arx_smoke.c"
$srcWsl = "/mnt/c/work/RiscV/RiscVEmulator/RiscVEmulator-RC4/RiscVEmulator.Tests/Programs/arx_smoke.c"
$elf    = "/tmp/arx_smoke.elf"

# Build with the freshly-rebuilt clang in ~/llvm-build (NOT the installed
# ~/llvm-rv32i one — that's still the old binary until build-llvm.sh runs).
$wslCmd = "~/llvm-build/bin/clang " +
          "--target=riscv32-unknown-elf -march=rv32i+arx -mabi=ilp32 " +
          "-nostdlib -O2 -fuse-ld=lld -ffreestanding -static -Wl,-Ttext=0 " +
          "$srcWsl -o $elf && " +
          "~/llvm-build/bin/llvm-objdump -d --mattr=+arx --triple=riscv32 $elf | grep -A2 '<_start>:' | head -20 && " +
          "echo --- && file $elf"

wsl -- bash -c "$wslCmd"

# Then copy the ELF out and run it on the host emulator via the existing
# probe machinery.
Copy-Item "\\wsl$\Ubuntu-24.04\tmp\arx_smoke.elf" "C:\Users\Daniel\AppData\Local\Temp\arx_smoke.elf" -Force
"ELF copied to: C:\Users\Daniel\AppData\Local\Temp\arx_smoke.elf"
