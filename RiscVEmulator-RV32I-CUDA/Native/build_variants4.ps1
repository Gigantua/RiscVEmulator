# Batch 4 — doc-grounded host-side cache ideas (L1 carveout, L2 persistence). All include the now-
# default INCBR fusion, so they compare directly against variants\incbr / the shipped default.
$ErrorActionPreference="Stop"
$root="C:\work\RiscV\RiscVEmulator\RiscVEmulator-RV32I-CUDA"; $vc="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$src="$root\Native\rv32i_cuda.cu"; $binsrc="$root\Examples\CudaBench\bin\x64\Release\net10.0"; $vardir="$root\Native\variants"
$ideas=[ordered]@{
  "default_incbr" = ""                                   # current shipped default (INCBR on) — reference
  "carveout_l1"   = "-DIDEA_CARVEOUT_L1"
  "l2_persist"    = "-DIDEA_L2_PERSIST"
  "l2_carve"      = "-DIDEA_CARVEOUT_L1 -DIDEA_L2_PERSIST"
}
New-Item -ItemType Directory -Force -Path $vardir|Out-Null
foreach($name in $ideas.Keys){ $defs=$ideas[$name]; $dll="$vardir\rv32i_cuda_$name.dll"; Write-Output "=== $name ($defs) ==="
  $log=cmd /c "call `"$vc`" >nul 2>&1 && nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 $defs -diag-suppress 549 -shared -cudart static -o `"$dll`" `"$src`" 2>&1"
  if($log|Select-String "error"){Write-Output "  FAILED"; $log|Select-String "error"|%{Write-Output "    $_"}; continue}
  $dest="$vardir\$name"; New-Item -ItemType Directory -Force -Path $dest|Out-Null
  Copy-Item "$binsrc\*.exe","$binsrc\*.dll","$binsrc\*.json" $dest -Force; Copy-Item $dll "$dest\rv32i_cuda.dll" -Force; Write-Output "  OK" }
Write-Output "done"
