"""
Pasa los motores del ejecutable por un lote nuevo de imagenes y deja un JSON
con el mismo formato que predictions_tuned.json, listo para
annotate_predictions.py.

No usa verdad de campo. Estrategias:
    slot   : detecciones reales y, si falta una clase, la coloca en un hueco
             fisico de la fila; si el hueco tiene una bola de color sin
             detectar, el color decide (no usa cajas de fondo). Recomendada.
    tuned  : smart_fill + correccion de la esfera negra (como el lote afinado)
    fusion : solo detecciones reales de ambos motores, una por clase

Antes de cualquier estrategia se resuelven los pares de clases que el NMS
por clase deja sobre la misma bola (1/9, 2/4, 8/4) usando el color real.

Uso:
    python label_new_images.py
    python label_new_images.py --images cvat_images_nuevas \
        --out predictions_nuevas.json --estrategia slot
"""

import argparse
import colorsys
import json
import re
import sys
from datetime import datetime
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
# -I no agrega la carpeta del script a sys.path; el modulo vive aqui al lado.
sys.path.insert(0, str(HERE))
import tune_exe_models as T  # noqa: E402
DEFAULT_IMAGES = HERE / "cvat_images_nuevas"
DEFAULT_TUNED = HERE / "predictions_tuned.json"
DEFAULT_OUT = HERE / "predictions_nuevas.json"

MODEL_SPECS = [
    {"name": "modelo_1920x1088.engine", "key": "principal"},
    {"name": "llegada_best.engine", "key": "llegada"},
]


def natural_key(p: Path):
    return [int(t) if t.isdigit() else t.lower()
            for t in re.split(r"(\d+)", p.name)]


def _plausible(d, my, mh, mw):
    """Caja con tamano de esfera y dentro de la banda de la fila."""
    x1, y1, x2, y2 = d["bbox"]
    w, h, cy = x2 - x1, y2 - y1, (y1 + y2) / 2
    return (0.5 * mw <= w <= 1.8 * mw and 0.5 * mh <= h <= 1.8 * mh
            and abs(cy - my) <= 1.0 * mh)


def _patch_stats(hsv, bbox, frac=0.35):
    """Mediana H/S/V y fraccion de blanco de la fila mas blanca."""
    x1, y1, x2, y2 = bbox
    cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
    rw, rh = frac * (x2 - x1), frac * (y2 - y1)
    x0, x1i = max(0, int(cx - rw)), min(hsv.shape[1], int(cx + rw))
    y0, y1i = max(0, int(cy - rh)), min(hsv.shape[0], int(cy + rh))
    patch = hsv[y0:y1i, x0:x1i]
    if patch.size == 0:
        return 0.0, 0.0, 0.0, 0.0
    h, s, v = patch[..., 0], patch[..., 1], patch[..., 2]
    white = (s < 70) & (v > 180)
    rows = white.mean(axis=1)
    return (float(np.median(h)), float(np.median(s)), float(np.median(v)),
            float(rows.max()) if rows.size else 0.0)


def resolve_duplicate_boxes(dets, hsv):
    """Deja una sola etiqueta cuando dos clases caen sobre la misma bola.

    El NMS del motor es por clase, asi que pares como 1/9 (amarillas),
    2/4 (azul/morada) y 8/4 (negra/morada) pueden compartir caja. Decide
    con el color real: la banda ancha de la 9, el tono azul de la 2 y la
    oscuridad de la 8.
    """
    drop = set()
    for i, a in enumerate(dets):
        for b in dets[i + 1:]:
            if a["cid"] == b["cid"] or T.iou(a["bbox"], b["bbox"]) <= 0.85:
                continue
            h, s, v, row_white = _patch_stats(hsv, a["bbox"])
            pair = {a["cid"], b["cid"]}
            if pair == {1, 9}:
                winner = 9 if row_white >= 0.45 else 1
            elif pair == {2, 4}:
                winner = 2 if h <= 185 else 4
            elif pair == {8, 4}:
                winner = 8 if (v < 100 and s < 80) else 4
            else:
                winner = None
            if winner in pair:
                drop.add(id(b if a["cid"] == winner else a))
            else:
                drop.add(id(min((a, b), key=lambda d: d["conf"])))
    return [d for d in dets if id(d) not in drop]


