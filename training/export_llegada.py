"""
Exporta el modelo de llegada entrenado a TensorRT .engine (y ONNX intermedio).

Contrato (verificado contra InferenceEngine::PostProcess):
  Entrada : [batch, 3, 1088, 1920] FP32 RGB /255
  Salida  : [batch, 300, 6] = [x1, y1, x2, y2, obj, class_id]  (end-to-end, sin NMS)
            -> el InferenceEngine auto-detecta stride=6 y lee det[5] como class_id.

Uso:
    python export_llegada.py                          # usa best.pt del run por defecto
    python export_llegada.py --weights <best.pt>      # exporta otro checkpoint
"""

import argparse
from pathlib import Path

from ultralytics import YOLO

HERE = Path(__file__).resolve().parent
DEFAULT_BEST = HERE / "runs" / "detect" / "yolo26s_llegada_run_1" / "weights" / "best.pt"


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", type=Path, default=DEFAULT_BEST)
    ap.add_argument("--imgsz", type=int, nargs="+", default=[1088, 1920],
                    help="Alto y ancho. [1088, 1920] = contrato del motor.")
    ap.add_argument("--no-onnx", action="store_true",
                    help="Saltar el ONNX intermedio (solo .engine).")
    return ap.parse_args()


def main():
    args = parse_args()
    best_pt = args.weights
    if not best_pt.exists():
        raise SystemExit(f"No existe '{best_pt}'. Entrena primero con train_llegada.py.")

    imgsz = args.imgsz if len(args.imgsz) == 2 else args.imgsz[0]
    print(f"[export] weights={best_pt} imgsz={imgsz}")

    if not args.no_onnx:
        # ONNX intermedio FP32 (portable): sirve para reconstruir el .engine
        # en otra GPU con trtexec --fp16 si el engine no carga allí.
        m = YOLO(str(best_pt))
        onnx_path = m.export(format="onnx", imgsz=imgsz, half=False,
                             dynamic=True, max_det=300)
        print(f"[export] onnx -> {onnx_path}")

    # TensorRT .engine FP16 (específico de esta GPU: RTX 5080).
    # IMPORTANTE: dynamic=False (batch 1 estático, H/W fijos [1088,1920]).
    #   Con dynamic=True ultralytics deja H/W dinámicos (-1,3,-1,-1), y el
    #   InferenceEngine adapta inputWidth/Height a -1 -> falla la asignación
    #   de buffers (InferenceEngine.cpp:566-574). Para una cámara fija de
    #   llegada batch 1 es suficiente; si se necesitara batch dinámico, se
    #   reconstruye con trtexec a partir del .onnx usando --min/--opt/--max.
    m = YOLO(str(best_pt))
    engine_path = m.export(format="engine", imgsz=imgsz, half=True,
                           dynamic=False, max_det=300)
    print(f"[export] engine -> {engine_path}")
    print("[export] listo.")


if __name__ == "__main__":
    main()
