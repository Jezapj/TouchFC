# Detector models

TouchFC reads any detector listed in [`config/detectors.json`](../config/detectors.json). Pick one in the Main tab before **Start capture** (the pick is kept in `profiles/detector.txt`), or with `--detector "<name>"`. `--model file.onnx` still works and overrides both; a file whose name matches a preset takes that preset's settings.

Weights live in `third_party/models/` and are never committed.

## Presets

| Field | Values | Meaning |
|---|---|---|
| `name` | text | Shown in the Main tab and the HUD. |
| `file` | path | ONNX file, relative to the repo root. A missing file is greyed out. |
| `resize` | `letterbox` (default), `stretch` | How the frame is fitted to the square input. Use what the model was trained with: Roboflow exports and D-FINE / RF-DETR training stretch; Ultralytics letterboxes. |
| `norm` | `unit` (default), `imagenet` | `unit` is RGB / 255 (YOLO, D-FINE, RT-DETR). `imagenet` also subtracts the mean and divides by the std (RF-DETR). |
| `classes` | list of names | The model's labels in index order. `player` / `person`, `goalkeeper`, `referee`, `ball` / `sports ball` are kept; anything else, including `""`, is dropped. Without a list: COCO person and sports ball for 80+ class heads, ids as they are otherwise. |
| `note` | text | Tooltip. |

A Roboflow COCO export numbers its categories from the dataset superclass at 0, so a fine-tune on it labels ball, goalkeeper, player and referee 1 to 4, which is why the shipped presets start with `""`. If boxes come out with the wrong colours (goalkeepers yellow, referee grey, ball white), the list is off by one.

## Heads read

Told apart from the ONNX outputs; the HUD log line `ORT ready: ... head=` says which.

| Head | Outputs | From |
|---|---|---|
| `yolo` | `[1, 4+nc, anchors]` | Ultralytics YOLOv8 / v11. NMS after decode. |
| `detr joint` | `[1, Q, 4+nc]`, cxcywh in [0, 1], probabilities | Ultralytics RT-DETR export. |
| `detr split` | `[1, Q, 4]` boxes + `[1, Q, C]` logits | RF-DETR `model.export()`. |
| `detr sparse` | `labels [1, Q]` int64, `boxes [1, Q, 4]`, `scores [1, Q]`, plus an `orig_target_sizes` input | D-FINE, DEIM and RT-DETR (lyuwenyu) `export_onnx.py`. |

DETR heads skip NMS; the "player confidence" slider is the score cut for every head. The input side comes from the model (D-FINE 640, RF-DETR Nano 384, Base 560).

## Licences, for a build that may be sold

| Part | Licence | Commercial use |
|---|---|---|
| D-FINE, DEIM, RT-DETR (lyuwenyu), RF-DETR Nano to Base | Apache 2.0 code | Yes, keep the notice. Check the RF-DETR repo for its larger sizes. |
| Ultralytics YOLO, RT-DETR in `ultralytics` | AGPL-3.0 or paid licence | Only under AGPL or with their licence. `yolov8n.onnx` and the roboflow/sports pitch model are in this group. |
| Roboflow Universe football datasets | Usually CC BY 4.0, per project | Yes, with credit. Read the project page of the version you download. |
| SoccerNet | Research use, under an NDA | No. Fine for testing only. |

Weights trained on a dataset carry that dataset's terms in practice: an Apache 2.0 model trained on SoccerNet is not a commercial model.

## Fine-tuning a Light preset

Any GPU with 8 GB or more, or a hosted notebook. One to three hours.

1. Download a CC BY 4.0 football dataset with ball, goalkeeper, player and referee from Roboflow Universe, in COCO format, 640 x 640 stretch.
2. Train one of these:
   - **D-FINE-N:** clone https://github.com/Peterande/D-FINE, point `configs/dataset/custom_detection.yml` at the export (`remap_mscoco_category: False`, `num_classes: 5`), train `configs/dfine/custom/dfine_hgnetv2_n_custom.yml`, then `python tools/deployment/export_onnx.py -c <config> -r best_stg2.pth`.
   - **RF-DETR-Nano:** `pip install rfdetr`, `RFDETRNano().train(dataset_dir=..., epochs=50)`, then `model.export()`.
3. Copy the ONNX to the preset's `file` (for example `third_party/models/dfine_n_soccer.onnx`) and pick it in the Main tab.
4. Compare on the test clips with `--replay clip.mp4 --detector "<name>" --telemetry t.csv`: inference p50 in the CSV, and how often the HUD shows a ball.

Upweight the ball class or train at the input side you will run at; at 640 a ball in a wide shot is 3 to 6 pixels.