def reassign_4_8(dets, hsv):
    """Corrige 4/8 con el color real: morada = 4, negra = 8.

    El motor a veces etiqueta la negra como morada (cap_0020) o al reves.
    El parche de la morada es saturado (S>=100); el de la negra es oscuro
    y poco saturado. Se muta la deteccion para conservar su identidad.
    """
    out = []
    for d in dets:
        if d["cid"] in (4, 8):
            _, s, v, _ = _patch_stats(hsv, d["bbox"])
            if d["cid"] == 8 and s >= 100:
                d["cid"] = 4
            elif d["cid"] == 4 and s < 80 and v < 110:
                d["cid"] = 8
        out.append(d)
    # Si el cambio creo dos etiquetas de la misma clase, deja la mas segura.
    best = {}
    for d in out:
        c = d["cid"]
        if c not in best or d["conf"] > best[c]["conf"]:
            best[c] = d
    return [d for d in out if best[d["cid"]] is d]


def _hue_dist(a, b):
    """Distancia circular entre dos tonos en escala 0-255."""
    d = abs(a - b)
    return min(d, 255 - d)


def _black_ball(hsv, x, my, mw, mh):
    """True si el parche parece una bola negra iluminada y no el riel.

    El riel vacio es mas oscuro (V<62) y el fondo de cuadros es saturado.
    """
    rw, rh = 0.30 * mw, 0.30 * mh
    x0, x1i = max(0, int(x - rw)), min(hsv.shape[1], int(x + rw))
    y0, y1i = max(0, int(my - rh)), min(hsv.shape[0], int(my + rh))
    patch = hsv[y0:y1i, x0:x1i]
    if patch.size == 0:
        return False
    s, v = patch[..., 1], patch[..., 2]
    return (float(np.median(s)) < 70 and 62 <= float(np.median(v)) <= 145
            and float((s >= 100).mean()) < 0.2)


def _ball_box(d):
    x1, y1, x2, y2 = d["bbox"]
    return 45 <= x2 - x1 <= 140 and 45 <= y2 - y1 <= 140


def _cy(d):
    return (d["bbox"][1] + d["bbox"][3]) / 2


def _ball_size(d):
    x1, y1, x2, y2 = d["bbox"]
    return ((x2 - x1) + (y2 - y1)) / 2


def _size_boxes(per_engine):
    return [(k, d) for k, dets in per_engine.items() for d in dets
            if _ball_box(d)]


def _rack_inliers(items, s):
    """Cajas alineadas con la pista; descarta falsos lejanos (RANSAC 2 puntos).

    Un grupo por cercania parte la pista cuando hay un hueco grande entre
    bolas (cap_0037/0040). La recta, en cambio, une todo el tramo y deja
    fuera la deteccion falsa de la esquina superior derecha.
    """
    if len(items) < 4:
        return items
    pts = [np.array([d["cx"], _cy(d)], dtype=float) for _, d in items]
    best = None
    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            dvec = pts[j] - pts[i]
            norm = float(np.linalg.norm(dvec))
            if norm < 1e-6:
                continue
            dvec = dvec / norm
            normal = np.array([-dvec[1], dvec[0]])
            inl = [k for k, pt in enumerate(pts)
                   if abs(float(np.dot(pt - pts[i], normal))) <= 1.5 * s]
            if len(inl) < 4:
                continue
            proj = [float(np.dot(pts[k] - pts[i], dvec)) for k in inl]
            score = (len(inl), max(proj) - min(proj))
            if best is None or score > best[0]:
                best = (score, inl)
    if best is None:
        return items
    return [items[k] for k in best[1]]


