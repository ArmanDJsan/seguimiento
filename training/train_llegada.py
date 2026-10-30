"""
Entrena el modelo de "llegada" (esferas detenidas, cámara fija) y opcionalmente
lo exporta a TensorRT (.engine).

Autocontenido: no depende del paquete src/ (que solo existe en el VPS).
Basado en la receta ya validada de train_model.py, ajustada para este caso:
  - base yolo26s.pt (menos overfitting con ~208 imágenes)
  - imgsz [1088, 1920] -> coincide con la entrada del motor TensorRT de VIB
  - rect=True, mosaic=0 (escena fija), AdamW

Uso:
    python train_llegada.py --data llegada/data.yaml --base yolo26s.pt --epochs 150 --batch 4
    python train_llegada.py --data llegada/data.yaml --export   # entrena Y exporta a .engine
"""

import argparse
from pathlib import Path

from ultralytics import YOLO

HERE = Path(__file__).resolve().parent


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", type=Path, default=HERE / "llegada" / "data.yaml")
    ap.add_argument("--base", default="yolo26s.pt",
                    help="Modelo base (yolo26s.pt / yolo26m.pt). El .pt se descarga solo si no está.")
    ap.add_argument("--epochs", type=int, default=150)
    ap.add_argument("--imgsz", type=int, nargs="+", default=[1088, 1920],
                    help="Alto y ancho. NO cambiar: [1088, 1920] es el contrato del motor.")
    ap.add_argument("--batch", type=int, default=4,
                    help="Ajusta según VRAM (RTX 5080 16GB: 4-8).")
    ap.add_argument("--device", default="0")
    ap.add_argument("--name", default="yolo26s_llegada_run_1")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--export", action="store_true",
                    help="Exportar best.pt a TensorRT .engine al terminar.")
    return ap.parse_args()


def main():
    args = parse_args()
    imgsz = args.imgsz if len(args.imgsz) == 2 else args.imgsz[0]

    model = YOLO(args.base)

    train_params = {
        "data": str(args.data),
        "epochs": args.epochs,
        "imgsz": imgsz,
        "batch": args.batch,
        "device": args.device,
        "patience": 50,
        "optimizer": "AdamW",
        "lr0": 0.01,
        "lrf": 0.0001,
        "cos_lr": True,
        "rect": True,              # rectangular: conserva 16:9
        "mosaic": 0,               # desactivado (escena fija)
        "mixup": 0.0,
        "fliplr": 0.5,
        "hsv_s": 0.4,
        "hsv_v": 0.4,
        "label_smoothing": 0.1,
        "warmup_epochs": 5.0,
        "close_mosaic": 20,
        "cache": True,             # 208 img @1088x1920 ~1.3GB en RAM
        "workers": args.workers,
        "amp": True,
        "name": args.name,
        "project": str(HERE / "runs" / "detect"),
    }

    print(f"[train] base={args.base} imgsz={imgsz} batch={args.batch} epochs={args.epochs}")
    model.train(**train_params)

    best_pt = HERE / "runs" / "detect" / args.name / "weights" / "best.pt"
    print(f"[train] best.pt -> {best_pt}")

    if args.export:
        # Exporta a TensorRT (y guarda también el .onnx intermedio).
        # Entrada: images [b,3,1088,1920] FP32 RGB /255.
        # Salida : [b,300,6] = [x1,y1,x2,y2,obj,class_id]  (end-to-end, sin NMS).
        # dynamic=False: batch 1 estático y H/W fijos. Con dynamic=True
        #   ultralytics deja H/W dinámicos y el InferenceEngine los adapta a
        #   -1 (falla). Para batch dinámico, reconstruir con trtexec.
        print("[export] exportando a TensorRT...")
        model.export(format="engine", imgsz=imgsz, half=True, dynamic=False, max_det=300)
        print("[export] listo. Revisa runs/detect/<name>/weights/*.engine y *.onnx")


if __name__ == "__main__":
    main()
