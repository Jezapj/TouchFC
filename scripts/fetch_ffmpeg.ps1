# Downloads the official BtbN LGPL shared FFmpeg build into third_party/ffmpeg.
# The GPL build is intentionally not used. Re-run is a no-op when avcodec.h exists.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dest = Join-Path $root 'third_party\ffmpeg'
$header = Join-Path $dest 'include\libavcodec\avcodec.h'
if (Test-Path $header) {
  Write-Host "FFmpeg already present at $dest"
  exit 0
}
$zip = Join-Path $root 'third_party\downloads\ffmpeg-lgpl-shared.zip'
New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
$url = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip'
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
$extract = Join-Path $root 'third_party\downloads\ffmpeg-extract'
if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
Expand-Archive -Path $zip -DestinationPath $extract -Force
$inner = Get-ChildItem $extract -Directory | Select-Object -First 1
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Move-Item $inner.FullName $dest
Write-Host "FFmpeg extracted to $dest"