def _is_rack(per_engine, size):
    """True si las bolas no forman una fila horizontal (columna o diagonal).

    Las tomas nuevas muestran la pista en vertical o inclinada. Se descartan
    las detecciones falsas lejanas agrupando por cercania y se mira el alto
    del grupo: una fila horizontal tiene poco alto.
    """
    items = _size_boxes(per_engine)
    if len(items) < 4:
        return False
    s = float(np.median([_ball_size(d) for _, d in items]))
    group = _rack_inliers(items, s)
    if len(group) < 4:
        return False
    ys = [_cy(d) for _, d in group]
    span = max(ys) - min(ys)
    return span > 4.0 * s and span > 0.25 * min(size)


def reassign_3_7(dets, hsv):
    """Corrige 3/7 con el color real: roja = 3, vino = 7.

    En la columna el motor confunde la vino con roja (cap_0010, cap_0062).
    La roja tiene tono 0-3 y V alto; la vino ronda H=249 con V menor.
    """
    for d in dets:
        if d["cid"] in (3, 5, 7):
            h, s, v, _ = _patch_stats(hsv, d["bbox"])
            if d["cid"] in (3, 5) and 235 <= h <= 255 and s >= 100:
                # La vino tambien aparece como 5 (cap_0040).
                d["cid"] = 7
            elif d["cid"] == 7 and h <= 9 and s >= 130 and v >= 210:
                d["cid"] = 3
    # Una etiqueta por clase.
    best = {}
    for d in dets:
        c = d["cid"]
        if c not in best or d["conf"] > best[c]["conf"]:
            best[c] = d
    return [d for d in dets if best[d["cid"]] is d]


def reassign_1_9(dets, hsv):
    """Corrige 1/9 con la banda blanca: la 9 es rayada, la 1 amarilla lisa.

    En la pista vertical el motor suele llamar 9 a la amarilla lisa
    (cap_0037: numeros 1 y 9 intercambiados).
    """
    yellows = []
    band = {}
    for d in dets:
        if d["cid"] not in (1, 9):
            continue
        h, s, v, rw = _patch_stats(hsv, d["bbox"])
        band[id(d)] = rw
        if rw >= 0.45:
            d["cid"] = 9
        elif h >= 20:
            d["cid"] = 1
        yellows.append(d)
    # Dos amarillas distintas: una es la 1 y la otra la 9, aunque la banda
    # no se vea; se reparten por banda blanca en vez de perder una.
    if len(yellows) == 2 and yellows[0]["cid"] == yellows[1]["cid"]:
        a, b = yellows
        hi, lo = (a, b) if band[id(a)] >= band[id(b)] else (b, a)
        hi["cid"], lo["cid"] = 9, 1
    best = {}
    for d in dets:
        c = d["cid"]
        if c not in best or d["conf"] > best[c]["conf"]:
            best[c] = d
    return [d for d in dets if best[d["cid"]] is d]


