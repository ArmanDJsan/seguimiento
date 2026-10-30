"""Conteo de presencia de las 10 esferas (punto de salida).

No etiqueta clases para CVAT: responde "¿estan las 10 esferas?".

Fuentes, en orden:
  1. Detecciones de los dos motores (posiciones y clases).
  2. Circulos Hough con umbral bajo (recupera bolas tapadas, la 8 negra).
  3. Mascaras de color por clase, asociadas a una esfera ya vista: azul 2,
     roja 3, morada 4, naranja 5, verde 6, vino 7 y dos amarillas (1 y 9).

El conteo `esferas` es de numeros identificados: cada esfera tiene un
numero unico, asi que una clase vista = una bola en la mesa. Una bola
tapada sin numero resoluble queda en `faltantes` (no se cuenta a ciegas).

Uso:
    python -I training\\count_balls.py --images training\\cvat_images_lote4 ^
        --out training\\conteo_lote4.json
"""
import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # python -I no agrega el directorio del script

import label_new_images as L  # noqa: E402
import tune_exe_models as T  # noqa: E402

DEFAULT_MODELS = T.DEFAULT_MODELS
DEFAULT_TUNED = HERE / "predictions_tuned.json"

# (H min, H max, S min, V min) por clase.
COLOR_SPECS = {
    2: (150, 170, 120, 150),   # azul
    3: (-1, 5, 120, 200),      # roja (la vino es H 235-255)
    4: (168, 196, 80, 120),    # morada
    5: (12, 27, 100, 200),     # naranja
    6: (95, 130, 90, 80),      # verde
    7: (235, 255, 100, 100),   # vino
}


def _blobs(mask):
    """Blobs (area, cx, cy, ancho, alto) de una mascara binaria."""
    _, _, stats, cent = cv2.connectedComponentsWithStats(mask, 8)
    return [(int(st[4]), float(c[0]), float(c[1]), int(st[2]), int(st[3]))
            for st, c in zip(stats[1:], cent[1:])]


def _near(cx, cy, clusters, s, k):
    return min(np.hypot(cx - q["cx"], cy - q["cy"]) for q in clusters) <= k * s


