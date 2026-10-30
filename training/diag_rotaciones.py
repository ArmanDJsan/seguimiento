"""
Diagnostico: prueba rotaciones y modelos sobre frames muestreados del video
para ver cual combinacion detecta mejor las esferas de la pista.
"""

import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from predict_video import DEFAULT_MODELS, TrtEngine, load_class_names, postprocess_frame

VIDEO = Path(r"c:\Users\TecnologíaIntegraTec\Desktop\27-10-2026 Mercedes Pista V 3.0.1"
             r"\20261029 - Camera4 - [16-36-35] [16-37-35].mp4")
MODELS = ["modelo_1920x1088.engine", "yolo26m_b1.engine", "llegada_best.engine"]
ROTATIONS = [None, cv2.ROTATE_90_CLOCKWISE, cv2.ROTATE_180, cv2.ROTATE_90_COUNTERCLOCKWISE]
ROT_NAMES = ["0", "90cw", "180", "90ccw"]
N_FRAMES = 24
CONF = 0.25

names = load_class_names()
cap = cv2.VideoCapture(str(VIDEO))
total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
idxs = [int(total * (i + 1) / (N_FRAMES + 1)) for i in range(N_FRAMES)]
frames = {}
for idx in idxs:
    cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
    ok, f = cap.read()
    if ok:
        frames[idx] = f
cap.release()
print(f"Frames muestreados: {len(frames)} de {total}\n")

for model in MODELS:
    path = DEFAULT_MODELS / model
    if not path.exists():
        print(f"AVISO: falta {model}")
        continue
    eng = TrtEngine(path)
    print(f"=== {model}  (entrada {eng.in_w}x{eng.in_h}) ===")
    for rot, rname in zip(ROTATIONS, ROT_NAMES):
        tot_dets = 0
        confs: list[float] = []
        per_class: dict[str, int] = {}
        for f in frames.values():
            img = f if rot is None else cv2.rotate(f, rot)
            h, w = img.shape[:2]
            raw = eng.infer_batch([img])[0]
            dets, _ = postprocess_frame(raw, (eng.in_w, eng.in_h), (w, h), CONF, names)
            tot_dets += len(dets)
            for d in dets:
                per_class[names[d["cls"]]] = per_class.get(names[d["cls"]], 0) + 1
                confs.append(d["conf"])
        mean_c = float(np.mean(confs)) if confs else 0.0
        med_c = float(np.median(confs)) if confs else 0.0
        print(f"  rot {rname:6s}: {tot_dets:4d} dets  "
              f"(media/frame {tot_dets / len(frames):5.1f})  "
              f"conf media {mean_c:.3f} mediana {med_c:.3f}  "
              f"clases {dict(sorted(per_class.items(), key=lambda kv: -kv[1])[:6])}")
    print()
