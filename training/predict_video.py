"""
Pasa un motor TensorRT del ejecutable VIB sobre un video y genera:
  - un MP4 anotado con las esferas detectadas
  - un JSON con las detecciones por frame
  - N imagenes PNG de muestra para revisar rapido

Replica el preprocesado/postprocesado de src/ai/InferenceEngine.cpp:
  - frame completo estirado (sin letterbox) a la entrada del engine (1920x1088)
  - RGB, /255, NCHW
  - salida end-to-end [N, 300, 6] = [x1, y1, x2, y2, conf, clase] en pixeles
  - filtro por confianza + NMS (IoU 0.7, misma clase) como ApplyNMS

Uso:
    python predict_video.py --video "C:\\ruta\\clip.mp4"
    python predict_video.py --video clip.mp4 --conf 0.3 --stride 2 --stills 6
"""

import argparse
import json
import time
from collections import Counter
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np
import tensorrt as trt
import torch

HERE = Path(__file__).resolve().parent
DEFAULT_MODELS = HERE.parent / "src" / "bin" / "Release" / "models"
DEFAULT_OUTDIR = HERE / "video_inferencia"

NMS_IOU = 0.7  # ApplyNMS del ejecutable


def load_class_names() -> list[str]:
    with open(HERE / "cvat_labels_pool.json", encoding="utf-8") as f:
        return [c["name"] for c in json.load(f)]


def class_color(cls: int) -> tuple[int, int, int]:
    """BGR: usa el color CVAT de la clase cuando esta disponible."""
    palette = [
        (255, 255, 255),  # bola_0 blanca
        (0, 255, 255),    # bola_1 amarilla
        (250, 183, 50),   # bola_2 azul
        (0, 0, 255),      # bola_3 roja
        (128, 0, 128),    # bola_4 morada
        (0, 165, 255),    # bola_5 naranja
        (0, 255, 0),      # bola_6 verde
        (0, 0, 128),      # bola_7 vinotinto
        (64, 64, 64),     # bola_8 negra
        (224, 255, 255),  # bola_9 amarillo_blanca
    ]
    return palette[cls % len(palette)]


class TrtEngine:
    def __init__(self, path: Path):
        self.path = path
        runtime = trt.Runtime(trt.Logger(trt.Logger.ERROR))
        self.engine = runtime.deserialize_cuda_engine(path.read_bytes())
        if self.engine is None:
            raise RuntimeError(f"No se pudo deserializar {path}")
        self.context = self.engine.create_execution_context()

        self.input_name = None
        self.output_name = None
        for i in range(self.engine.num_io_tensors):
            name = self.engine.get_tensor_name(i)
            if self.engine.get_tensor_mode(name) == trt.TensorIOMode.INPUT:
                self.input_name = name
            else:
                self.output_name = name

        shape = tuple(self.engine.get_tensor_shape(self.input_name))
        if shape[2] <= 0 or shape[3] <= 0:
            raise RuntimeError(f"Entrada dinamica en H/W no soportada: {shape}")
        self.in_h = int(shape[2])
        self.in_w = int(shape[3])

        batch = shape[0]
        self.batch = 1 if batch in (-1, 1) else int(batch)

    def infer_batch(self, frames_bgr: list[np.ndarray]) -> np.ndarray:
        n = len(frames_bgr)
        buf = np.empty((n, 3, self.in_h, self.in_w), dtype=np.float32)
        for i, frame in enumerate(frames_bgr):
            # cv2 lee BGR; el kernel CUDA hace resize + BGRA->RGB + /255
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            rgb = cv2.resize(rgb, (self.in_w, self.in_h), interpolation=cv2.INTER_LINEAR)
            buf[i] = rgb.transpose(2, 0, 1).astype(np.float32) / 255.0

        d_in = torch.from_numpy(buf).cuda()
        self.context.set_input_shape(self.input_name, (n, 3, self.in_h, self.in_w))
        out_shape = list(self.context.get_tensor_shape(self.output_name))
        out_shape[0] = n
        d_out = torch.empty(out_shape, dtype=torch.float32, device="cuda")

        self.context.set_tensor_address(self.input_name, d_in.data_ptr())
        self.context.set_tensor_address(self.output_name, d_out.data_ptr())

        stream = torch.cuda.current_stream().cuda_stream
        if not self.context.execute_async_v3(stream):
            raise RuntimeError("execute_async_v3 fallo")
        torch.cuda.synchronize()
        return d_out.cpu().numpy()


def iou(a, b) -> float:
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    inter = max(0.0, ix2 - ix1) * max(0.0, iy2 - iy1)
    union = (ax2 - ax1) * (ay2 - ay1) + (bx2 - bx1) * (by2 - by1) - inter
    return inter / union if union > 0 else 0.0


