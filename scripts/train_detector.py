"""Fine-tune a permissive football detector and export it for TouchFC.

Standalone: copy this one file to a CUDA machine or a Colab GPU runtime.

  pip install roboflow
  export ROBOFLOW_API_KEY=...            # or pass --dataset-dir
  python train_detector.py --arch dfine-n --epochs 80
  python train_detector.py --arch rfdetr-nano --epochs 50

After a lost session: --resume <checkpoint.pth> continues training, and
--export-only <checkpoint_best_ema.pth, checkpoint_best_total.pth or best_stg2.pth>
just writes the ONNX. RF-DETR resumes from its newest checkpoint_<epoch>.ckpt.

Archs (all Apache 2.0 code):
  dfine-n, dfine-s     D-FINE (github.com/Peterande/D-FINE), sparse head, unit norm
  rfdetr-nano, -small, -base   RF-DETR (pip rfdetr), split head, ImageNet norm

Output: <out>/<arch>_soccer.onnx and the matching config/detectors.json entry
printed at the end. Copy the .onnx to third_party/models/ on the TouchFC PC.

Dataset: a Roboflow Universe project in COCO format (train/, valid/, each with
_annotations.coco.json). Check its licence on the project page before use;
CC BY 4.0 allows commercial use with credit. SoccerNet data is research-only.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import urllib.request

DFINE_REPO = "https://github.com/Peterande/D-FINE.git"
# COCO-pretrained starting weights, from the D-FINE README model table.
DFINE_WEIGHTS = {
    "n": "https://github.com/Peterande/storage/releases/download/dfinev1.0/dfine_n_coco.pth",
    "s": "https://github.com/Peterande/storage/releases/download/dfinev1.0/dfine_s_coco.pth",
}
PRESET = {
    "dfine-n": ("Light: D-FINE-N", "unit", "CPU"),
    "dfine-s": ("Balanced: D-FINE-S", "unit", "Fast CPU or any GPU"),
    "rfdetr-nano": ("Light: RF-DETR-Nano", "imagenet", "CPU"),
    "rfdetr-small": ("Balanced: RF-DETR-Small", "imagenet", "Fast CPU or any GPU"),
    "rfdetr-base": ("Quality: RF-DETR-Base", "imagenet", "GPU build of ONNX Runtime recommended"),
}


def run(cmd, cwd=None):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def get_dataset(a):
    if a.dataset_dir:
        return os.path.abspath(a.dataset_dir)
    key = os.environ.get("ROBOFLOW_API_KEY")
    if not key:
        sys.exit("set ROBOFLOW_API_KEY or pass --dataset-dir")
    from roboflow import Roboflow

    ds = Roboflow(api_key=key).workspace(a.workspace).project(a.project).version(a.version).download(
        "coco", location=os.path.join(a.work, "dataset"))
    return os.path.abspath(ds.location)


def read_classes(ds):
    """Label id -> name, as the trained model will emit it (COCO category id)."""
    with open(os.path.join(ds, "train", "_annotations.coco.json")) as f:
        coco = json.load(f)
    cats = coco["categories"]
    names = [""] * (max(c["id"] for c in cats) + 1)
    used = {an["category_id"] for an in coco["annotations"]}
    for c in cats:
        # A superclass with no boxes of its own (Roboflow's id 0) stays "".
        names[c["id"]] = c["name"].lower() if c["id"] in used else ""
    return names


def check_layout(ds):
    for split in ("train", "valid"):
        p = os.path.join(ds, split, "_annotations.coco.json")
        if not os.path.isfile(p):
            sys.exit(f"missing {p}: download the dataset in COCO format")


def train_dfine(a, ds, classes):
    size = a.arch.split("-")[1]
    repo = os.path.join(a.work, "D-FINE")
    if not os.path.isdir(repo):
        run(["git", "clone", "--depth", "1", DFINE_REPO, repo])
        run([sys.executable, "-m", "pip", "install", "-q", "-r", "requirements.txt"], cwd=repo)
    weights = os.path.join(a.work, f"dfine_{size}_coco.pth")
    if not os.path.isfile(weights):
        print("downloading", DFINE_WEIGHTS[size])
        urllib.request.urlretrieve(DFINE_WEIGHTS[size], weights)
    base = f"configs/dfine/dfine_hgnetv2_{size}_coco.yml"
    if not os.path.isfile(os.path.join(repo, base)):
        sys.exit(f"{base} not in the D-FINE checkout; pick the matching config from configs/dfine/")
    out_dir = os.path.abspath(os.path.join(a.work, f"dfine_{size}_out"))
    cfg = os.path.join(repo, "configs", "dfine", f"touchfc_{size}.yml")

    def split(name, shuffle):
        return {
            "dataset": {
                "img_folder": os.path.join(ds, name),
                "ann_file": os.path.join(ds, name, "_annotations.coco.json"),
            },
            "shuffle": shuffle,
            "total_batch_size": a.batch,
        }

    # The stock COCO config, with our data and class count. Labels stay the
    # COCO category ids (remap off), which is what the preset's list indexes.
    with open(cfg, "w") as f:
        json.dump({
            "__include__": [os.path.basename(base)],
            "output_dir": out_dir,
            "num_classes": len(classes),
            "remap_mscoco_category": False,
            "epoches": a.epochs,
            "train_dataloader": split("train", True),
            "val_dataloader": split("valid", False),
        }, f, indent=1)  # JSON is valid YAML
    if a.export_only:
        ckpt = os.path.abspath(a.export_only)
    else:
        start = ["-r", os.path.abspath(a.resume)] if a.resume else ["-t", weights]
        run([sys.executable, "train.py", "-c", cfg, "--use-amp", "--seed=0", *start], cwd=repo)
        ckpt = None
    ckpt = ckpt or next((p for p in (os.path.join(out_dir, n) for n in ("best_stg2.pth", "best_stg1.pth", "last.pth"))
                 if os.path.isfile(p)), None)
    if ckpt is None:
        sys.exit(f"no checkpoint in {out_dir}")
    run([sys.executable, "tools/deployment/export_onnx.py", "--check", "-c", cfg, "-r", ckpt], cwd=repo)
    return os.path.splitext(ckpt)[0] + ".onnx"


def train_rfdetr(a, ds):
    run([sys.executable, "-m", "pip", "install", "-q", "rfdetr[onnx]"])
    import rfdetr

    cls = {"rfdetr-nano": "RFDETRNano", "rfdetr-small": "RFDETRSmall", "rfdetr-base": "RFDETRBase"}[a.arch]
    out_dir = os.path.abspath(os.path.join(a.work, a.arch + "_out"))
    model = getattr(rfdetr, cls)()
    if not a.export_only:
        extra = {"resume": os.path.abspath(a.resume)} if a.resume else {}
        model.train(dataset_dir=ds, epochs=a.epochs, batch_size=a.batch, grad_accum_steps=max(1, 16 // a.batch),
                    lr=1e-4, output_dir=out_dir, **extra)
    # Older rfdetr writes checkpoint_best_total.pth, newer checkpoint_best_ema.pth
    # (plus checkpoint_<epoch>.ckpt for --resume).
    best = os.path.abspath(a.export_only) if a.export_only else next(
        (p for p in (os.path.join(out_dir, n) for n in (
            "checkpoint_best_total.pth", "checkpoint_best_ema.pth", "checkpoint_best_regular.pth"))
         if os.path.isfile(p)), "")
    if os.path.isfile(best):
        model = getattr(rfdetr, cls)(pretrain_weights=best)
    model.export(output_dir=out_dir)
    found = sorted(glob.glob(os.path.join(out_dir, "**", "*.onnx"), recursive=True), key=os.path.getmtime)
    if not found:
        sys.exit(f"export wrote no .onnx under {out_dir}")
    return found[-1]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--arch", required=True, choices=sorted(PRESET))
    p.add_argument("--epochs", type=int, default=80)
    p.add_argument("--batch", type=int, default=8)
    p.add_argument("--dataset-dir", help="COCO export already on disk; skips the Roboflow download")
    p.add_argument("--workspace", default="roboflow-jvuqo")
    p.add_argument("--project", default="football-players-detection-3zvbc")
    p.add_argument("--version", type=int, default=12)
    p.add_argument("--work", default="touchfc_train")
    p.add_argument("--out", default="touchfc_models")
    p.add_argument("--resume", help="checkpoint to continue training from (after a lost session)")
    p.add_argument("--export-only", help="checkpoint to export without training (runs on a CPU)")
    a = p.parse_args()
    os.makedirs(a.work, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)

    ds = get_dataset(a)
    check_layout(ds)
    classes = read_classes(ds)
    print("classes by label id:", classes)
    # TouchFC maps labels by keyword (core/include/touchfc/detr.hpp touchfc_class_of).
    if not any("ball" in c and "player" not in c for c in classes) or not any("player" in c for c in classes):
        print("warning: no ball or player class; TouchFC drops labels it does not know")

    onnx = train_dfine(a, ds, classes) if a.arch.startswith("dfine") else train_rfdetr(a, ds)
    name = a.arch.replace("-", "_") + "_soccer.onnx"
    dst = os.path.join(a.out, name)
    shutil.copyfile(onnx, dst)
    title, norm, note = PRESET[a.arch]
    entry = {"name": title, "file": "third_party/models/" + name, "resize": "stretch", "norm": norm,
             "classes": classes, "note": note}
    print("\nmodel:", os.path.abspath(dst))
    print("config/detectors.json entry:\n" + json.dumps(entry, indent=2))


if __name__ == "__main__":
    main()
