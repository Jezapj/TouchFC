# Fetches a YOLO soccer .pt and exports it to the ONNX files the
# "Test: YOLO11n soccer 960/1280" presets expect. Optional: COCO yolov8n.
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_yolo_models.ps1 -Source <url or path to best.pt>
#   ... -Coco      also make yolov8n.onnx ("Test: COCO yolov8n")
#
# Ultralytics weights are AGPL-3.0: fine for testing, not for a closed build.
# Weights land in third_party\models\ and are never committed.
param(
  [string]$Source = "",
  [int[]]$Sizes = @(960, 1280),
  [switch]$Coco
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$models = Join-Path $root "third_party\models"
$work = Join-Path $root "touchfc_train\yolo_export"
New-Item -ItemType Directory -Force $models, $work | Out-Null

if (-not $Source -and -not $Coco) {
  Write-Host "Give -Source <url or path to the soccer best.pt>, and/or -Coco."
  exit 1
}

# Python venv with ultralytics, kept in touchfc_train\ (gitignored).
$venv = Join-Path $work "venv"
$py = Join-Path $venv "Scripts\python.exe"
if (-not (Test-Path $py)) {
  Write-Host "Creating venv in $venv"
  & py -3 -m venv $venv
  if (-not $?) { python -m venv $venv }
  & $py -m pip install --upgrade pip --quiet
  & $py -m pip install --only-binary=:all: ultralytics onnx onnxslim --quiet
}

function Export-Onnx([string]$pt, [int]$size, [string]$dst) {
  Write-Host "Exporting $(Split-Path -Leaf $pt) at $size -> $dst"
  & $py -c "from ultralytics import YOLO; print(YOLO(r'$pt').export(format='onnx', imgsz=$size, opset=17))"
  if ($LASTEXITCODE -ne 0) { throw "export failed" }
  Move-Item -Force ([IO.Path]::ChangeExtension($pt, ".onnx")) $dst
}

if ($Source) {
  $pt = Join-Path $work "yolo11n_soccer.pt"
  if ($Source -match '^https?://') {
    Write-Host "Downloading $Source"
    Invoke-WebRequest -Uri $Source -OutFile $pt -UseBasicParsing
  } else {
    Copy-Item -Force $Source $pt
  }
  foreach ($s in $Sizes) { Export-Onnx $pt $s (Join-Path $models "yolo11n_soccer_$s.onnx") }
}

if ($Coco) {
  Push-Location $work
  try {
    & $py -c "from ultralytics import YOLO; YOLO('yolov8n.pt')"  # downloads the weights here
    Export-Onnx (Join-Path $work "yolov8n.pt") 640 (Join-Path $models "yolov8n.onnx")
  } finally { Pop-Location }
}

Write-Host "Done. Models in ${models}:"
Get-ChildItem $models -Filter *.onnx | ForEach-Object { Write-Host "  $($_.Name)" }