def apply_nms(dets: list[dict], iou_thr: float) -> list[dict]:
    """Igual que ApplyNMS: por confianza, suprime solo misma clase."""
    dets = sorted(dets, key=lambda d: d["conf"], reverse=True)
    keep: list[dict] = []
    for d in dets:
        if all(k["cls"] != d["cls"] or iou(d["bbox"], k["bbox"]) <= iou_thr for k in keep):
            keep.append(d)
    return keep


def postprocess_frame(raw: np.ndarray, in_wh: tuple[int, int],
                      orig_wh: tuple[int, int], conf_thr: float,
                      names: list[str]) -> tuple[list[dict], float]:
    in_w, in_h = in_wh
    out_w, out_h = orig_wh
    sx, sy = out_w / in_w, out_h / in_h

    dets: list[dict] = []
    max_conf = 0.0
    for row in raw:
        conf = float(row[4])
        max_conf = max(max_conf, conf)
        if conf < conf_thr:
            continue
        x1, y1, x2, y2 = (float(v) for v in row[:4])
        x1 = min(max(x1, 0.0), in_w)
        x2 = min(max(x2, 0.0), in_w)
        y1 = min(max(y1, 0.0), in_h)
        y2 = min(max(y2, 0.0), in_h)
        if x2 <= x1 or y2 <= y1:
            continue
        cls = int(round(float(row[5])))
        cls = min(max(cls, 0), len(names) - 1)
        dets.append({
            "cls": cls,
            "conf": round(conf, 4),
            "bbox": [x1 * sx, y1 * sy, x2 * sx, y2 * sy],
        })

    return apply_nms(dets, NMS_IOU), max_conf


