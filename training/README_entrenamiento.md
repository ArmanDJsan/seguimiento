# Entrenamiento — Modelo de "llegada"

Modelo YOLO26 dedicado para detectar las 10 esferas detenidas en la zona de llegada
(cámara fija) y determinar el orden de llegada. **No reemplaza** `best.engine` (seguimiento).

## Requisitos

- Python 3.11 (ya instalado).
- GPU NVIDIA RTX 5080 (Blackwell, sm_120) → **PyTorch CUDA 12.8+ obligatorio**.
- TensorRT ≥ 10.7 (ya instalado: 10.16).

## 1. Entorno (ya configurado en esta máquina)

```bash
# venv con acceso al TensorRT/ultralytics del sistema
python -m venv --system-site-packages training/.venv

# PyTorch compatible con Blackwell (RTX 5080)
training/.venv/Scripts/python.exe -m pip install --upgrade torch torchvision \
    --index-url https://download.pytorch.org/whl/cu128
training/.venv/Scripts/python.exe -m pip install onnx
```

Verificar que la GPU se ve:

```bash
training/.venv/Scripts/python.exe -c "import torch; print(torch.cuda.get_device_name(0), torch.cuda.get_device_capability(0))"
# Debe imprimir: NVIDIA GeForce RTX 5080 (12, 0)   (sin warning de sm_120)
```

## 2. Preparar el dataset

0. (Opcional) Convertir los frames PPM capturados con la opción 10 del menú
   (`src/bin/Release/training_frames/*.ppm`) a PNG y generar un ZIP para subir a CVAT:

   ```bash
   cd training
   ./.venv/Scripts/python.exe ppm_to_cvat.py --zip cvat_upload.zip
   ```

   Usa `--src <carpeta>` si los frames están en otra ruta y `--format jpg` si prefieres JPEG.

1. Descarga el dataset de CVAT en formato **YOLO 1.1** con imágenes → `training/llegada_dataset.zip`.
2. Ejecuta:

```bash
cd training
./.venv/Scripts/python.exe prepare_dataset.py --zip llegada_dataset.zip
```

Esto descomprime, valida las 10 clases (`bola_0`…`bola_9`) contra `cvat_labels_pool.json`,
y genera `llegada/{images,labels}/{train,val}`, `llegada/holdout/` y `llegada/data.yaml`.

## 3. Entrenar

```bash
cd training
./.venv/Scripts/python.exe train_llegada.py \
    --data llegada/data.yaml --base yolo26s.pt --epochs 150 --batch 4 --export
```

- `--batch`: sube a 8 si no da OOM (RTX 5080 16GB).
- `--base`: si `yolo26s` falla en esferas oscuras, prueba `yolo26m.pt`.
- `--export`: exporta `best.pt` → TensorRT `.engine` (y guarda el `.onnx` intermedio).

Resultados en `training/runs/detect/yolo26s_llegada_run_1/` (`weights/best.pt`, `results.csv`).

## 4. Validar

- Revisa `results.csv`: `metrics/mAP50(B)`, `mAP50-95`, y las curvas train vs val (evitar overfitting).
- Inferencia visual sobre `llegada/holdout/`:

```bash
./.venv/Scripts/python.exe -c "
from ultralytics import YOLO
m = YOLO('runs/detect/yolo26s_llegada_run_1/weights/best.pt')
m.predict(source='llegada/holdout/images', imgsz=[1088,1920], conf=0.5, save=True)
"
```

## 5. Desplegar en VIB (Fase 4 — aparte)

- Copia el `.engine` a `src/bin/Release/models/llegada_best.engine`.
- ⚠️ **El `.engine` de TensorRT es específico de la GPU.** Si el binario de producción corre en una
  GPU distinta a la RTX 5080, el engine NO cargará allí: conserva el `.onnx` y reconstruye el engine
  en la máquina de producción con `trtexec` (o `model.export(format="engine")` en esa máquina).
- Añade un slot en `src/config.json` para el motor de llegada (sin tocar `best.engine`) y cablea un
  segundo `InferenceEngine` a `SphereVerifier::ExecuteArrivalOrder`.