def _circle_class(hsv, cx, cy, r):
    """Clase y puntaje de un circulo Hough segun el color del disco."""
    r0 = max(3, int(0.55 * r))
    y0, y1 = max(0, cy - r0), min(hsv.shape[0], cy + r0)
    x0, x1 = max(0, cx - r0), min(hsv.shape[1], cx + r0)
    p = hsv[y0:y1, x0:x1]
    if p.size == 0:
        return -1, 0.0
    yy, xx = np.mgrid[y0:y1, x0:x1]
    disk = (xx - cx) ** 2 + (yy - cy) ** 2 <= r0 ** 2
    if not disk.any():
        return -1, 0.0
    h, s, v = p[..., 0][disk], p[..., 1][disk], p[..., 2][disk]
    H, S, V = float(np.median(h)), float(np.median(s)), float(np.median(v))
    white = float(((s < 70) & (v > 180)).mean())
    band = float(((h >= 28) & (h <= 48) & (s >= 120)).mean())
    if V < 185 and S < 70 and 0.05 <= white < 0.50:
        # La bola 8 negra lleva el numero blanco; el cable/manguera negra
        # del fondo no (cap_0011: falso 8 sobre el resorte). El numero sube
        # la V mediana (cap_0043: V=150), por eso el tope es 185 y no 125;
        # una blanca tapada tiene mas de mitad de disco blanco.
        return 8, 1.0 - V / 255.0
    if white >= 0.20 and band >= 0.12:
        # Bola rayada amarilla (9): banda blanca con franja amarilla. El
        # disco parece blanco, pero la 0 no tiene amarillo.
        return 9, 0.5 + band
    if V > 190 and S < 45:
        return 0, V / 255.0
    if H <= 9 and S >= 140 and V >= 215:
        return 3, S / 255.0
    if 235 <= H <= 255 and S >= 100:
        return 7, S / 255.0
    if 150 <= H <= 170 and S >= 90:
        return 2, S / 255.0
    if 168 < H <= 192 and S >= 90:
        return 4, S / 255.0
    if 95 <= H <= 130 and S >= 90:
        return 6, S / 255.0
    if 12 <= H <= 27 and S >= 100:
        return 5, S / 255.0
    if 25 <= H <= 50 and S >= 120:
        return 9 if (S < 140 and white > 0.12) else 1, S / 255.0
    return -1, 0.0


def _rack_circles(rgb, hsv, chosen, param2=30):
    """Bolas candidatas por HoughCircles cerca de la pista detectada."""
    import cv2

    if not chosen:
        return []
    s = float(np.median([_ball_size(d) for d in chosen]))
    xs = [d["cx"] for d in chosen]
    ys = [_cy(d) for d in chosen]
    # Margen amplio: la pista puede doblarse (cap_0011) y las bolas que
    # siguen abajo quedan lejos de la recta de las detecciones (cap_0043).
    pad = 4.0 * s
    x0 = int(max(0, min(xs) - pad))
    x1 = int(min(hsv.shape[1], max(xs) + pad))
    y0 = int(max(0, min(ys) - pad))
    y1 = int(min(hsv.shape[0], max(ys) + pad))
    if x1 - x0 < 2 * s or y1 - y0 < 2 * s:
        return []
    gray = cv2.cvtColor(rgb[y0:y1, x0:x1], cv2.COLOR_RGB2GRAY)
    gray = cv2.medianBlur(gray, 5)
    circles = cv2.HoughCircles(
        gray, cv2.HOUGH_GRADIENT, dp=1.2, minDist=max(20.0, 0.75 * s),
        param1=100, param2=param2,
        minRadius=max(5, int(0.30 * s)), maxRadius=max(8, int(0.62 * s)))
    if circles is None:
        return []
    out = []
    for cx, cy, r in np.round(circles[0]).astype(int):
        gx, gy = int(cx + x0), int(cy + y0)
        cid, score = _circle_class(hsv, gx, gy, int(r))
        if cid < 0:
            continue
        # Banda blanca en todo el disco (la 9 tiene mucha, la 1 poca).
        rr = int(r)
        p = hsv[max(0, gy - rr):gy + rr, max(0, gx - rr):gx + rr]
        yy, xx = np.mgrid[max(0, gy - rr):gy + rr,
                          max(0, gx - rr):gx + rr]
        disk = (xx - gx) ** 2 + (yy - gy) ** 2 <= rr * rr
        if p.size and disk.any():
            s, v = p[..., 1][disk], p[..., 2][disk]
            white = float(((s < 70) & (v > 180)).mean())
        else:
            white = 0.0
        out.append({"cid": cid, "score": score, "white": white,
                    "bbox": [gx - r, gy - r, gx + r, gy + r],
                    "cx": float(gx)})
    return out


def _free(bbox, chosen, mw):
    """True si la caja no cae sobre otra ya elegida (distancia 2D)."""
    cx = (bbox[0] + bbox[2]) / 2
    cy = (bbox[1] + bbox[3]) / 2
    for d in chosen:
        if T.iou(bbox, d["bbox"]) > 0.3:
            return False
        if ((cx - d["cx"]) ** 2 + (cy - _cy(d)) ** 2
                <= (0.45 * mw) ** 2):
            return False
    return True