def draw(frame: np.ndarray, dets: list[dict], names: list[str],
         scale: float, conf_thr: float) -> np.ndarray:
    for d in dets:
        x1, y1, x2, y2 = (int(round(v * scale)) for v in d["bbox"])
        color = class_color(d["cls"])
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
        label = f"{names[d['cls']]} {d['conf']:.2f}"
        (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)
        cv2.rectangle(frame, (x1, y1 - th - 6), (x1 + tw + 4, y1), color, -1)
        cv2.putText(frame, label, (x1 + 2, y1 - 4), cv2.FONT_HERSHEY_SIMPLEX,
                    0.5, (0, 0, 0), 1, cv2.LINE_AA)
    cv2.putText(frame, f"conf>={conf_thr}  n={len(dets)}", (12, 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2, cv2.LINE_AA)
    return frame


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--video", type=Path, required=True)
    ap.add_argument("--model", default="modelo_1920x1088.engine",
                    help="nombre del .engine dentro de src/bin/Release/models")
    ap.add_argument("--models", type=Path, default=DEFAULT_MODELS)
    ap.add_argument("--conf", type=float, default=0.6,
                    help="umbral de confianza (config.json usa 0.6)")
    ap.add_argument("--stride", type=int, default=1,
                    help="procesar 1 de cada N frames")
    ap.add_argument("--max-frames", type=int, default=0, help="0 = todo el video")
    ap.add_argument("--start", type=float, default=0.0, help="segundo inicial")
    ap.add_argument("--outdir", type=Path, default=DEFAULT_OUTDIR)
    ap.add_argument("--out-height", type=int, default=1080,
                    help="alto del video anotado (0 = resolucion original)")
    ap.add_argument("--stills", type=int, default=6,
                    help="cantidad de PNG de muestra (0 = ninguna)")
    args = ap.parse_args()

    names = load_class_names()
    model_path = args.models / args.model
    if not model_path.exists():
        raise SystemExit(f"No existe el modelo: {model_path}")
    if not args.video.exists():
        raise SystemExit(f"No existe el video: {args.video}")

    eng = TrtEngine(model_path)
    print(f"Modelo : {model_path.name}  entrada {eng.in_w}x{eng.in_h}  batch {eng.batch}")

    cap = cv2.VideoCapture(str(args.video))
    if not cap.isOpened():
        raise SystemExit(f"No se pudo abrir {args.video}")

    src_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    src_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"Video  : {args.video.name}  {src_w}x{src_h} @ {fps:.2f} fps  "
          f"({total} frames, {total / fps:.1f}s)")

    if args.start > 0:
        cap.set(cv2.CAP_PROP_POS_MSEC, args.start * 1000.0)

    args.outdir.mkdir(parents=True, exist_ok=True)
    stem = args.video.stem.replace(" ", "_")
    out_video = args.outdir / f"{stem}__{model_path.stem}__conf{args.conf:g}.mp4"
    out_json = args.outdir / f"{stem}__{model_path.stem}__conf{args.conf:g}.json"
    stills_dir = args.outdir / f"{stem}__stills"
    if args.stills > 0:
        stills_dir.mkdir(parents=True, exist_ok=True)

    out_h = args.out_height if args.out_height > 0 else src_h
    out_w = int(round(src_w * out_h / src_h / 2) * 2)
    draw_scale = out_w / src_w

    writer = cv2.VideoWriter(str(out_video), cv2.VideoWriter_fourcc(*"mp4v"),
                             fps / args.stride, (out_w, out_h))
    if not writer.isOpened():
        raise SystemExit(f"No se pudo crear {out_video}")

    # frames donde guardar PNG de muestra (dentro del tramo que se va a procesar)
    first_frame = int((args.start * fps)) if args.start > 0 else 0
    last_frame = total if total > 0 else 0
    if args.max_frames:
        last_frame = min(last_frame, first_frame + args.max_frames * args.stride)
    still_frames: set[int] = set()
    if args.stills > 0 and last_frame > first_frame:
        span = (last_frame - first_frame) // args.stride
        step = max(1, span // (args.stills + 1))
        still_frames = {first_frame + (step * (i + 1)) * args.stride
                        for i in range(args.stills)}

    records: list[dict] = []
    class_counts: Counter = Counter()
    frames_with_det = 0
    frames_processed = 0
    frame_idx = 0
    conf_hist = np.zeros(10, dtype=np.int64)  # mejor conf por frame -> histograma
    t0 = time.time()

    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if frame_idx % args.stride != 0:
            frame_idx += 1
            continue

        raw = eng.infer_batch([frame])[0]
        dets, max_conf = postprocess_frame(raw, (eng.in_w, eng.in_h),
                                          (src_w, src_h), args.conf, names)
        frames_processed += 1
        if dets:
            frames_with_det += 1

        bucket = min(9, max(0, int(max_conf * 10)))
        conf_hist[bucket] += 1

        for d in dets:
            class_counts[names[d["cls"]]] += 1

        records.append({
            "frame": frame_idx,
            "t": round(frame_idx / fps, 3),
            "max_conf": round(max_conf, 4),
            "detecciones": [
                {
                    "clase_id": d["cls"],
                    "clase": names[d["cls"]],
                    "confianza": d["conf"],
                    "bbox_xyxy": [round(v, 1) for v in d["bbox"]],
                }
                for d in dets
            ],
        })

        # ordenar de izquierda a derecha para que el dibujo sea legible
        dets.sort(key=lambda d: (d["bbox"][0] + d["bbox"][2]) / 2.0)
        vis = frame if args.out_height <= 0 else cv2.resize(
            frame, (out_w, out_h), interpolation=cv2.INTER_AREA)
        writer.write(draw(vis, dets, names, draw_scale, args.conf))

        if frame_idx in still_frames:
            still = args.outdir / f"{stem}__f{frame_idx:05d}.png"
            cv2.imwrite(str(still), vis)

        if frames_processed % 30 == 0:
            elapsed = time.time() - t0
            pct = 100.0 * frame_idx / total if total else 0
            print(f"  frame {frame_idx:5d}/{total}  {pct:5.1f}%  "
                  f"{frames_processed / elapsed:5.1f} fps proc  "
                  f"dets={len(dets)}", flush=True)

        frame_idx += 1
        if args.max_frames and frames_processed >= args.max_frames:
            break

    cap.release()
    writer.release()
    elapsed = time.time() - t0

    payload = {
        "generado": datetime.now().isoformat(timespec="seconds"),
        "video": str(args.video.resolve()),
        "modelo": model_path.name,
        "confianza_min": args.conf,
        "nms_iou": NMS_IOU,
        "stride": args.stride,
        "frames_procesados": frames_processed,
        "frames_con_deteccion": frames_with_det,
        "resumen_clases": dict(class_counts.most_common()),
        "histograma_max_conf_por_frame": {
            f"{i / 10:.1f}-{(i + 1) / 10:.1f}": int(conf_hist[i]) for i in range(10)
        },
        "frames": records,
    }
    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    print("\n--- Resumen ---")
    print(f"Frames procesados   : {frames_processed} en {elapsed:.1f}s "
          f"({frames_processed / elapsed:.1f} fps)")
    print(f"Frames con deteccion: {frames_with_det} "
          f"({100.0 * frames_with_det / max(1, frames_processed):.1f}%)")
    print("Detecciones por clase:", dict(class_counts.most_common()) or "ninguna")
    print("Max conf por frame (histograma):")
    for i in range(10):
        if conf_hist[i]:
            bar = "#" * int(40 * conf_hist[i] / max(1, conf_hist.max()))
            print(f"  {i / 10:.1f}-{(i + 1) / 10:.1f} {conf_hist[i]:5d} {bar}")
    print(f"\nVideo : {out_video}")
    print(f"JSON  : {out_json}")
    if args.stills:
        print(f"PNGs  : {stills_dir}")


if __name__ == "__main__":
    main()
