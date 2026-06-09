param([string]$defs = "")
$ErrorActionPreference = "Stop"
$root = "C:\work\RiscV\RiscVEmulator\RiscVEmulator-RV32I-CUDA"
$vc = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
$out = "$root\Native\bin\Release\rv32i_cuda.dll"
$src = "$root\Native\rv32i_cuda.cu"
cmd /c "call `"$vc`" >nul 2>&1 && nvcc -O3 -std=c++20 --expt-relaxed-constexpr -arch=sm_86 $defs -diag-suppress 549 -shared -cudart static -o `"$out`" `"$src`" 2>&1" | Select-String -Pattern "error" -Context 0,2
Copy-Item $out "$root\Examples\CudaBench\bin\x64\Release\net10.0\rv32i_cuda.dll" -Force
& "$root\Examples\CudaBench\bin\x64\Release\net10.0\Examples.CudaBench.exe" --bench 2>&1 | Select-Object -Last 5