def rack_fill(primary, secondary, circles, mw):
    """Completa clases con el secundario y con circulos Hough, sin inventar.

    Un circulo solo se acepta si esta sobre la pista (recta de las
    detecciones) o pegado a una bola ya elegida; asi se agregan las bolas
    del tramo bajo (cap_0037/0043) sin tomar los botones negros del fondo.
    """
    chosen = list(primary)
    have = {d["cid"] for d in chosen}
    missing = [c for c in range(10) if c not in have]
    pending = []
    for c in missing:
        own = [d for d in secondary
               if d["cid"] == c and _free(d["bbox"], chosen, mw)]
        if not own and c == 9 and any(d["cid"] == 1 for d in chosen):
            # La otra amarilla del motor suele venir como 1; si ya hay una
            # 1 elegida, la segunda es la 9 (cap_0040).
            own = [d for d in secondary
                   if d["cid"] == 1 and _free(d["bbox"], chosen, mw)]
        if own:
            d = max(own, key=lambda d: d["conf"])
            chosen.append({"cid": c, "conf": d["conf"],
                           "bbox": d["bbox"], "cx": d["cx"]})
        else:
            pending.append(c)
    if not chosen or not pending:
        return chosen
    pts = np.array([[d["cx"], _cy(d)] for d in chosen], dtype=float)
    mean = pts.mean(axis=0)
    _, _, vt = np.linalg.svd(pts - mean)
    direction = vt[0]
    normal = np.array([-direction[1], direction[0]])
    proj = [float(np.dot(p - mean, direction)) for p in pts]
    t0, t1 = min(proj) - 2.0 * mw, max(proj) + 2.0 * mw
    t0e, t1e = min(proj) - 4.0 * mw, max(proj) + 4.0 * mw

    def near(q, ref):
        p = np.array([q["cx"], (q["bbox"][1] + q["bbox"][3]) / 2])
        return min(float(np.hypot(p[0] - d["cx"], p[1] - _cy(d)))
                   for d in ref)

    def usable(q):
        # Pegado a una bola ya elegida (tramo oculto/bajo) o sobre la pista
        # dentro del tramo; fuera de eso son botones/cables del fondo.
        if near(q, chosen) <= 2.2 * mw:
            return True
        p = np.array([q["cx"], (q["bbox"][1] + q["bbox"][3]) / 2])
        perp = abs(float(np.dot(p - mean, normal)))
        if perp > 1.5 * mw:
            return False
        t = float(np.dot(p - mean, direction))
        if t0 <= t <= t1:
            return True
        # Bola aislada al final de la pista (cap_0043): se acepta si cae
        # casi exactamente sobre la recta, no en el fondo.
        return t0e <= t <= t1e and perp <= 0.5 * mw

    def eff_cid(q):
        # Segunda amarilla lisa: si ya hay una 1 y falta la 9, el circulo
        # con banda blanca es la 9 (cap_0011).
        if (q["cid"] == 1 and q.get("white", 0.0) >= 0.12
                and any(d["cid"] == 1 for d in chosen)
                and not any(d["cid"] == 9 for d in chosen)):
            return 9
        return q["cid"]

    used = set()
    for _ in range(3):
        added = False
        for c in list(pending):
            cands = [q for q in circles
                     if eff_cid(q) == c and id(q) not in used
                     and _free(q["bbox"], chosen, mw) and usable(q)]
            if not cands:
                continue
            # La bola mas cercana a la pista gana al puntaje de color: el
            # boton negro del fondo no debe tapar la bola negra real.
            q = min(cands, key=lambda q: (near(q, chosen), -q["score"]))
            used.add(id(q))
            chosen.append({"cid": c, "conf": 0.0,
                           "bbox": list(q["bbox"]), "cx": q["cx"]})
            pending.remove(c)
            added = True
        if not added:
            break
    return chosen


