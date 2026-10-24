# PresentMon reports present-to-photon time. TouchFC's HUD reports
# capture-to-present-call only. Run PresentMon against a live touchfc.exe.
$ErrorActionPreference = 'Stop'
$names = @(
  'PresentMon.exe',
  "$env:ProgramFiles\Intel\PresentMon\PresentMon.exe",
  "${env:ProgramFiles(x86)}\Intel\PresentMon\PresentMon.exe"
)
$found = $null
foreach ($n in $names) {
  $cmd = Get-Command $n -ErrorAction SilentlyContinue
  if ($cmd) { $found = $cmd.Source; break }
  if (Test-Path $n) { $found = $n; break }
}
if (-not $found) {
  Write-Host "PresentMon is not installed."
  Write-Host "Intel publishes it at https://github.com/GameTechDev/PresentMon"
  Write-Host "After installing, re-run this script. The HUD number is not glass latency."
  exit 0
}
$out = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'docs\bench\presentmon.csv'
Write-Host "PresentMon: $found"
Write-Host "Suggested command (start touchfc first):"
Write-Host "  & `"$found`" -process_name touchfc.exe -output `"$out`""
Write-Host "Compare PresentMon's MsUntilDisplayed with the HUD capture-to-present-call."
