"""
Matriz de confusion sobre el holdout de "llegada" a la resolucion y conf del motor.

Para cuantificar la probabilidad de falla del orden de llegada necesitamos:
  1. Confusion real por clase (una bola de color X vista como color Y) -> intercambio de IDs.
  2. Miss rate por clase (bola presente no detectada) -> orden incompleto.
  3. False-positive rate (fantasma detectado) -> bola extra inexistente.

Usa predict() a imgsz=[1088,1920] conf=0.3 (igual que llegada_best.engine) y matching
greedy IoU@0.5.
"""

import argparse
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
from ultralytics import YOLO

HERE = Path(__file__).resolve().parent
DEFAULT_BEST = HERE / "runs" / "detect" / "yolo26s_llegada_run_1" / "weights" / "best.pt"
HOLDOUT = HERE / "llegada" / "holdout"

CONF = 0.3
IOU_MATCH = 0.5


def iou(a, b):
    x1 = max(a[0], b[0]); y1 = max(a[1], b[1])
    x2 = min(a[2], b[2]); y2 = min(a[3], b[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    area_a = max(0.0, a[2] - a[0]) * max(0.0, a[3] - a[1])
    area_b = max(0.0, b[2] - b[0]) * max(0.0, b[3] - b[1])
    u = area_a + area_b - inter
    return inter / u if u > 0 else 0.0


def load_gt(p, oh, ow):
    boxes = []
    if not p.exists():
        return boxes
    for line in p.read_text().strip().splitlines():
        s = line.split()
        if len(s) < 5:
            continue
        cls = int(float(s[0]))
        cx, cy, w, h = (float(x) for x in s[1:5])
        boxes.append([(cx - w/2)*ow, (cy - h/2)*oh, (cx + w/2)*ow, (cy + h/2)*oh, cls])
    return boxes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", type=Path, default=DEFAULT_BEST)
    ap.add_argument("--conf", type=float, default=CONF)
    args = ap.parse_args()

    model = YOLO(str(args.weights))
    names = list(model.names.values())

    imgs = sorted(HOLDOUT.glob("images/*.jpg"))
    results = model.predict(
        source=[str(p) for p in imgs],
        imgsz=[1088, 1920], conf=args.conf, iou=0.7, device="0",
        verbose=False,
    )

    n_cls = len(names)
    conf_matrix = np.zeros((n_cls, n_cls), dtype=int)   # [gt][pred]
    misses = Counter()                                   # gt class -> # no match
    false_pos = Counter()                                # pred class -> # phantom
    matched_pairs = []

    for img, r in zip(imgs, results):
        oh, ow = r.orig_shape
        gts = load_gt(HOLDOUT / "labels" / f"{img.stem}.txt", oh, ow)
        preds = []
        if r.boxes is not None and len(r.boxes):
            xyxy = r.boxes.xyxy.cpu().numpy()
            cls = r.boxes.cls.cpu().numpy().astype(int)
            conf = r.boxes.conf.cpu().numpy()
            preds = [(conf[k], xyxy[k], cls[k]) for k in range(len(xyxy))]

        # greedy matching: por cada pred (orden por conf desc), asignar al GT mas cercano de SU clase real
        used_gt = set()
        for conf_v, pbox, pcls in preds:
            best_i, best_iou = -1, 0.0
            for j, g in enumerate(gts):
                if j in used_gt:
                    continue
                v = iou(pbox, g[:4])
                if v > best_iou:
                    best_iou, best_i = v, j
            if best_i >= 0 and best_iou >= IOU_MATCH:
                used_gt.add(best_i)
                gcls = int(gts[best_i][4])
                conf_matrix[gcls][pcls] += 1
                matched_pairs.append((gcls, pcls, best_iou))
            else:
                false_pos[pcls] += 1
        for j, g in enumerate(gts):
            if j not in used_gt:
                misses[int(g[4])] += 1

    total_gt = conf_matrix.sum() + sum(misses.values())
    total_fp = sum(false_pos.values())

    print(f"holdout: {len(imgs)} imagenes, conf={args.conf}, IoU_match={IOU_MATCH}")
    print(f"GT total = {total_gt}, TP = {conf_matrix.sum()}, FP = {total_fp}, FN = {sum(misses.values())}")

    print("\n=== Matriz de confusion [GT -> pred] (diagonal = correcto) ===")
    hdr = "gt\\pred " + "".join(f"{n[-1]:>4}" for n in names)
    print(hdr)
    for i in range(n_cls):
        row = " ".join(f"{conf_matrix[i][j]:>4}" for j in range(n_cls))
        print(f"  {i:>2}   {row}   miss={misses[i]}")

    print("\n=== Tasas por clase ===")
    print(f"{'clase':>6} {'nombre':>10} {'n':>4} {'correcto':>9} {'confusion':>9} {'miss':>6}")
    for i in range(n_cls):
        n = int(conf_matrix[i].sum()) + misses[i]
        if n == 0:
            continue
        correct = int(conf_matrix[i][i])
        swapped = int(conf_matrix[i].sum()) - correct
        print(f"{i:>6} {names[i]:>10} {n:>4} {correct/n:>9.3f} {swapped/n:>9.3f} {misses[i]/n:>6.3f}")

    # resumen agregado de falla de identidad (swap rate global)
    total_gt_all = total_gt
    total_swaps = sum(int(conf_matrix[i].sum()) - int(conf_matrix[i][i]) for i in range(n_cls))
    total_miss = sum(misses.values())
    print("\n=== Resumen global ===")
    print(f"swap (bola vista como otro color): {total_swaps}/{total_gt_all} = {total_swaps/total_gt_all:.4f}")
    print(f"miss (bola no detectada):         {total_miss}/{total_gt_all} = {total_miss/total_gt_all:.4f}")
    print(f"false positive (fantasma):        {total_fp}  (sobre {total_gt_all} GT)")

    # confusiones especificas
    print("\n=== Pares mas confundidos ===")
    pairs = []
    for i in range(n_cls):
        for j in range(n_cls):
            if i != j and conf_matrix[i][j] > 0:
                pairs.append((conf_matrix[i][j], i, j))
    pairs.sort(reverse=True)
    for c, i, j in pairs:
        print(f"  {names[i]:>10} -> {names[j]:>10} : {c}")

    if matched_pairs:
        ious = np.array([p[2] for p in matched_pairs])
        print(f"\nIoU mediano de los matches: {np.median(ious):.3f}")


if __name__ == "__main__":
    main()