def slot_fill(primary, secondary, slots, my, mh, mw, hsv, palette):
    """Rellena clases faltantes SOLO en huecos fisicos de la fila.

    Primero coloca las detecciones propias del secundario que sean plausibles;
    despues asigna las clases que siguen faltando a los slots vacios. Si el
    hueco tiene una bola de color que el motor no detecto, el color manda
    sobre el orden de clase; si el hueco esta vacio, se asigna de izquierda a
    derecha. Nunca usa cajas de fondo (logo, sombras).
    """
    chosen = list(primary)
    have = {d["cid"] for d in chosen}
    missing = [c for c in range(10) if c not in have]
    if not missing:
        return sorted(chosen, key=lambda d: d["cx"])

    occupied_boxes = [d["bbox"] for d in primary]

    def free_box(d):
        return max((T.iou(d["bbox"], b) for b in occupied_boxes),
                   default=0.0) < 0.3

    pending = []
    for c in missing:
        own = [d for d in secondary
               if d["cid"] == c and free_box(d) and _plausible(d, my, mh, mw)]
        if own:
            chosen.append(max(own, key=lambda d: d["conf"]))
        else:
            pending.append(c)

    spacing = (slots[-1] - slots[0]) / max(1, len(slots) - 1)

    def interior(i):
        x = slots[i]
        return (any(d["cx"] < x for d in chosen)
                and any(d["cx"] > x for d in chosen))

    occupied_slots = {
        i for i, x in enumerate(slots)
        if any(abs(d["cx"] - x) <= 0.45 * spacing for d in chosen)
    }
    empty = [i for i in range(len(slots)) if i not in occupied_slots]

    # Solo buscar la bola fuera del tramo detectado cuando todos los huecos
    # estan en los extremos. Si el hueco es interior, ahi va la que falta
    # (cap_0057 x563). En cap_0055 el unico hueco esta al final del tramo
    # y la negra real quedo antes de la primera bola; el riel no pasa.
    if not any(interior(i) for i in empty):
        extra = []
        for x in (slots[0] - spacing, slots[-1] + spacing):
            if 0 <= x < hsv.shape[1] and _black_ball(hsv, x, my, mw, mh):
                extra.append(float(x))
        if extra:
            slots = sorted([float(s) for s in slots] + extra)
            occupied_slots = {
                i for i, x in enumerate(slots)
                if any(abs(d["cx"] - x) <= 0.45 * spacing for d in chosen)
            }
            empty = [i for i in range(len(slots)) if i not in occupied_slots]

    # Color de cada hueco vacio: si hay una bola de color sin detectar, se
    # usa para desempatar clases pendientes (p.ej. 7 vino vs 9 amarilla).
    colored = []
    blackish = []
    for i in empty:
        x = slots[i]
        rw, rh = 0.30 * mw, 0.30 * mh
        x0, x1i = max(0, int(x - rw)), min(hsv.shape[1], int(x + rw))
        y0, y1i = max(0, int(my - rh)), min(hsv.shape[0], int(my + rh))
        patch = hsv[y0:y1i, x0:x1i]
        if patch.size == 0:
            continue
        h = float(np.median(patch[..., 0]))
        s = float(np.median(patch[..., 1]))
        v = float(np.median(patch[..., 2]))
        if s >= 100:
            colored.append((i, h, s, v))
        elif s < 70 and 62 <= v <= 145 and float((patch[..., 1] >= 100).mean()) < 0.2:
            blackish.append((i, v))

    matched = {}
    used_slots = set()
    # La negra tiene prioridad: se coloca en el hueco mas iluminado que
    # tiene pinta de bola negra (el riel real es mas oscuro).
    if 8 in pending and blackish:
        i = max(blackish, key=lambda t: t[1])[0]
        matched[8] = i
        used_slots.add(i)
    if colored:
        pairs = []
        for c in pending:
            if c in matched:
                continue
            ph, ps, pv = colorsys.rgb_to_hsv(*(q / 255.0 for q in palette[c]))
            for i, sh, ss, sv in colored:
                if i in used_slots:
                    continue
                cost = _hue_dist(sh, ph * 255)
                cost += 0.2 * abs(ss - ps * 255)
                cost += 0.15 * abs(sv - pv * 255)
                pairs.append((cost, c, i))
        pairs.sort()
        for cost, c, i in pairs:
            if cost <= 80 and c not in matched and i not in used_slots:
                matched[c] = i
                used_slots.add(i)

    rest_slots = [i for i in empty if i not in used_slots]
    rest_classes = [c for c in pending if c not in matched]
    for c, i in zip(rest_classes, rest_slots):
        matched[c] = i

    for c, i in matched.items():
        x = slots[i]
        chosen.append({
            "cid": c,
            "conf": 0.0,
            "bbox": [x - mw / 2, my - mh / 2, x + mw / 2, my + mh / 2],
            "cx": x,
        })
    return sorted(chosen, key=lambda d: d["cx"])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=Path, default=DEFAULT_IMAGES)
    ap.add_argument("--models", type=Path, default=T.DEFAULT_MODELS)
    ap.add_argument("--tuned", type=Path, default=DEFAULT_TUNED)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--estrategia", choices=("slot", "tuned", "fusion"),
                    default="slot")
    # Punto de la pista: en llegada la fila va horizontal; en salida la
    # columna va vertical/diagonal. Si no se indica, se deduce del lote.
    ap.add_argument("--zona", choices=("llegada", "salida"), default=None)
    args = ap.parse_args()

    cfg = json.loads(args.tuned.read_text(encoding="utf-8"))
    conf, nms = cfg["confianza"], cfg["nms"]
    pool = json.loads((HERE / "cvat_labels_pool.json").read_text(encoding="utf-8"))
    names = [c["name"] for c in pool]
    palette = []
    for c in pool:
        hx = c["color"].lstrip("#")
        palette.append(tuple(int(hx[i:i + 2], 16) for i in (0, 2, 4)))
    # La 9 es amarilla con banda blanca; para comparar color sirve el amarillo.
    palette[9] = (255, 255, 0)

    paths = sorted(args.images.glob("*.png"), key=natural_key)
    if not paths:
        raise SystemExit(f"No hay PNG en {args.images}")

    engines = {s["key"]: T.TrtEngine(args.models / s["name"]) for s in MODEL_SPECS}
    engine_names = {s["key"]: s["name"] for s in MODEL_SPECS}
    print(f"Imagenes: {len(paths)}  estrategia: {args.estrategia}")

    results = []
    modos = {"columna": 0, "fila": 0}
    for p in paths:
        with Image.open(p) as im:
            size = im.size
            hsv = np.asarray(im.convert("HSV"), dtype=np.float32)
            rgb_u8 = np.asarray(im.convert("RGB"), dtype=np.uint8)
            raws = {k: e.infer(im) for k, e in engines.items()}

        per_engine = {}
        for key in ("llegada", "principal"):
            per_engine[key] = T.postprocess(
                raws[key], engines[key].in_w, engines[key].in_h,
                size[0], size[1], conf[key], nms[key]["modo"],
                nms[key]["iou"], nms[key]["una_por_clase"])

        rack = _is_rack(per_engine, size)
        if rack:
            # La pista nueva va en columna o inclinada. Se agrupan las cajas
            # reales (descarta falsos lejanos) y se completa con Hough.
            items = _size_boxes(per_engine)
            s = float(np.median([_ball_size(d) for _, d in items]))
            group = _rack_inliers(items, s)
            counts = {}
            for k, _ in group:
                counts[k] = counts.get(k, 0) + 1
            primary_key = max(counts, key=counts.get)
            secondary_key = ("principal" if primary_key == "llegada"
                             else "llegada")
            primary = [d for k, d in group if k == primary_key]
            secondary = [d for k, d in group if k == secondary_key]
            primary_src = engine_names[primary_key]
            secondary_src = engine_names[secondary_key]
            primary = resolve_duplicate_boxes(primary, hsv)
            primary = reassign_4_8(primary, hsv)
            primary = reassign_3_7(primary, hsv)
            primary = reassign_1_9(primary, hsv)
            secondary = reassign_4_8(secondary, hsv)
            secondary = reassign_3_7(secondary, hsv)
            secondary = reassign_1_9(secondary, hsv)
            size_ball = float(np.median([_ball_size(d) for d in primary]))
            circles = _rack_circles(rgb_u8, hsv, primary)
            dets = rack_fill(primary, secondary, circles, size_ball)
            # En esta toma el orden es de arriba hacia abajo.
            dets.sort(key=lambda d: (_cy(d), d["cx"]))
        else:
            primary_src = "llegada_best.engine"
            secondary_src = "modelo_1920x1088.engine"
            slots, my, mh, mw = T.row_slots(
                engines, {(k, p.name): (raws[k], size) for k in raws},
                p.name, size)
            black = T.black_slot(hsv, slots, my, mh)
            primary = per_engine["llegada"]
            secondary = per_engine["principal"]
            # Una bola = una etiqueta: resuelve pares 1/9, 2/4 y 8/4 que el
            # NMS por clase deja sobre la misma caja.
            primary = resolve_duplicate_boxes(primary, hsv)
            # El color real decide entre morada (4) y negra (8) en ambos motores.
            primary = reassign_4_8(primary, hsv)
            secondary = reassign_4_8(secondary, hsv)
            all_boxes = (
                T.postprocess(raws["llegada"], engines["llegada"].in_w,
                              engines["llegada"].in_h, size[0], size[1],
                              0.05, "per-class", 0.4, False)
                + T.postprocess(raws["principal"], engines["principal"].in_w,
                                engines["principal"].in_h, size[0], size[1],
                                0.05, "per-class", 0.4, False))

            if args.estrategia == "tuned":
                dets = T.smart_fill(primary, secondary, all_boxes)
                dets = T.color8_fix(dets, slots, black, my, mh, mw)
            elif args.estrategia == "slot":
                dets = slot_fill(primary, secondary, slots, my, mh, mw,
                                 hsv, palette)
            else:
                dets = T.fuse(primary, secondary, "union-best")

        modo = "columna" if rack else "fila"
        modos[modo] += 1
        out = []
        for i, d in enumerate(dets, 1):
            x1, y1, x2, y2 = d["bbox"]
            if any(d is q for q in primary):
                src = primary_src
            elif any(d is q for q in secondary):
                src = secondary_src
            else:
                src = "relleno(fusion)"
            out.append({
                "orden": i,
                "clase_id": d["cid"],
                "clase": names[d["cid"]],
                "confianza": round(float(d["conf"]), 4),
                "bbox_xyxy": [round(float(v), 1) for v in (x1, y1, x2, y2)],
                "centro_xy": [round(float(d["cx"]), 1), round((y1 + y2) / 2, 1)],
                "fuente": src,
            })

        seq = "".join(str(x["clase_id"]) for x in out)
        print(f"  {p.name:<14} {len(out):2d} etiquetas  seq={seq}")
        results.append({"modelo": args.estrategia, "imagen": p.name,
                        "modo": modo, "motor_principal": primary_src,
                        "resultados": out})

    zona = args.zona or ("salida" if modos["columna"] > modos["fila"]
                         else "llegada")
    payload = {
        "generado": datetime.now().isoformat(timespec="seconds"),
        "imagenes_dir": str(args.images.resolve()),
        "verdad_campo": None,
        "zona": zona,
        "estrategia": args.estrategia,
        "modos": modos,
        "confianza": conf,
        "nms": nms,
        "resumen": {"nota": "lote nuevo sin verdad de campo"},
        "resultados": results,
    }
    args.out.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                        encoding="utf-8")
    print(f"\nJSON: {args.out}")


if __name__ == "__main__":
    main()
