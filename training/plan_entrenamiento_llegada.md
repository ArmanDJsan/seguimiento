# Plan de entrenamiento — Modelo de "llegada" (esferas detenidas, cámara fija)

> Fecha: 2026-08-13 · Estado: aprobado, en ejecución

## Contexto

El sistema VIB (`seguimientoYolo7LMDS`) usa un modelo YOLO26 (`best.engine`, TensorRT FP16) para el
tracking de 4 cámaras en carrera. Se capturaron **208 fotos de la zona de llegada con cámara fija**
(vía la opción 10 del menú, `RunTrainingCapture()` → `src/bin/Release/training_frames/*.ppm`, 1920×1080)
y se etiquetaron en **CVAT** con **10 clases** (`bola_0`…`bola_9`, una por color).

Objetivo: entrenar un **segundo modelo dedicado** (no reemplaza al principal) que detecte las 10 esferas
detenidas en la llegada para **determinar el orden de llegada**. Forma parte de una arquitectura multi-modelo
(3 modelos: seguimiento + llegada + …). El entrenamiento corre **localmente** en esta máquina
(RTX 5080 16GB, 128GB RAM, Threadripper 16c/32t).

Decisiones confirmadas:
- **10 clases** (`bola_0`…`bola_9`) — se necesita identidad por color para el orden de llegada.
- **Base `yolo26s.pt`** (fallback: `yolo26m.pt` si `s` falla en esferas oscuras/pequeñas).
- **Segundo motor dedicado** — no toca `best.engine`.
- **Resolución nativa 1088×1920** (coincide con el motor).
- **Dataset local** — `llegada_dataset.zip` (CVAT, YOLO 1.1 con imágenes), sin Roboflow.

## Contrato exacto que debe cumplir el modelo exportado

Lo que `InferenceEngine::PostProcess` (`src/ai/InferenceEngine.cpp:799-882`) espera del `.engine`:

- **Entrada**: tensor `images`, FP32, **RGB NCHW**, forma `[batch, 3, 1088, 1920]`, valores `/255` (sin mean/std).
- **Salida**: `[batch, 300, 6]`, cada fila `[x1, y1, x2, y2, objectness, class_id]` en **píxeles absolutos**, `class_id` float 0–9.

YOLO26 de Ultralytics es **nativamente end-to-end (sin NMS)** y exporta exactamente esta salida
`(N, 300, 6)` con `max_det=300` por defecto. → El export debe ser `end2end` (default de yolo26), `half=True`, `dynamic=True`.

## Fases

### Fase 0 — Preparar los datos (CVAT → Ultralytics)

1. Descomprimir `llegada_dataset.zip` (layout CVAT YOLO 1.1: `obj.names`, `obj.data`, `train.txt`, `obj_train_data/`).
2. Validar `obj.names` = 10 clases en el orden de `cvat_labels_pool.json` (`bola_0`…`bola_9`).
3. Convertir a layout Ultralytics (`images/{train,val}` + `labels/{train,val}` + `data.yaml`) con split 80/20 estratificado, semilla fija.
4. Guardar un hold-out de ~20 imágenes fuera de train/val para validación final.

> Automatizado en `prepare_dataset.py`.

### Fase 1 — Entrenamiento (local, RTX 5080)

`train_llegada.py` (autocontenido, sin el paquete `src/`):

| Parámetro | Valor |
|---|---|
| base | `yolo26s.pt` (fallback `yolo26m.pt`) |
| imgsz | `[1088, 1920]` (h,w) — **no cambiar** |
| rect | `True` |
| epochs | 150 (patience 50 corta antes) |
| batch | 4 (subir a 8 si la VRAM de 16GB lo permite) |
| mosaic / mixup | `0` / `0.0` (escena fija) |
| optimizer | AdamW, `lr0=0.01`, `lrf=0.0001`, cos_lr |
| augmentación | fliplr 0.5, hsv_s/v 0.4 |
| label_smoothing / warmup | 0.1 / 5.0 |
| close_mosaic / cache / workers | 20 / True / 12 |

### Fase 2 — Exportar a TensorRT `.engine`

```python
# ⚠️ dynamic=False (batch 1 estático, H/W fijos). Con dynamic=True ultralytics
#    deja H/W dinámicos (-1,3,-1,-1) y el InferenceEngine adapta inputWidth/Height
#    a -1 (InferenceEngine.cpp:566-574) -> falla la asignación de buffers.
model.export(format="engine", imgsz=[1088, 1920], half=True, dynamic=False, max_det=300)
# ONNX intermedio portable (para reconstruir el engine en otra GPU):
model.export(format="onnx", imgsz=[1088, 1920], half=False, dynamic=True, max_det=300)
# Reconstrucción en la máquina de producción (batch dinámico 1-4, H/W fijos):
# trtexec --onnx=best.onnx --saveEngine=llegada_best.engine --fp16 \
#   --minShapes=images:1x3x1088x1920 --optShapes=images:4x3x1088x1920 --maxShapes=images:4x3x1088x1920
```

Verificar: entrada `[1,3,1088,1920]` FP32 RGB /255; salida `[1,300,6]` con `cls` en índice 5.

### Fase 3 — Validar

- `mAP50` / `mAP50-95` y precisión/recall **por clase** (atención a `bola_8` negra, `bola_7` vinotinto).
- Inferencia visual sobre el hold-out (~20 imágenes) con boxes + clase + conf.
- Si `s` falla en esferas oscuras → repetir con `yolo26m.pt`.

### Fase 4 — Desplegar en VIB (segundo motor dedicado)

- Copiar `.engine` a `src/bin/Release/models/llegada_best.engine`.
- Añadir slot de config en `src/config.json` (análogo a `model_path`, línea 125) sin tocar `best.engine`.
- Segundo `InferenceEngine` cableado a `SphereVerifier::ExecuteArrivalOrder` (stub en `src/verification/SphereVerifier.cpp:366-380`).

## Archivos

| Archivo | Acción |
|---|---|
| `training/prepare_dataset.py` | **Nuevo** — CVAT zip → layout Ultralytics + split |
| `training/train_llegada.py` | **Nuevo** — entrenamiento + export autocontenido |
| `training/llegada_data.yaml` | **Generado** por prepare_dataset.py |
| `training/requirements.txt` | **Nuevo** — dependencias del entorno local |
| `training/README_entrenamiento.md` | **Nuevo** — instrucciones de esta máquina |
| `src/config.json` / `src/verification/SphereVerifier.cpp` | **Modificar** en Fase 4 |

## Verificación end-to-end

1. `data.yaml` con 10 clases en orden correcto; 208 parejas imagen+label.
2. `best.pt` generado localmente; `results.csv` con mAP50 > umbral y sin overfitting (train vs val).
3. Export confirma entrada `[1,3,1088,1920]` y salida `[1,300,6]`.
4. Inferencia sobre 3–5 fotos del hold-out detecta cada esfera con su clase/bbox.
5. (Fase 4) El motor de llegada carga sin tocar `best.engine`.
