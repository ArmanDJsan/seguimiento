"""
Pasa los motores TensorRT del ejecutable VIB sobre las imagenes capturadas
(opcion 10) y genera un JSON con modelo / imagen / resultados.

Replica el preprocesado del ejecutable (InferenceEngine.cpp):
  - RGB, resize estirado a la entrada del engine (1920x1088), /255, NCHW
  - salida end-to-end [N, 300, 6] = [x1, y1, x2, y2, conf, clase] en pixeles
  - filtro por confianza + NMS (igual que ApplyNMS en el ejecutable)

Uso:
    python predict_exe_models.py
    python predict_exe_models.py --images <carpeta> --out <archivo.json>
"""

import argparse
import json
from datetime import datetime
from pathlib import Path

import numpy as np
import tensorrt as trt
import torch
from PIL import Image

HERE = Path(__file__).resolve().parent
DEFAULT_IMAGES = HERE / "cvat_images"
DEFAULT_MODELS = HERE.parent / "src" / "bin" / "Release" / "models"
DEFAULT_OUT = HERE / "predictions_exe_models.json"

# Modelos que usa el ejecutable (config.json: principal + motor de llegada)
MODEL_SPECS = [
    {"name": "modelo_1920x1088.engine", "conf": 0.6},
    {"name": "llegada_best.engine", "conf": 0.3},
]
NMS_IOU = 0.4


def load_class_names() -> list[str]:
    with open(HERE / "cvat_labels_pool.json", encoding="utf-8") as f:
        return [c["name"] for c in json.load(f)]


class TrtEngine:
    def __init__(self, path: Path):
        self.path = path
        data = path.read_bytes()
        runtime = trt.Runtime(trt.Logger(trt.Logger.ERROR))
        self.engine = runtime.deserialize_cuda_engine(data)
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

        stream = torch.cuda.current_stream().cuda_stream
        ok = self.context.execute_async_v3(stream)
        torch.cuda.synchronize()
        if not ok:
            raise RuntimeError("execute_async_v3 fallo")

        return d_out[0].cpu().numpy()


def iou(a: list[float], b: list[float]) -> float:
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    inter = max(0.0, ix2 - ix1) * max(0.0, iy2 - iy1)
    union = (ax2 - ax1) * (ay2 - ay1) + (bx2 - bx1) * (by2 - by1) - inter
    return inter / union if union > 0 else 0.0


def apply_nms(dets: list[dict], iou_thr: float) -> list[dict]:
    dets = sorted(dets, key=lambda d: d["confianza"], reverse=True)
    keep: list[dict] = []
    for d in dets:
        if all(iou(d["bbox"], k["bbox"]) <= iou_thr for k in keep):
            keep.append(d)
    return keep


def postprocess(raw: np.ndarray, engine_wh: tuple[int, int], orig_wh: tuple[int, int],
                conf_thr: float, names: list[str]) -> list[dict]:
    in_w, in_h = engine_wh
    out_w, out_h = orig_wh
    sx, sy = out_w / in_w, out_h / in_h

    dets: list[dict] = []
    for row in raw:
        conf = float(row[4])
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
            "clase_id": cls,
            "clase": names[cls],
            "confianza": round(conf, 4),
            "bbox": [x1 * sx, y1 * sy, x2 * sx, y2 * sy],
        })

    dets = apply_nms(dets, NMS_IOU)
    # Orden de izquierda a derecha por el centro del bbox
    dets.sort(key=lambda d: (d["bbox"][0] + d["bbox"][2]) / 2.0)

    results: list[dict] = []
    for i, d in enumerate(dets, 1):
        x1, y1, x2, y2 = d.pop("bbox")
        results.append({
            "orden": i,
            "clase_id": d["clase_id"],
            "clase": d["clase"],
            "confianza": d["confianza"],
            "bbox_xyxy": [round(x1, 1), round(y1, 1), round(x2, 1), round(y2, 1)],
            "centro_xy": [round((x1 + x2) / 2.0, 1), round((y1 + y2) / 2.0, 1)],
        })
    return results


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--images", type=Path, default=DEFAULT_IMAGES)
    ap.add_argument("--models", type=Path, default=DEFAULT_MODELS)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    names = load_class_names()
    images = sorted(args.images.glob("*.png"), key=lambda p: p.name)
    if not images:
        raise SystemExit(f"No hay PNG en {args.images}")

    engines = []
    for spec in MODEL_SPECS:
        path = args.models / spec["name"]
        if not path.exists():
            print(f"AVISO: no existe {path}, se omite")
            continue
        engines.append((TrtEngine(path), spec, path))

    results: list[dict] = []
    summary: dict[str, int] = {spec["name"]: 0 for _, spec, _ in engines}

    for eng, spec, path in engines:
        print(f"Modelo: {spec['name']}")
        for img_path in images:
            with Image.open(img_path) as img:
                raw = eng.infer(img)
                dets = postprocess(raw, (eng.in_w, eng.in_h), img.size,
                                   spec["conf"], names)
            summary[spec["name"]] += len(dets)
            results.append({
                "modelo": spec["name"],
                "imagen": img_path.name,
                "resultados": dets,
            })
            print(f"  {img_path.name}: {len(dets)} detecciones")

    payload = {
        "generado": datetime.now().isoformat(timespec="seconds"),
        "imagenes_dir": str(args.images.resolve()),
        "modelos": [
            {"modelo": spec["name"], "ruta": str(path.resolve()),
             "confianza_min": spec["conf"], "nms_iou": NMS_IOU}
            for _, spec, path in engines
        ],
        "resumen": summary,
        "resultados": results,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    print(f"\nJSON: {args.out}")
    print("Resumen:", summary)


if __name__ == "__main__":
    main()
