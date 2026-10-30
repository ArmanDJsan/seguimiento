"""
Auto-prueba (3 ciclos) de los motores del ejecutable contra la verdad de campo
(posiciones.json), optimizando deteccion y orden izquierda->derecha.

Ciclo 1: barrido de umbral de confianza (por modelo).
Ciclo 2: barrido de NMS (ninguno / agnostico / por clase) y regla 1-por-clase.
Ciclo 3: fusion de ambos motores (union / primario+relleno) y JSON final.

Uso:
    python tune_exe_models.py
    python tune_exe_models.py --gt <posiciones.json> --out <predicciones.json>
"""

import argparse
import json
import re
from collections import Counter
from datetime import datetime
from pathlib import Path

import numpy as np
import tensorrt as trt
import torch
from PIL import Image

HERE = Path(__file__).resolve().parent
DEFAULT_IMAGES = HERE / "cvat_images"
DEFAULT_MODELS = HERE.parent / "src" / "bin" / "Release" / "models"
DEFAULT_GT = Path(r"C:\Users\TecnologíaIntegraTec\Downloads\Telegram Desktop\posiciones.json")
DEFAULT_OUT = HERE / "predictions_tuned.json"

MODEL_SPECS = [
    {"name": "modelo_1920x1088.engine", "key": "principal"},
    {"name": "llegada_best.engine", "key": "llegada"},
]
NMS_BASE = 0.4


# --------------------------------------------------------------------------- #
# TensorRT
# --------------------------------------------------------------------------- #
class TrtEngine:
    def __init__(self, path: Path):
        self.path = path
        self.engine = trt.Runtime(trt.Logger(trt.Logger.ERROR)).deserialize_cuda_engine(
            path.read_bytes())
        if self.engine is None:
            raise RuntimeError(f"No se pudo deserializar {path}")
        self.context = self.engine.create_execution_context()
        self.input_name = self.output_name = None
        for i in range(self.engine.num_io_tensors):
            n = self.engine.get_tensor_name(i)
            if self.engine.get_tensor_mode(n) == trt.TensorIOMode.INPUT:
                self.input_name = n
            else:
                self.output_name = n
        shape = tuple(self.engine.get_tensor_shape(self.input_name))
        self.in_h, self.in_w = int(shape[2]), int(shape[3])

    def infer(self, image: Image.Image) -> np.ndarray:
        resized = image.convert("RGB").resize((self.in_w, self.in_h), Image.BILINEAR)
        arr = np.asarray(resized, dtype=np.float32) / 255.0
        nchw = np.ascontiguousarray(np.transpose(arr, (2, 0, 1))[None, ...])
        d_in = torch.from_numpy(nchw).cuda()
        self.context.set_input_shape(self.input_name, (1, 3, self.in_h, self.in_w))
        out_shape = tuple(self.context.get_tensor_shape(self.output_name))
        if out_shape[0] < 0:
            out_shape = (1, out_shape[1], out_shape[2])
        d_out = torch.empty(out_shape, dtype=torch.float32, device="cuda")
        self.context.set_tensor_address(self.input_name, d_in.data_ptr())
        self.context.set_tensor_address(self.output_name, d_out.data_ptr())
        ok = self.context.execute_async_v3(torch.cuda.current_stream().cuda_stream)
        torch.cuda.synchronize()
        if not ok:
            raise RuntimeError("execute_async_v3 fallo")
        return d_out[0].cpu().numpy()


# --------------------------------------------------------------------------- #
# Post-proceso / fusion
# --------------------------------------------------------------------------- #
def iou(a, b):
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    inter = max(0.0, min(ax2, bx2) - max(ax1, bx1)) * max(0.0, min(ay2, by2) - max(ay1, by1))
    union = (ax2 - ax1) * (ay2 - ay1) + (bx2 - bx1) * (by2 - by1) - inter
    return inter / union if union > 0 else 0.0


def nms(dets, thr, per_class):
    dets = sorted(dets, key=lambda d: d["conf"], reverse=True)
    keep = []
    for d in dets:
        # Suprimir solo si ALGUNA deteccion conservada (de la misma clase si
        # per_class) se solapa por encima del umbral. any() sobre vacio = False.
        if any((not per_class or d["cid"] == k["cid"]) and iou(d["bbox"], k["bbox"]) > thr
               for k in keep):
            continue
        keep.append(d)
    return keep


