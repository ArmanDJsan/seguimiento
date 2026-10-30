"""
Fase 3 — Validación del modelo de "llegada" (esferas detenidas).

Sobre el hold-out (~20 imágenes etiquetadas fuera de train/val):
  1. mAP50 / mAP50-95 por clase   -> AP@0.5 y AP@0.5:0.95 (COCO 101-pt) sobre predict nativo.
  2. Precisión / recall por clase  -> matching IoU@0.5 con conf>=0.25.
  3. Inferencia visual              -> imágenes anotadas (box + clase + conf).

Atención especial a bola_7 (vinotinto) y bola_8 (negra), las dos más difíciles.

Nota: se usa predict() con imgsz=[1088,1920] (resolución nativa, igual al motor TensorRT),
no val(), porque val() de ultralytics fuerza imgsz entero cuadrado y no refleja la resolución real.

Uso:
    python validate_llegada.py                       # best.pt del run por defecto
    python validate_llegada.py --weights <best.pt>
"""

import argparse
from pathlib import Path

import numpy as np
from ultralytics import YOLO

HERE = Path(__file__).resolve().parent
DEFAULT_BEST = HERE / "runs" / "detect" / "yolo26s_llegada_run_1" / "weights" / "best.pt"
HOLDOUT_DIR = HERE / "llegada" / "holdout"

CONF = 0.25          # umbral de confianza para P/R y visual
CONF_AP = 0.001      # umbral bajo para construir la curva PR completa (AP)
IOU_MATCH = 0.5      # IoU de match para AP@0.5 y P/R


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", type=Path, default=DEFAULT_BEST)
    ap.add_argument("--imgsz", type=int, nargs="+", default=[1088, 1920])
    ap.add_argument("--device", default="0")
    ap.add_argument("--conf", type=float, default=CONF)
    return ap.parse_args()


