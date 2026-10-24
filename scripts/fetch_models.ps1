# TouchFC does not download model weights.
#
# Before any weight file is fetched, open the upstream licence pages and read
# them for the commit you intend to pin. Do not trust a comment in this repo.
#
# - https://github.com/roboflow/sports
#     The code repository is widely described as MIT. Confirm the LICENCE file
#     at the commit you pin. Do not assume it from this script.
# - Ultralytics YOLOv8 weights, which roboflow/sports has pointed at, are
#     published by Ultralytics under AGPL-3.0. Confirm on
#     https://github.com/ultralytics/ultralytics before use.
#
# This repository is AGPL-3.0-only. Weights are never committed. Broadcast
# footage is never committed.
Write-Host "Model download is disabled until the weight licence is confirmed for a pinned commit."
exit 0
