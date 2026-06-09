# Build one CudaBench exe per rvcud idea (each idea behind its own -D macro).
# Does NOT run anything — produces variants\<name>\ each with the exe + that idea's DLL,
# so they can be benchmarked later (when the GPU is free) via:
#     variants\<name>\Examples.CudaBench.exe --bench      (and --rvcud for the correctness gate)
$ErrorActionPreference = "Stop"
$root   = "C:\work\RiscV\RiscVEmulator\RiscVEmulator-RV32I-CUDA"
$vc     = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$src    = "$root\Native\rv32i_cuda.cu"
$binsrc = "$root\Examples\CudaBench\bin\x64\Release\net10.0"
$vardir = "$root\Native\variants"

# name => extra nvcc defines (baseline = current kept wins, no extra defines)
$ideas = [ordered]@{
  "baseline"   = ""
  # fusions (translator)
  "incbr"      = "-DIDEA_INCBR"
  "rot"        = "-DIDEA_ROT"
  "xshadd"     = "-DIDEA_XSHADD"
  "andsh"      = "-DIDEA_ANDSH"
  "fusions"    = "-DIDEA_INCBR -DIDEA_ROT -DIDEA_XSHADD -DIDEA_ANDSH"
  # loop unroll factor
  "unroll2"    = "-DRVCUD_UNROLL=2"
  "unroll4"    = "-DRVCUD_UNROLL=4"
  "unroll8"    = "-DRVCUD_UNROLL=8"
  # thread-block size
  "block32"    = "-DRVCUD_BLOCK=32"
  "block48"    = "-DRVCUD_BLOCK=48"
  "block96"    = "-DRVCUD_BLOCK=96"
  "block128"   = "-DRVCUD_BLOCK=128"
  "block160"   = "-DRVCUD_BLOCK=160"
  "block192"   = "-DRVCUD_BLOCK=192"
  "block256"   = "-DRVCUD_BLOCK=256"
  # shared-regfile stride (33 = conflict-free)
  "stride32"   = "-DREGSTRIDE=32"
  "stride34"   = "-DREGSTRIDE=34"
  "stride36"   = "-DREGSTRIDE=36"
  "stride40"   = "-DREGSTRIDE=40"
  # __launch_bounds__ hint
  "lb64"       = "-DRVCUD_LB=64"
  "lb128"      = "-DRVCUD_LB=128"
  "lb192"      = "-DRVCUD_LB=192"
  "lb512"      = "-DRVCUD_LB=512"
  # fetch / cache
  "prefetch"   = "-DIDEA_PREFETCH"
  "fetch64"    = "-DIDEA_FETCH64"
  "ldg_cs"     = "-DIDEA_LDG_CS"
  # writeback form
  "wb_selp"    = "-DIDEA_WB_SELP"
  # combinations
  "combo_all"  = "-DIDEA_INCBR -DIDEA_ROT -DIDEA_XSHADD -DIDEA_ANDSH -DIDEA_PREFETCH"
  "combo_b128" = "-DIDEA_INCBR -DIDEA_ROT -DIDEA_XSHADD -DIDEA_ANDSH -DRVCUD_BLOCK=128"
}

New-Item -ItemType Directory -Force -Path $vardir | Out-Null
foreach ($name in $ideas.Keys) {
  $defs = $ideas[$name]
  $dll  = "$vardir\rv32i_cuda_$name.dll"
  Write-Output "=== building $name ($defs) ==="
  $log = cmd /c "call `"$vc`" >nul 2>&1 && nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 $defs -diag-suppress 549 -shared -cudart static -o `"$dll`" `"$src`" 2>&1"
  $err = $log | Select-String -Pattern "error"
  if ($err) { Write-Output "  BUILD FAILED:"; $err | ForEach-Object { Write-Output "    $_" }; continue }
  $dest = "$vardir\$name"
  New-Item -ItemType Directory -Force -Path $dest | Out-Null
  Copy-Item "$binsrc\*.exe","$binsrc\*.dll","$binsrc\*.json" $dest -Force
  Copy-Item $dll "$dest\rv32i_cuda.dll" -Force
  Write-Output "  OK -> $dest\Examples.CudaBench.exe"
}
Write-Output "`nDone. To benchmark later (GPU free):"
Write-Output "  foreach (`$d in Get-ChildItem '$vardir' -Directory) { Write-Output `$d.Name; & `"`$(`$d.FullName)\Examples.CudaBench.exe`" --bench | Select-Object -Last 4 }"