def iou(a, b):
    """IoU entre dos cajas xyxy."""
    x1 = max(a[0], b[0]); y1 = max(a[1], b[1])
    x2 = min(a[2], b[2]); y2 = min(a[3], b[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    area_a = max(0.0, a[2] - a[0]) * max(0.0, a[3] - a[1])
    area_b = max(0.0, b[2] - b[0]) * max(0.0, b[3] - b[1])
    union = area_a + area_b - inter
    return inter / union if union > 0 else 0.0


def load_gt(label_path, orig_h, orig_w):
    """Lee un .txt YOLO (cls cx cy w h normalizados) -> lista [x1,y1,x2,y2,cls] en px."""
    boxes = []
    if not label_path.exists():
        return boxes
    for line in label_path.read_text().strip().splitlines():
        parts = line.split()
        if len(parts) < 5:
            continue
        cls = int(float(parts[0]))
        cx, cy, w, h = (float(x) for x in parts[1:5])
        x1 = (cx - w / 2) * orig_w
        y1 = (cy - h / 2) * orig_h
        x2 = (cx + w / 2) * orig_w
        y2 = (cy + h / 2) * orig_h
        boxes.append([x1, y1, x2, y2, cls])
    return boxes


def compute_ap_from_pr(rec, prec):
    """AP por interpolación 101 puntos (COCO)."""
    mrec = np.concatenate(([0.0], rec, [1.0]))
    mpre = np.concatenate(([0.0], prec, [0.0]))
    for i in range(mpre.size - 1, 0, -1):
        mpre[i - 1] = max(mpre[i - 1], mpre[i])
    i = np.where(mrec[1:] != mrec[:-1])[0]
    return float(np.sum((mrec[i + 1] - mrec[i]) * mpre[i + 1]))


def match_predictions(preds, gts, iou_thr):
    """Matching greedy por clase. preds: [(conf, xyxy, cls)], gts: [[x1,y1,x2,y2,cls],...].
    Devuelve {cls: (tp, fp, fn)}."""
    gt_by_cls = {}
    for g in gts:
        gt_by_cls.setdefault(int(g[4]), []).append(g[:4])
    pred_by_cls = {}
    for _, box, c in preds:
        pred_by_cls.setdefault(int(c), []).append(box)

    per_class = {}
    for cls in sorted(set(gt_by_cls) | set(pred_by_cls)):
        gts_c = gt_by_cls.get(cls, [])
        preds_c = pred_by_cls.get(cls, [])
        used = set()
        tp = 0
        for p in preds_c:
            best_iou, best_j = 0.0, -1
            for j, g in enumerate(gts_c):
                if j in used:
                    continue
                v = iou(p, g)
                if v > best_iou:
                    best_iou, best_j = v, j
            if best_j >= 0 and best_iou >= iou_thr:
                tp += 1
                used.add(best_j)
        fp = len(preds_c) - tp
        fn = len(gts_c) - len(used)
        per_class[cls] = (tp, fp, fn)
    return per_class


def compute_ap_per_class(pred_by_img, gt_by_img, iou_thr, classes):
    """AP por clase en un umbral de IoU dado (matching greedy, conf en orden descendente)."""
    ap = {}
    for cls in classes:
        preds = []
        for img_id, plist in pred_by_img.items():
            for conf, box, c in plist:
                if int(c) == cls:
                    preds.append((conf, img_id, box))
        preds.sort(key=lambda t: -t[0])

        gt_boxes = {}
        gt_unassigned = {}
        n_gt = 0
        for img_id, glist in gt_by_img.items():
            boxes = [g[:4] for g in glist if int(g[4]) == cls]
            gt_boxes[img_id] = boxes
            gt_unassigned[img_id] = set(range(len(boxes)))
            n_gt += len(boxes)

        tp = np.zeros(len(preds), dtype=bool)
        fp = np.zeros(len(preds), dtype=bool)
        for i, (conf, img_id, box) in enumerate(preds):
            boxes = gt_boxes.get(img_id, [])
            un = gt_unassigned.get(img_id, set())
            best_j, best_iou = -1, 0.0
            for j in un:
                v = iou(box, boxes[j])
                if v > best_iou:
                    best_iou, best_j = v, j
            if best_j >= 0 and best_iou >= iou_thr:
                tp[i] = True
                un.discard(best_j)
            else:
                fp[i] = True

        tp_cum = np.cumsum(tp).astype(float)
        fp_cum = np.cumsum(fp).astype(float)
        rec = tp_cum / n_gt if n_gt else np.zeros_like(tp_cum)
        prec = tp_cum / np.maximum(tp_cum + fp_cum, np.finfo(float).eps)
        ap[cls] = compute_ap_from_pr(rec, prec)
    return ap


def run_predict(model, imgs, imgsz, device, conf, save, name):
    """predict sobre la lista de imágenes. Devuelve lista (Path, Result) en orden."""
    results = model.predict(
        source=[str(p) for p in imgs],
        imgsz=imgsz,
        conf=conf,
        iou=0.7,
        device=device,
        save=save,
        project=str(HERE / "runs" / "validate"),
        name=name,
        exist_ok=True,
        show_labels=True,
        show_conf=True,
        line_width=2,
        verbose=False,
    )
    return list(zip(imgs, results))


def collect(pairs):
    """Recolecta GT y predicciones por imagen (clave = stem del archivo original)."""
    pred_by_img = {}
    gt_by_img = {}
    for img, r in pairs:
        oh, ow = r.orig_shape
        stem = img.stem
        gts = load_gt(HOLDOUT_DIR / "labels" / f"{stem}.txt", oh, ow)
        gt_by_img[stem] = gts
        if r.boxes is None or len(r.boxes) == 0:
            pred_by_img[stem] = []
        else:
            xyxy = r.boxes.xyxy.cpu().numpy()
            cls = r.boxes.cls.cpu().numpy().astype(int)
            conf = r.boxes.conf.cpu().numpy()
            pred_by_img[stem] = [(conf[k], xyxy[k], cls[k]) for k in range(len(xyxy))]
    return pred_by_img, gt_by_img


def main():
    args = parse_args()
    if not args.weights.exists():
        raise SystemExit(f"No existe '{args.weights}'.")
    imgsz = args.imgsz if len(args.imgsz) == 2 else args.imgsz[0]

    model = YOLO(str(args.weights))
    names = list(model.names.values()) if hasattr(model.names, "values") else model.names
    n_classes = len(names)

    imgs = sorted(HOLDOUT_DIR.glob("images/*.jpg"))
    print(f"[validate] weights={args.weights} imgsz={imgsz}  holdout={len(imgs)} imágenes")

    # 1) Predicción a conf muy baja para construir la curva PR completa (AP).
    print(f"[validate] inferencia (conf={CONF_AP}) para métricas AP ...")
    pred_by_img, gt_by_img = collect(run_predict(model, imgs, imgsz, args.device, CONF_AP, save=False, name="holdout_ap"))

    all_classes = set()
    for glist in gt_by_img.values():
        for g in glist:
            all_classes.add(int(g[4]))
    for plist in pred_by_img.values():
        for _, _, c in plist:
            all_classes.add(int(c))
    all_classes = sorted(all_classes)

    # 2) AP@0.5 y AP@0.5:0.95 por clase.
    ap50 = compute_ap_per_class(pred_by_img, gt_by_img, 0.5, all_classes)
    ap_all = {c: [] for c in all_classes}
    for thr in np.arange(0.5, 1.0, 0.05):
        a = compute_ap_per_class(pred_by_img, gt_by_img, float(thr), all_classes)
        for c in all_classes:
            ap_all[c].append(a[c])
    ap5095 = {c: float(np.mean(ap_all[c])) for c in all_classes}

    print("\n=== AP por clase (hold-out, resolución nativa) ===")
    print(f"{'clase':>6} {'nombre':>10} {'AP@0.5':>8} {'AP@0.5:0.95':>12}")
    for c in all_classes:
        nm = names[c] if c < n_classes else str(c)
        print(f"{c:>6} {nm:>10} {ap50[c]:>8.4f} {ap5095[c]:>12.4f}")
    m50 = float(np.mean([ap50[c] for c in all_classes]))
    m5095 = float(np.mean([ap5095[c] for c in all_classes]))
    print(f"{'MEAN':>6} {'':>10} {m50:>8.4f} {m5095:>12.4f}")

    # 3) Precisión / recall por clase a conf>=CONF (y visual anotada).
    print(f"\n[validate] inferencia visual (conf={args.conf}) ...")
    pairs_v = run_predict(model, imgs, imgsz, args.device, args.conf, save=True, name="holdout_visual")
    pv, gv = collect(pairs_v)

    # ultralytics guarda como imageN.jpg; renombrar a los stems originales.
    save_dir = HERE / "runs" / "validate" / "holdout_visual"
    for i, (img, _) in enumerate(pairs_v):
        old = save_dir / f"image{i}.jpg"
        if old.exists():
            old.replace(save_dir / f"{img.stem}.jpg")

    agg = {c: [0, 0, 0] for c in all_classes}
    for img_id in gv:
        per = match_predictions(pv.get(img_id, []), gv[img_id], IOU_MATCH)
        for cls, (tp, fp, fn) in per.items():
            agg.setdefault(cls, [0, 0, 0])
            agg[cls][0] += tp; agg[cls][1] += fp; agg[cls][2] += fn

    print(f"\n=== Precisión / recall por clase (IoU@0.5, conf>={args.conf}) ===")
    print(f"{'clase':>6} {'nombre':>10} {'TP':>4} {'FP':>4} {'FN':>4} {'P':>8} {'R':>8}")
    ttp = tfp = tfn = 0
    for c in sorted(agg):
        tp, fp, fn = agg[c]
        nm = names[c] if c < n_classes else str(c)
        p = tp / (tp + fp) if (tp + fp) else 0.0
        r = tp / (tp + fn) if (tp + fn) else 0.0
        print(f"{c:>6} {nm:>10} {tp:>4} {fp:>4} {fn:>4} {p:>8.4f} {r:>8.4f}")
        ttp += tp; tfp += fp; tfn += fn
    p = ttp / (ttp + tfp) if (ttp + tfp) else 0.0
    r = ttp / (ttp + tfn) if (ttp + tfn) else 0.0
    print(f"{'TOTAL':>6} {'':>10} {ttp:>4} {tfp:>4} {tfn:>4} {p:>8.4f} {r:>8.4f}")

    print("\n[validate] listo. Imágenes anotadas en runs/validate/holdout_visual/")


if __name__ == "__main__":
    main()