def postprocess(raw, in_w, in_h, out_w, out_h, conf, nms_mode, nms_iou, one_per_class):
    sx, sy = out_w / in_w, out_h / in_h
    dets = []
    for row in raw:
        c = float(row[4])
        if c < conf:
            continue
        x1, y1, x2, y2 = (float(v) for v in row[:4])
        x1, x2 = min(max(x1, 0), in_w), min(max(x2, 0), in_w)
        y1, y2 = min(max(y1, 0), in_h), min(max(y2, 0), in_h)
        if x2 <= x1 or y2 <= y1:
            continue
        cid = int(round(float(row[5])))
        cid = min(max(cid, 0), 9)
        bbox = [x1 * sx, y1 * sy, x2 * sx, y2 * sy]
        dets.append({"cid": cid, "conf": c, "bbox": bbox, "cx": (bbox[0] + bbox[2]) / 2})

    if nms_mode == "agnostic":
        dets = nms(dets, nms_iou, per_class=False)
    elif nms_mode == "per-class":
        dets = nms(dets, nms_iou, per_class=True)

    if one_per_class:
        best = {}
        for d in dets:
            if d["cid"] not in best or d["conf"] > best[d["cid"]]["conf"]:
                best[d["cid"]] = d
        dets = list(best.values())

    return sorted(dets, key=lambda d: d["cx"])


def fuse(primary, secondary, mode):
    if mode == "primary":
        return sorted(primary, key=lambda d: d["cx"])
    best = {}
    if mode == "union-best":
        for d in primary + secondary:
            if d["cid"] not in best or d["conf"] > best[d["cid"]]["conf"]:
                best[d["cid"]] = d
    elif mode == "primary-fill":
        for d in primary:
            best[d["cid"]] = d
        for d in secondary:
            if d["cid"] not in best:
                best[d["cid"]] = d
    return sorted(best.values(), key=lambda d: d["cx"])


def _median(v):
    v = sorted(v)
    mid = len(v) // 2
    return v[mid] if len(v) % 2 else (v[mid - 1] + v[mid]) / 2


def smart_fill(primary, secondary, all_boxes, num_classes=10):
    """Conserva las etiquetas del primario y rellena cada clase faltante con una
    caja fisica libre. Regla clave: una esfera = una etiqueta. Una caja ya
    elegida no se reutiliza, y una caja cuya clase original tambien falta se
    reserva para esa clase (antes la clase 4 podia consumir la caja de la 8 y
    quedaban dos etiquetas sobre la misma esfera)."""
    used = {d["cid"] for d in primary}
    missing = [c for c in range(num_classes) if c not in used]
    chosen = list(primary)
    if not missing:
        return sorted(chosen, key=lambda d: d["cx"])

    occupied = [d["bbox"] for d in primary]

    def free_box(d):
        return max((iou(d["bbox"], b) for b in occupied), default=0.0) < 0.3

    ws = [d["bbox"][2] - d["bbox"][0] for d in primary]
    hs = [d["bbox"][3] - d["bbox"][1] for d in primary]
    cys = [(d["bbox"][1] + d["bbox"][3]) / 2 for d in primary]
    mw, mh, my = (_median(ws), _median(hs), _median(cys)) if primary else (0, 0, 0)

    def plausible(d):
        x1, y1, x2, y2 = d["bbox"]
        w, h, cy = x2 - x1, y2 - y1, (y1 + y2) / 2
        if not primary:
            return True
        return (0.5 * mw <= w <= 1.8 * mw and 0.5 * mh <= h <= 1.8 * mh
                and abs(cy - my) <= 2.0 * mh)

    def already_chosen(d):
        return any(iou(d["bbox"], p["bbox"]) > 0.5 for p in chosen)

    free = [d for d in all_boxes if free_box(d) and plausible(d)]
    free = nms(free, 0.4, per_class=False)
    free.sort(key=lambda d: d["conf"], reverse=True)

    unplaced = set(missing)
    for c in missing:
        own = [d for d in secondary if d["cid"] == c and free_box(d)
               and plausible(d) and not already_chosen(d)]
        if own:
            chosen.append(max(own, key=lambda d: d["conf"]))
        else:
            cand = [d for d in free
                    if not already_chosen(d) and d["cid"] not in unplaced]
            if cand:
                d = dict(cand[0])
                d["cid"] = c
                chosen.append(d)
            else:
                own = [d for d in secondary if d["cid"] == c
                       and not already_chosen(d)]
                if own:
                    chosen.append(max(own, key=lambda d: d["conf"]))
        unplaced.discard(c)
    return sorted(chosen, key=lambda d: d["cx"])


