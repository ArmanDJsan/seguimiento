"""Cerradura de salida: ¿estan las 10 esferas en la pista?

Evidencia, por capas:
  1. Numeros legibles (el contador de count_balls): una clase vista = una
     esfera distinta.
  2. Bola sin numero: circulo Hough sobre la linea de la pista, a una
     posicion propia, con interior claro (la 0 blanca) u oscuro (la 8
     negra). Asi se cuenta una bola tapada de la que no se lee el numero.

Estados (fail-closed: solo LISTO abre):
  LISTO          10 numeros legibles. Se puede liberar.
  LISTO_SIN_NUMERO  9 numeros + 1 bola validada por forma. Cuenta 10,
                 pero falta el numero: requiere decision del operador.
  FALTAN         menos de 10 esferas.
  DUDOSO         evidencia contradictoria (mas de una bola sin numero,
                 o el extra no tiene aspecto de bola).

Uso:
    python -I training\\gate_salida.py --images training\\cvat_images_lote4
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
sys.path.insert(0, str(HERE))

import count_balls as C  # noqa: E402
import label_new_images as L  # noqa: E402
import tune_exe_models as T  # noqa: E402

DEFAULT_MODELS = T.DEFAULT_MODELS
DEFAULT_TUNED = HERE / "predictions_tuned.json"


def _evidence(im, engines, conf, nms):
    """Escala, detecciones, linea de la pista y clases del contador."""
    size = im.size
    hsv = np.asarray(im.convert("HSV"), dtype=np.float32)
    rgb = np.asarray(im.convert("RGB"), dtype=np.uint8)
    raws = {k: e.infer(im) for k, e in engines.items()}
    per = {k: T.postprocess(raws[k], engines[k].in_w, engines[k].in_h,
                            size[0], size[1], conf[k], nms[k]["modo"],
                            nms[k]["iou"], nms[k]["una_por_clase"])
           for k in raws}
    items = L._size_boxes(per)
    if not items:
        return None
    s = float(np.median([L._ball_size(d) for _, d in items]))
    dets = [d for _, d in L._rack_inliers(items, s)]
    clusters = C._merge(dets, s)
    base = C.count_image(im, engines, conf, nms)
    pts = np.array([[d["cx"], L._cy(d)] for d in dets], dtype=float)
    mean = pts.mean(axis=0)
    _, _, vt = np.linalg.svd(pts - mean)
    direction = vt[0]
    normal = np.array([-direction[1], direction[0]])
    proj = (pts - mean) @ direction
    line_perp = float(np.mean((pts - mean) @ normal))
    return {"s": s, "dets": dets, "clusters": clusters, "base": base,
            "hsv": hsv, "rgb": rgb, "mean": mean, "direction": direction,
            "normal": normal, "proj": proj, "line_perp": line_perp,
            "size": size}


def _extra_balls(ev):
    """Bolas sin numero: circulos propios con interior claro u oscuro."""
    s, clusters = ev["s"], ev["clusters"]
    hsv, rgb, mean = ev["hsv"], ev["rgb"], ev["mean"]
    direction, normal = ev["direction"], ev["normal"]
    line_perp, proj = ev["line_perp"], ev["proj"]
    w_img, h_img = ev["size"]
    pts = np.array([[q["cx"], q["cy"]] for q in clusters], dtype=float)
    pad = 2.0 * s
    x0 = int(max(0, pts[:, 0].min() - pad))
    x1 = int(min(w_img, pts[:, 0].max() + pad))
    y0 = int(max(0, pts[:, 1].min() - pad))
    y1 = int(min(h_img, pts[:, 1].max() + pad))
    gray = cv2.medianBlur(cv2.cvtColor(rgb[y0:y1, x0:x1], cv2.COLOR_RGB2GRAY), 5)
    circles = cv2.HoughCircles(
        gray, cv2.HOUGH_GRADIENT, dp=1.2, minDist=max(20.0, 0.5 * s),
        param1=100, param2=20,
        minRadius=max(5, int(0.42 * s)), maxRadius=max(8, int(0.62 * s)))
    out = []
    if circles is None:
        return out
    v = hsv[..., 2]
    hh, ss = hsv[..., 0], hsv[..., 1]
    for cx, cy, r in np.round(circles[0]).astype(int):
        gx, gy = int(cx + x0), int(cy + y0)
        qq = float((np.array([gx, gy]) - mean) @ normal) - line_perp
        tt = float((np.array([gx, gy]) - mean) @ direction)
        if abs(qq) > 0.40 * s or not (proj.min() - 1.2 * s <= tt
                                      <= proj.max() + 1.2 * s):
            continue
        if min(np.hypot(gx - k["cx"], gy - k["cy"]) for k in clusters) < 0.50 * s:
            continue
        rr = max(6, int(0.40 * s))
        ya, yb = max(0, gy - rr), min(h_img, gy + rr)
        xa, xb = max(0, gx - rr), min(w_img, gx + rr)
        yy, xx = np.mgrid[ya:yb, xa:xb]
        disk = (xx - gx) ** 2 + (yy - gy) ** 2 <= rr * rr
        if not disk.any():
            continue
        mv = float(np.median(v[ya:yb, xa:xb][disk]))
        ms = float(np.median(ss[ya:yb, xa:xb][disk]))
        if mv >= 165 and ms <= 80:
            tipo = "blanca"
        elif mv <= 110:
            tipo = "negra"
        else:
            continue
        if all(np.hypot(gx - o["cx"], gy - o["cy"]) >= 0.5 * s for o in out):
            out.append({"cx": gx, "cy": gy, "r": int(r), "tipo": tipo,
                        "V": round(mv), "S": round(ms)})
    return out


def gate_image(im, engines, conf, nms):
    ev = _evidence(im, engines, conf, nms)
    if ev is None:
        return {"estado": "FALTAN", "esferas": 0, "certificado": False,
                "nota": "sin detecciones"}
    base = ev["base"]
    clases = base["clases"]
    falta = base["faltantes"]
    # Una bola sin numero solo es admisible si su color es el que le toca a
    # la clase que falta: la 0 es blanca y la 8 es negra. Cualquier otra
    # (o una blanca cuando la 0 ya se conto) es un falso circulo y no suma.
    validos = []
    if len(clases) < 10:
        for e in _extra_balls(ev):
            if (falta == [0] and e["tipo"] == "blanca") or \
                    (falta == [8] and e["tipo"] == "negra"):
                validos.append(e)
    total = len(clases) + len(validos)
    if len(clases) >= 10:
        estado, nota = "LISTO", "10 numeros legibles"
    elif total >= 10 and len(validos) == 1:
        estado = "LISTO_SIN_NUMERO"
        nota = (f"9 numeros + 1 bola {validos[0]['tipo']} sin numero "
                f"(clase {falta[0]} sin leer)")
    elif total >= 10:
        estado, nota = "DUDOSO", f"{len(validos)} bolas sin numero"
    else:
        estado = "FALTAN"
        nota = f"faltan {10 - total} esferas"
    return {
        "estado": estado,
        "abrir": estado == "LISTO",
        "certificado": estado == "LISTO",
        "esferas": total,
        "clases": clases,
        "faltantes": falta,
        "extras": validos,
        "escala_px": base["escala_px"],
        "nota": nota,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=Path, default=HERE / "cvat_images_lote4")
    ap.add_argument("--models", type=Path, default=DEFAULT_MODELS)
    ap.add_argument("--tuned", type=Path, default=DEFAULT_TUNED)
    ap.add_argument("--out", type=Path, default=HERE / "gate_salida.json")
    args = ap.parse_args()

    cfg = json.loads(args.tuned.read_text(encoding="utf-8"))
    conf, nms = cfg["confianza"], cfg["nms"]
    engines = {s["key"]: T.TrtEngine(args.models / s["name"])
               for s in L.MODEL_SPECS}
    paths = sorted(args.images.glob("*.png"), key=L.natural_key)
    if not paths:
        raise SystemExit(f"No hay PNG en {args.images}")

    results = []
    for p in paths:
        with Image.open(p) as im:
            r = gate_image(im, engines, conf, nms)
        r["imagen"] = p.name
        results.append(r)
        ext = ",".join(f"{e['tipo']}@{e['cx']},{e['cy']}"
                       for e in r["extras"]) or "-"
        print(f"  {p.name:<14} {r['estado']:<16} {r['esferas']:2d} "
              f"clases={len(r['clases']):2d} extras={ext}")

    total = len(results)
    cuenta = {}
    for r in results:
        cuenta[r["estado"]] = cuenta.get(r["estado"], 0) + 1
    payload = {
        "generado": datetime.now().isoformat(timespec="seconds"),
        "imagenes_dir": str(args.images.resolve()),
        "resumen": {"imagenes": total, **cuenta,
                    "abrir": sum(1 for r in results if r["abrir"])},
        "resultados": results,
    }
    args.out.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                        encoding="utf-8")
    print("\nresumen:", payload["resumen"])
    print("JSON:", args.out)


if __name__ == "__main__":
    main()
