# Batch 3 — edge-of-range sweeps + prefetch-distance/level. Builds into variants\ (no rebuild of 1/2).
$ErrorActionPreference = "Stop"
$root="C:\work\RiscV\RiscVEmulator\RiscVEmulator-RV32I-CUDA"; $vc="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$src="$root\Native\rv32i_cuda.cu"; $binsrc="$root\Examples\CudaBench\bin\x64\Release\net10.0"; $vardir="$root\Native\variants"
$ideas=[ordered]@{
  "prefetch2"="-DIDEA_PREFETCH2"; "prefetch_l2"="-DIDEA_PREFETCH_L2"
  "block8"="-DRVCUD_BLOCK=8"; "block80"="-DRVCUD_BLOCK=80"; "block112"="-DRVCUD_BLOCK=112"
  "block144"="-DRVCUD_BLOCK=144"; "block224"="-DRVCUD_BLOCK=224"; "block384"="-DRVCUD_BLOCK=384"
  "lb32"="-DRVCUD_LB=32"; "lb384"="-DRVCUD_LB=384"; "lb1024"="-DRVCUD_LB=1024"
  "unroll1"="-DRVCUD_UNROLL=1"; "stride41"="-DREGSTRIDE=41"
}
New-Item -ItemType Directory -Force -Path $vardir | Out-Null
foreach ($name in $ideas.Keys) {
  $defs=$ideas[$name]; $dll="$vardir\rv32i_cuda_$name.dll"; Write-Output "=== $name ($defs) ==="
  $log=cmd /c "call `"$vc`" >nul 2>&1 && nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 $defs -diag-suppress 549 -shared -cudart static -o `"$dll`" `"$src`" 2>&1"
  if ($log | Select-String -Pattern "error") { Write-Output "  FAILED"; $log|Select-String "error"|%{Write-Output "    $_"}; continue }
  $dest="$vardir\$name"; New-Item -ItemType Directory -Force -Path $dest|Out-Null
  Copy-Item "$binsrc\*.exe","$binsrc\*.dll","$binsrc\*.json" $dest -Force; Copy-Item $dll "$dest\rv32i_cuda.dll" -Force
  Write-Output "  OK"
}
Write-Output "done"