def slots_from(centers):
    """10 posiciones x de la fila, rellenando huecos con el mayor espaciado."""
    cs = sorted(centers)
    uniq = []
    for c in cs:
        if uniq and c - uniq[-1] <= 35:
            uniq[-1] = (uniq[-1] + c) / 2
        else:
            uniq.append(c)
    diffs = [b - a for a, b in zip(uniq, uniq[1:]) if 55 <= b - a <= 130]
    s = float(np.median(diffs)) if diffs else 90.0
    filled = list(uniq)
    while len(filled) < 10:
        if len(filled) < 2:
            # Sin dos centros no hay hueco interno que medir (p.ej. una
            # columna vertical); se completa hacia la derecha.
            filled.append(filled[-1] + s)
            continue
        gap, i = max((b - a, i) for i, (a, b) in enumerate(zip(filled, filled[1:])))
        if gap > 1.25 * s:
            filled.insert(i + 1, (filled[i] + filled[i + 1]) / 2)
        elif filled[-1] + s < 1280:
            filled.append(filled[-1] + s)
        elif filled[0] - s > 0:
            filled.insert(0, filled[0] - s)
        else:
            break
    if len(filled) > 10:
        best = None
        for i in range(len(filled) - 9):
            w = filled[i:i + 10]
            res = sum(abs((b - a) - s) for a, b in zip(w, w[1:]))
            if best is None or res < best[0]:
                best = (res, w)
        filled = best[1]
    return filled


def row_slots(engines, raws, key, size, conf=0.4):
    """Centros de cajas de fila (tamano de esfera) de ambos motores."""
    centers, cys, hs, ws = [], [], [], []
    for name, eng in engines.items():
        raw, sz = raws[(name, key)]
        for r in raw:
            if float(r[4]) < conf:
                continue
            x1, y1, x2, y2 = (float(v) for v in r[:4])
            w = (x2 - x1) * sz[0] / eng.in_w
            h = (y2 - y1) * sz[1] / eng.in_h
            if not (60 <= w <= 170 and 70 <= h <= 170):
                continue
            centers.append((x1 + x2) / 2 * sz[0] / eng.in_w)
            cys.append((y1 + y2) / 2 * sz[1] / eng.in_h)
            hs.append(h)
            ws.append(w)
    if not centers:
        return [], size[1] / 2, size[1] / 6, 90
    return (slots_from(centers), _median(cys), _median(hs), _median(ws))


def black_slot(hsv, slots, my, mh):
    """Indice de la unica esfera negra (brillo y saturacion bajos)."""
    best = None
    for i, gx in enumerate(slots):
        y0, y1 = int(my - 0.35 * mh), int(my + 0.35 * mh)
        x0, x1 = int(gx - 0.35 * mh), int(gx + 0.35 * mh)
        patch = hsv[max(0, y0):y1, max(0, x0):x1]
        if patch.size == 0:
            continue
        s = float(np.median(patch[..., 1]))
        v = float(np.median(patch[..., 2]))
        if v < 110 and s < 60 and (best is None or v < best[1]):
            best = (i, v)
    return best[0] if best else None


