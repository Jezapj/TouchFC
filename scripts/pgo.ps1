# M8: profile-guided build of touchfc.exe in build\pgo.
#   powershell -File scripts\pgo.ps1 [-Seconds 30]
# 1. instrumented build (TOUCHFC_PGO=GEN)
# 2. training: --selftest, and --replay on each test_clips\*.mp4 if present
#    (stub detector, so the profile is our code, not ONNX Runtime)
# 3. optimised relink from the .pgc files (TOUCHFC_PGO=USE)
param([int]$Seconds = 30, [string[]]$CMakeArgs = @())
$ErrorActionPreference = 'Continue'  # cmake warns on stderr; exit codes are checked
$dir = 'build\pgo'
$exe = "$dir\app\Release\touchfc.exe"

cmake --preset windows-msvc -B $dir -DTOUCHFC_PGO=GEN @CMakeArgs
if ($LASTEXITCODE) { throw 'configure (GEN) failed' }
Get-ChildItem $dir -Filter '*.pgc' -Recurse -ErrorAction SilentlyContinue | Remove-Item
cmake --build $dir --config Release --target touchfc
if ($LASTEXITCODE) { throw 'build (GEN) failed' }

$runs = @(, @('--selftest', '--seconds', "$Seconds", '--mini-replay', '1'))
foreach ($clip in Get-ChildItem test_clips -Filter *.mp4 -ErrorAction SilentlyContinue) {
  $runs += , @('--replay', $clip.FullName, '--seconds', "$Seconds", '--mini-replay', '1')
}
foreach ($a in $runs) {
  Write-Host "training: $($a -join ' ')"
  $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -Wait -WindowStyle Hidden
  if ($p.ExitCode -ne 0) { throw "training run exit $($p.ExitCode)" }
}

cmake --preset windows-msvc -B $dir -DTOUCHFC_PGO=USE @CMakeArgs
if ($LASTEXITCODE) { throw 'configure (USE) failed' }
cmake --build $dir --config Release --target touchfc
if ($LASTEXITCODE) { throw 'build (USE) failed' }
Write-Host "PGO build: $exe"