def _yellow_class(hsv, cx, cy, s):
    """1 lisa o 9 rayada segun la mancha blanca del disco."""
    cx, cy = int(round(cx)), int(round(cy))
    r = max(6, int(0.45 * s))
    y0, y1 = max(0, cy - r), min(hsv.shape[0], cy + r)
    x0, x1 = max(0, cx - r), min(hsv.shape[1], cx + r)
    p = hsv[y0:y1, x0:x1]
    if p.size == 0:
        return 1
    yy, xx = np.mgrid[y0:y1, x0:x1]
    disk = (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r
    white = float(((p[..., 1][disk] < 70) & (p[..., 2][disk] > 180)).mean())
    return 9 if white >= 0.15 else 1


def _masks(hsv, band, s, clusters, clases, on_line):
    """Clases que delatan una esfera con su color, junto a una ya vista.

    Se exige que la mancha caiga sobre una esfera contada o sobre la linea
    de la pista: asi el naranja o el amarillo de la maquina (fuera de la
    pista) no suma clases, pero si una bola que el motor salto entre otras.
    """
    h, sa, v = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    ball = np.pi * (0.5 * s) ** 2
    out = {}
    for cid, (h0, h1, smin, vmin) in COLOR_SPECS.items():
        m = ((h >= h0) & (h <= h1) & (sa >= smin) & (v >= vmin)
             & band).astype(np.uint8)
        # Umbral bajo: una bola tapada deja una franja chica (cap_0020: la
        # naranja 5 tiene 873 px repartidos en varias manchas).
        if any(a >= 0.06 * ball
               and (_near(cx, cy, clusters, s, 1.5) or on_line(cx, cy))
               for a, cx, cy, _, _ in _blobs(m)):
            out[cid] = True

    # Amarillas: la 1 lisa y la 9 rayada. Dos manchas separadas, o una sola
    # que abarca dos bolas pegadas (cap_0020/0055), = estan las dos.
    m = ((h >= 28) & (h <= 50) & (sa >= 140) & (v >= 200)
         & band).astype(np.uint8)
    ys = [(cx, cy, w, hh) for a, cx, cy, w, hh in _blobs(m)
          if a >= 0.10 * ball
          and (_near(cx, cy, clusters, s, 1.5) or on_line(cx, cy))]
    if ys:
        apart = any(np.hypot(a[0] - b[0], a[1] - b[1]) >= 0.65 * s
                    for i, a in enumerate(ys) for b in ys[i + 1:])
        spans = any(max(w, hh) >= 1.35 * s for _, _, w, hh in ys)
        if apart or spans:
            out[1] = True
            out[9] = True
        elif not ({1, 9} & clases):
            out[_yellow_class(hsv, ys[0][0], ys[0][1], s)] = True
    return out


def _band(dets, s, pad=1.5):
    xs = [d["cx"] for d in dets]
    ys = [L._cy(d) for d in dets]
    return (int(min(xs) - pad * s), int(max(xs) + pad * s),
            int(min(ys) - pad * s), int(max(ys) + pad * s))


def _gate(dets, s):
    """Cerca de las esferas vistas o sobre la recta de la pista."""
    pts = np.array([[d["cx"], L._cy(d)] for d in dets], dtype=float)
    mean = pts.mean(axis=0)
    _, _, vt = np.linalg.svd(pts - mean)
    direction = vt[0]
    normal = np.array([-direction[1], direction[0]])
    proj = (pts - mean) @ direction
    t0, t1 = proj.min() - 2.0 * s, proj.max() + 2.0 * s

    def usable(cx, cy):
        p = np.array([cx, cy], dtype=float)
        if min(np.hypot(*(p - q)) for q in pts) <= 2.2 * s:
            return True
        if abs(float(np.dot(p - mean, normal))) > 1.5 * s:
            return False
        return t0 <= float(np.dot(p - mean, direction)) <= t1

    return usable


def _merge(dets, s):
    """Una posicion por esfera; se queda la deteccion de mayor confianza."""
    out = []
    for d in sorted(dets, key=lambda d: -d["conf"]):
        p = (d["cx"], L._cy(d))
        if all(np.hypot(p[0] - q["cx"], p[1] - q["cy"]) > 0.5 * s
               for q in out):
            out.append({"cx": p[0], "cy": p[1], "cid": d["cid"],
                        "fuente": "motor"})
    return out


def count_image(im, engines, conf, nms):
    size = im.size
    hsv = np.asarray(im.convert("HSV"), dtype=np.float32)
    rgb = np.asarray(im.convert("RGB"), dtype=np.uint8)
    raws = {k: e.infer(im) for k, e in engines.items()}
    per = {}
    for k in raws:
        per[k] = T.postprocess(raws[k], engines[k].in_w, engines[k].in_h,
                               size[0], size[1], conf[k], nms[k]["modo"],
                               nms[k]["iou"], nms[k]["una_por_clase"])
    items = L._size_boxes(per)
    if not items:
        return {"esferas": 0, "clases": [], "faltantes": list(range(10)),
                "escala_px": None, "banda_xyxy": None}
    s = float(np.median([L._ball_size(d) for _, d in items]))
    dets = [d for _, d in L._rack_inliers(items, s)]
    clusters = _merge(dets, s)
    x0, x1, y0, y1 = _band(dets, s)
    band = np.zeros(hsv.shape[:2], bool)
    band[max(0, y0):y1, max(0, x0):x1] = True

    clases = {c["cid"] for c in clusters if c["cid"] >= 0}

    # La 8 negra es la unica sin color propio. Hough con umbral bajo la
    # recupera tapada (cap_0011/0043); su numero blanco evita falsos 8 en
    # el cable y los botones del carril. Las demas clases salen del motor
    # o de la mascara de color.
    usable = _gate(dets, s)
    for q in L._rack_circles(rgb, hsv, dets, param2=22):
        if q["cid"] != 8:
            continue
        cx = q["cx"]
        cy = (q["bbox"][1] + q["bbox"][3]) / 2
        if x0 <= cx <= x1 and y0 <= cy <= y1 and usable(cx, cy):
            clases.add(8)

    clases.update(_masks(hsv, band, s, clusters, clases, usable))
    return {
        "esferas": len(clases),
        "clases": sorted(clases),
        "faltantes": [c for c in range(10) if c not in clases],
        "escala_px": round(s, 1),
        "banda_xyxy": [x0, y0, x1, y1],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=Path,
                    default=HERE / "cvat_images_lote4")
    ap.add_argument("--models", type=Path, default=DEFAULT_MODELS)
    ap.add_argument("--tuned", type=Path, default=DEFAULT_TUNED)
    ap.add_argument("--out", type=Path, default=HERE / "conteo_esferas.json")
    args = ap.parse_args()

    cfg = json.loads(args.tuned.read_text(encoding="utf-8"))
    conf, nms = cfg["confianza"], cfg["nms"]
    engines = {s["key"]: T.TrtEngine(args.models / s["name"])
               for s in L.MODEL_SPECS}

    paths = sorted(args.images.glob("*.png"), key=L.natural_key)
    if not paths:
        raise SystemExit(f"No hay PNG en {args.images}")
    print(f"Imagenes: {len(paths)}")
    results = []
    for p in paths:
        with Image.open(p) as im:
            r = count_image(im, engines, conf, nms)
        r["imagen"] = p.name
        results.append(r)
        falt = ",".join(str(c) for c in r["faltantes"]) or "-"
        print(f"  {p.name:<14} {r['esferas']:2d} esferas  "
              f"{len(r['clases']):2d}/10 clases  faltan: {falt}")

    total = len(results)
    full = sum(1 for r in results if r["esferas"] == 10)
    payload = {
        "generado": datetime.now().isoformat(timespec="seconds"),
        "imagenes_dir": str(args.images.resolve()),
        "resumen": {
            "imagenes": total,
            "con_10_esferas": full,
            "con_menos": total - full,
            "promedio_esferas": round(
                sum(r["esferas"] for r in results) / total, 2),
        },
        "resultados": results,
    }
    args.out.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                        encoding="utf-8")
    print(f"\n{full}/{total} imagenes con las 10 esferas")
    print(f"JSON: {args.out}")


if __name__ == "__main__":
    main()