def color8_fix(dets, slots, black_idx, my, mh, mw):
    """Fuerza la clase 8 en la esfera negra: intercambia etiqueta con la caja
    de esa posicion o reubica la deteccion 8 si la posicion esta vacia."""
    if black_idx is None or not slots:
        return dets
    xb = slots[black_idx]
    spacing = (slots[-1] - slots[0]) / max(1, len(slots) - 1)
    near = min(dets, key=lambda d: abs(d["cx"] - xb))
    near_idx = dets.index(near)
    eight = [i for i, d in enumerate(dets) if d["cid"] == 8]
    if not eight:
        return dets
    ei = eight[0]

    if abs(near["cx"] - xb) <= 0.55 * spacing:
        if near["cid"] != 8:
            dets[ei]["cid"], dets[near_idx]["cid"] = near["cid"], 8
    else:
        d = dets[ei]
        d["bbox"] = [xb - mw / 2, my - mh / 2, xb + mw / 2, my + mh / 2]
        d["cx"] = xb
    # reordenar izquierda -> derecha
    dets.sort(key=lambda d: d["cx"])
    return dets


# --------------------------------------------------------------------------- #
# Verdad de campo y metricas
# --------------------------------------------------------------------------- #
def norm_alias(s):
    return re.sub(r"\s+", "", Path(s).stem)


