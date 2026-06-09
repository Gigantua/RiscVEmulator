# Measure every built variant — RUN ONLY WHEN THE GPU IS FREE. Dumps raw gate + bench lines.
$vardir = "C:\work\RiscV\RiscVEmulator\RiscVEmulator-RV32I-CUDA\Native\variants"
foreach ($d in Get-ChildItem $vardir -Directory | Sort-Object Name) {
  $exe = "$($d.FullName)\Examples.CudaBench.exe"
  if (-not (Test-Path $exe)) { continue }
  $g = (& $exe --rvcud 2>&1 | Select-String -Pattern "bit-identical|FAIL|mismatch")
  $gate = if ($g -match "bit-identical") { "PASS" } else { "FAIL" }
  $b = & $exe --bench 2>&1
  $cl = ($b | Where-Object { $_ -match '^\s+compute' }) -replace '\s+',' '
  $dl = ($b | Where-Object { $_ -match '^\s+data' })    -replace '\s+',' '
  $vl = ($b | Where-Object { $_ -match '^\s+diverge' }) -replace '\s+',' '
  Write-Output ("{0,-12} gate={1}" -f $d.Name, $gate)
  Write-Output ("   $cl")
  Write-Output ("   $dl")
  Write-Output ("   $vl")
}
