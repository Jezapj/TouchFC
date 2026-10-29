# M8: repeat --selftest with telemetry and print p50 / p99 per stage.
# powershell -File scripts\telemetry_runs.ps1 -Exe build\windows-msvc\app\Release\touchfc.exe -Tag base [-Pin] [-Runs 3] [-Seconds 20]
param(
  [Parameter(Mandatory)] [string]$Exe,
  [Parameter(Mandatory)] [string]$Tag,
  [switch]$Pin,
  [int]$Runs = 3,
  [int]$Seconds = 20
)
$ErrorActionPreference = 'Stop'
$out = 'build\telemetry_runs'
New-Item -ItemType Directory -Force $out | Out-Null
for ($i = 1; $i -le $Runs; ++$i) {
  $csv = "$out\$Tag-$i.csv"
  $a = @('--selftest', '--seconds', "$Seconds", '--mini-replay', '0', '--telemetry', $csv)
  if ($Pin) { $a += '--pin' }
  $p = Start-Process -FilePath $Exe -ArgumentList $a -PassThru -Wait -WindowStyle Hidden
  if ($p.ExitCode -ne 0) { throw "run $i exit $($p.ExitCode)" }
  Import-Csv $csv | ForEach-Object {
    '{0},{1},{2},{3:N3},{4:N3}' -f $Tag, $i, $_.stage, ($_.p50_ns / 1e6), ($_.p99_ns / 1e6)
  }
}