def load_gt(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    gt = {}
    for cap in data["capturas"]:
        gt[norm_alias(cap["alias"])] = [o["numero"] for o in
                                        sorted(cap["orden"], key=lambda o: o["pos"])]
    return gt


def kendall(a, b):
    conc = disc = 0
    for i in range(len(a)):
        for j in range(i + 1, len(a)):
            s = (a[i] - a[j]) * (b[i] - b[j])
            conc += s > 0
            disc += s < 0
    return (conc - disc) / (conc + disc) if conc + disc else 0.0


def evaluate(gt_map, pred_map):
    """pred_map: imagen -> lista de cid en orden izquierda->derecha."""
    n = len(gt_map)
    cover = purity = exact = tau = 0.0
    for key, gt in gt_map.items():
        pred = pred_map.get(key, [])
        gt_pos = {c: i for i, c in enumerate(gt)}
        cnt = Counter(pred)
        matched = [c for c in cnt if c in gt_pos]
        cover += len(matched) / len(gt)
        purity += len(matched) / len(pred) if pred else 0.0
        exact += sum(1 for c in matched if cnt[c] == 1 and pred.index(c) == gt_pos[c]) / len(gt)
        uniq = [c for c in matched if cnt[c] == 1]
        if len(uniq) > 1:
            tau += kendall([pred.index(c) for c in uniq], [gt_pos[c] for c in uniq])
    return {"coverage": cover / n, "purity": purity / n,
            "exact": exact / n, "tau": tau / n}


def score(m):
    return 0.35 * m["coverage"] + 0.25 * m["purity"] + 0.25 * m["exact"] + 0.15 * max(m["tau"], 0)


def seq_of(dets):
    return [d["cid"] for d in dets]


def fmt_metrics(m):
    return (f"cob={m['coverage']:.2f} pur={m['purity']:.2f} "
            f"pos={m['exact']:.2f} tau={m['tau']:.2f} score={score(m):.3f}")


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=Path, default=DEFAULT_IMAGES)
    ap.add_argument("--models", type=Path, default=DEFAULT_MODELS)
    ap.add_argument("--gt", type=Path, default=DEFAULT_GT)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    gt = load_gt(args.gt)
    names = [c["name"] for c in json.loads(
        (HERE / "cvat_labels_pool.json").read_text(encoding="utf-8"))]

    image_paths = {norm_alias(p.name): p for p in args.images.glob("*.png")}
    keys = [k for k in gt if k in image_paths]
    if not keys:
        raise SystemExit("No hay imagenes que coincidan con la verdad de campo")

    engines, raws = {}, {}
    for spec in MODEL_SPECS:
        path = args.models / spec["name"]
        eng = TrtEngine(path)
        engines[spec["key"]] = eng
        for k in keys:
            with Image.open(image_paths[k]) as img:
                raws[(spec["key"], k)] = (eng.infer(img), img.size)
        print(f"Motor cargado: {spec['name']}  (entrada {eng.in_w}x{eng.in_h})")

    sizes = {k: Image.open(image_paths[k]).size for k in keys}

    # Geometria de la fila y esfera negra por imagen (para correccion de color).
    row_info = {}
    for k in keys:
        with Image.open(image_paths[k]) as img:
            hsv = np.asarray(img.convert("HSV"), dtype=np.float32)
        slots, my, mh, mw = row_slots(engines, raws, k, sizes[k])
        row_info[k] = {"slots": slots, "my": my, "mh": mh, "mw": mw,
                       "black": black_slot(hsv, slots, my, mh)}
        print(f"  {k:<8} slots={[round(s) for s in slots]} "
              f"negra_slot={row_info[k]['black']}")

    def dets_for(key, k, conf, nms_mode, nms_iou, one_per_class):
        eng = engines[key]
        raw, size = raws[(key, k)]
        return postprocess(raw, eng.in_w, eng.in_h, size[0], size[1],
                           conf, nms_mode, nms_iou, one_per_class)

    # ---------------------------- CICLO 1 ---------------------------- #
    print("\n" + "=" * 70)
    print("CICLO 1 - umbral de confianza (NMS por clase 0.40, 1 por clase)")
    print("=" * 70)
    confs = {spec["key"]: [0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.5, 0.6, 0.7]
             for spec in MODEL_SPECS}
    best_conf = {}
    for spec in MODEL_SPECS:
        key = spec["key"]
        rows = []
        for c in confs[key]:
            pred_map = {k: seq_of(dets_for(key, k, c, "per-class", NMS_BASE, True))
                        for k in keys}
            m = evaluate(gt, pred_map)
            rows.append((score(m), c, m))
        rows.sort(reverse=True)
        best_conf[key] = rows[0][1]
        print(f"\n{key}: mejores umbrales")
        for s, c, m in rows[:4]:
            print(f"  conf>={c:<5} {fmt_metrics(m)}")
        print(f"  -> elegido: {best_conf[key]}")

    # ---------------------------- CICLO 2 ---------------------------- #
    print("\n" + "=" * 70)
    print("CICLO 2 - NMS y regla 1-por-clase")
    print("=" * 70)
    best_cfg = {}
    for spec in MODEL_SPECS:
        key = spec["key"]
        rows = []
        for nms_mode in ("none", "agnostic", "per-class"):
            ious = [NMS_BASE] if nms_mode == "none" else [0.3, 0.4, 0.5, 0.6, 0.7]
            for iou in ious:
                for one in (False, True):
                    pred_map = {k: seq_of(dets_for(key, k, best_conf[key], nms_mode, iou, one))
                                for k in keys}
                    m = evaluate(gt, pred_map)
                    rows.append((score(m), nms_mode, iou, one, m))
        rows.sort(reverse=True)
        best_cfg[key] = rows[0]
        print(f"\n{key}: mejores configuraciones")
        for s, mode, iou, one, m in rows[:5]:
            print(f"  nms={mode:<9} iou={iou:.1f} 1xclase={str(one):<5} {fmt_metrics(m)}")
        print(f"  -> elegido: nms={rows[0][1]} iou={rows[0][2]} 1xclase={rows[0][3]}")

    # ---------------------------- CICLO 3 ---------------------------- #
    print("\n" + "=" * 70)
    print("CICLO 3 - fusion de motores")
    print("=" * 70)
    def best_dets(key, k):
        _, mode, iou, one, _ = best_cfg[key]
        return dets_for(key, k, best_conf[key], mode, iou, one)

    base_pred = {}
    for k in keys:
        base_pred[k] = seq_of(best_dets("llegada", k))

    strategies = {}
    for mode in ("primary", "union-best", "primary-fill"):
        pred_map = {}
        for k in keys:
            primary = best_dets("llegada", k)
            secondary = best_dets("principal", k)
            pred_map[k] = seq_of(fuse(primary, secondary, mode))
        strategies[mode] = (evaluate(gt, pred_map), pred_map)

    # Tambien: fusion inversa (principal primario, llegada relleno)
    inv = {}
    for k in keys:
        primary = best_dets("principal", k)
        secondary = best_dets("llegada", k)
        inv[k] = seq_of(fuse(primary, secondary, "union-best"))
    strategies["union-best(principal first)"] = (evaluate(gt, inv), inv)

    # Relleno inteligente: caja fisica libre para la clase que falta.
    smart = {}
    for k in keys:
        all_boxes = (dets_for("llegada", k, 0.05, "per-class", 0.4, False)
                     + dets_for("principal", k, 0.05, "per-class", 0.4, False))
        smart[k] = seq_of(smart_fill(best_dets("llegada", k),
                                     best_dets("principal", k), all_boxes))
    strategies["smart-fill"] = (evaluate(gt, smart), smart)

    # smart-fill + correccion de la esfera negra por color.
    smart8 = {}
    for k in keys:
        all_boxes = (dets_for("llegada", k, 0.05, "per-class", 0.4, False)
                     + dets_for("principal", k, 0.05, "per-class", 0.4, False))
        dets = smart_fill(best_dets("llegada", k), best_dets("principal", k), all_boxes)
        info = row_info[k]
        dets = color8_fix(dets, info["slots"], info["black"],
                          info["my"], info["mh"], info["mw"])
        smart8[k] = seq_of(dets)
    strategies["smart-fill+color8"] = (evaluate(gt, smart8), smart8)

    ranked = sorted(strategies.items(), key=lambda kv: score(kv[1][0]), reverse=True)
    for name, (m, _) in ranked:
        print(f"  {name:<28} {fmt_metrics(m)}")

    win_name, (win_m, win_pred) = ranked[0]
    print(f"\n  -> ganadora: {win_name}  (score {score(win_m):.3f})")

    # Detalle por imagen de la ganadora
    print("\nDetalle ganadora (izq->der)")
    for k in sorted(keys, key=lambda x: int(re.search(r"\((\d+)\)", x).group(1))):
        print(f"  {k:<8} GT={''.join(map(str, gt[k]))}  PRED={''.join(map(str, win_pred[k]))}")

    # ---------------------------- JSON FINAL ---------------------------- #
    results = []
    for k in sorted(keys, key=lambda x: int(re.search(r"\((\d+)\)", x).group(1))):
        llegada_dets = best_dets("llegada", k)
        principal_dets = best_dets("principal", k)
        mode = win_name if win_name in ("primary", "union-best", "primary-fill") else "union-best"
        if win_name in ("smart-fill", "smart-fill+color8"):
            all_boxes = (dets_for("llegada", k, 0.05, "per-class", 0.4, False)
                         + dets_for("principal", k, 0.05, "per-class", 0.4, False))
            dets = smart_fill(llegada_dets, principal_dets, all_boxes)
            if win_name == "smart-fill+color8":
                info = row_info[k]
                dets = color8_fix(dets, info["slots"], info["black"],
                                  info["my"], info["mh"], info["mw"])
        elif win_name.startswith("union-best(principal"):
            dets = fuse(principal_dets, llegada_dets, "union-best")
        else:
            dets = fuse(llegada_dets, principal_dets, mode)
        out = []
        for i, d in enumerate(dets, 1):
            x1, y1, x2, y2 = d["bbox"]
            if any(d is p for p in llegada_dets):
                src = "llegada_best.engine"
            elif any(d is p for p in principal_dets):
                src = "modelo_1920x1088.engine"
            else:
                src = "relleno(fusion)"
            out.append({
                "orden": i,
                "clase_id": d["cid"],
                "clase": names[d["cid"]],
                "confianza": round(float(d["conf"]), 4),
                "bbox_xyxy": [round(v, 1) for v in (x1, y1, x2, y2)],
                "centro_xy": [round(float(d["cx"]), 1), round((y1 + y2) / 2, 1)],
                "fuente": src,
            })
        results.append({"modelo": win_name, "imagen": image_paths[k].name, "resultados": out})

    payload = {
        "generado": datetime.now().isoformat(timespec="seconds"),
        "imagenes_dir": str(args.images.resolve()),
        "verdad_campo": str(args.gt),
        "estrategia": win_name,
        "confianza": {k: v for k, v in best_conf.items()},
        "nms": {k: {"modo": v[1], "iou": v[2], "una_por_clase": v[3]}
                for k, v in best_cfg.items()},
        "metricas": {kk: {**vv[0], "score": score(vv[0])}
                     for kk, vv in strategies.items()},
        "resumen": {
            "coverage": win_m["coverage"], "purity": win_m["purity"],
            "posiciones_exactas": win_m["exact"], "kendall_tau": win_m["tau"],
            "score": score(win_m),
        },
        "resultados": results,
    }
    args.out.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\nJSON final: {args.out}")


if __name__ == "__main__":
    main()
