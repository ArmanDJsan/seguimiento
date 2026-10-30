"""Mosaico de los recortes detectados (a resolucion original) para ver QUE detecta."""

import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from predict_video import (DEFAULT_MODELS, HERE, TrtEngine, class_color,
                           load_class_names, postprocess_frame)

VIDEO = Path(r"c:\Users\TecnologíaIntegraTec\Desktop\27-10-2026 Mercedes Pista V 3.0.1"
             r"\20261029 - Camera4 - [16-36-35] [16-37-35].mp4")
OUT = HERE / "video_inferencia" / "recortes.png"
MODEL = "yolo26m_b1.engine"
CONF = 0.30
FRAMES = [120, 480, 900, 1320]
CELL = 160
COLS = 12

names = load_class_names()
cap = cv2.VideoCapture(str(VIDEO))
eng = TrtEngine(DEFAULT_MODELS / MODEL)

crops = []
for idx in FRAMES:
    cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
    ok, f = cap.read()
    if not ok:
        continue
    h, w = f.shape[:2]
    raw = eng.infer_batch([f])[0]
    dets, _ = postprocess_frame(raw, (eng.in_w, eng.in_h), (w, h), CONF, names)
    for d in dets:
        x1, y1, x2, y2 = (int(v) for v in d["bbox"])
        pad = 8
        crop = f[max(0, y1 - pad):min(h, y2 + pad), max(0, x1 - pad):min(w, x2 + pad)]
        if crop.size == 0:
            continue
        crop = cv2.resize(crop, (CELL, CELL), interpolation=cv2.INTER_NEAREST)
        cv2.rectangle(crop, (0, 0), (CELL - 1, CELL - 1), class_color(d["cls"]), 2)
        cv2.putText(crop, f"{names[d['cls']][-1]} {d['conf']:.2f}", (6, CELL - 8),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 4, cv2.LINE_AA)
        cv2.putText(crop, f"{names[d['cls']][-1]} {d['conf']:.2f}", (6, CELL - 8),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, class_color(d["cls"]), 1, cv2.LINE_AA)
        crops.append(crop)
cap.release()

print(f"recortes: {len(crops)}")
if crops:
    while len(crops) % COLS:
        crops.append(np.zeros((CELL, CELL, 3), np.uint8))
    rows = [np.hstack(crops[i:i + COLS]) for i in range(0, len(crops), COLS)]
    sheet = np.vstack(rows)
    cv2.imwrite(str(OUT), sheet)
    print(f"Guardado: {OUT}  {sheet.shape[1]}x{sheet.shape[0]}")
