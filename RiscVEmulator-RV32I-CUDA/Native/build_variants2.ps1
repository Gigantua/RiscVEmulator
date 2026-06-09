# Batch 2 — build the newer ideas into the same variants\ folder (does NOT rebuild batch 1).
$ErrorActionPreference = "Stop"
$root   = "C:\work\RiscV\RiscVEmulator\RiscVEmulator-RV32I-CUDA"
$vc     = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$src    = "$root\Native\rv32i_cuda.cu"
$binsrc = "$root\Examples\CudaBench\bin\x64\Release\net10.0"
$vardir = "$root\Native\variants"

$ideas = [ordered]@{
  "addadd"   = "-DIDEA_ADDADD"
  "brcmp"    = "-DIDEA_BRCMP"
  "ldg_lu"   = "-DIDEA_LDG_LU"
  "fusions2" = "-DIDEA_ADDADD -DIDEA_BRCMP"
  "allfuse"  = "-DIDEA_INCBR -DIDEA_ROT -DIDEA_XSHADD -DIDEA_ANDSH -DIDEA_ADDADD -DIDEA_BRCMP"
  "block16"  = "-DRVCUD_BLOCK=16"
  "block24"  = "-DRVCUD_BLOCK=24"
  "block40"  = "-DRVCUD_BLOCK=40"
  "block56"  = "-DRVCUD_BLOCK=56"
  "block72"  = "-DRVCUD_BLOCK=72"
  "block512" = "-DRVCUD_BLOCK=512"
  "stride48" = "-DREGSTRIDE=48"
  "stride64" = "-DREGSTRIDE=64"
  "lb96"     = "-DRVCUD_LB=96"
  "lb160"    = "-DRVCUD_LB=160"
  "lb320"    = "-DRVCUD_LB=320"
  "unroll3"  = "-DRVCUD_UNROLL=3"
  "unroll16" = "-DRVCUD_UNROLL=16"
}
New-Item -ItemType Directory -Force -Path $vardir | Out-Null
foreach ($name in $ideas.Keys) {
  $defs = $ideas[$name]; $dll = "$vardir\rv32i_cuda_$name.dll"
  Write-Output "=== building $name ($defs) ==="
  $log = cmd /c "call `"$vc`" >nul 2>&1 && nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 $defs -diag-suppress 549 -shared -cudart static -o `"$dll`" `"$src`" 2>&1"
  $err = $log | Select-String -Pattern "error"
  if ($err) { Write-Output "  BUILD FAILED:"; $err | ForEach-Object { Write-Output "    $_" }; continue }
  $dest = "$vardir\$name"; New-Item -ItemType Directory -Force -Path $dest | Out-Null
  Copy-Item "$binsrc\*.exe","$binsrc\*.dll","$binsrc\*.json" $dest -Force
  Copy-Item $dll "$dest\rv32i_cuda.dll" -Force
  Write-Output "  OK -> $dest"
}
Write-Output "done"
