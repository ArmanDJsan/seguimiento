"""Hoja de contactos: mismo frame con varios modelos/umbrales, para comparar a ojo."""

import json
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from predict_video import (DEFAULT_MODELS, HERE, TrtEngine, class_color,
                           load_class_names, postprocess_frame)

VIDEO = Path(r"c:\Users\TecnologíaIntegraTec\Desktop\27-10-2026 Mercedes Pista V 3.0.1"
             r"\20261029 - Camera4 - [16-36-35] [16-37-35].mp4")
OUT = HERE / "video_inferencia" / "comparativa.png"
PANEL_W = 1200
FRAMES = [120, 480, 900, 1320]
COMBOS = [
    ("modelo_1920x1088.engine", 0.60),
    ("modelo_1920x1088.engine", 0.30),
    ("yolo26m_b1.engine", 0.60),
    ("yolo26m_b1.engine", 0.30),
]

names = load_class_names()
cap = cv2.VideoCapture(str(VIDEO))
frames = {}
for idx in FRAMES:
    cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
    ok, f = cap.read()
    if ok:
        frames[idx] = f
cap.release()

engines = {}
panels = []
for model, conf in COMBOS:
    if model not in engines:
        engines[model] = TrtEngine(DEFAULT_MODELS / model)
    eng = engines[model]
    for idx, f in frames.items():
        h, w = f.shape[:2]
        raw = eng.infer_batch([f])[0]
        dets, _ = postprocess_frame(raw, (eng.in_w, eng.in_h), (w, h), conf, names)
        scale = PANEL_W / w
        vis = cv2.resize(f, (PANEL_W, int(h * scale)), interpolation=cv2.INTER_AREA)
        for d in dets:
            x1, y1, x2, y2 = (int(v * scale) for v in d["bbox"])
            c = class_color(d["cls"])
            cv2.rectangle(vis, (x1, y1), (x2, y2), c, 3)
            cv2.putText(vis, f"{names[d['cls']]} {d['conf']:.2f}", (x1, max(20, y1 - 6)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 0), 4, cv2.LINE_AA)
            cv2.putText(vis, f"{names[d['cls']]} {d['conf']:.2f}", (x1, max(20, y1 - 6)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, c, 2, cv2.LINE_AA)
        cv2.putText(vis, f"{model.replace('.engine', '')}  conf>={conf}  n={len(dets)}  f{idx}",
                    (10, 32), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 255), 2, cv2.LINE_AA)
        panels.append(vis)

# rejilla 4 columnas x len(frames) filas
rows = len(frames)
grid = [np.hstack(panels[r * 4:(r + 1) * 4]) for r in range(rows)]
sheet = np.vstack(grid)
cv2.imwrite(str(OUT), sheet)
print(f"Guardado: {OUT}  {sheet.shape[1]}x{sheet.shape[0]}")
