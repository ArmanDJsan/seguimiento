# Reporte: diferencias entre `seguimientoYolo7` y `seguimientoYolo7LMDS`

**Fecha del reporte:** 2026-09-23
**Comparación:** `C:\vs\proyectos\seguimientoYolo7` (referencia) vs `C:\vs\proyectos\seguimientoYolo7LMDS` (trabajo en curso)

---

## Lo esencial primero

Los dos directorios **son el mismo repositorio git**, misma rama (`compilando_ndi`) y mismo HEAD
(`fd84f01`, el merge del PR #84). La diferencia no es de commits:

- **`seguimientoYolo7`** → árbol de trabajo **limpio** en `fd84f01`. Es la referencia intacta.
- **`seguimientoYolo7LMDS`** → ese mismo commit **+ ~2 semanas de trabajo sin commitear**
  (fechas 10–14 de agosto). Nada de eso existe en `seguimientoYolo7`.

`git diff --stat` en LMDS: **1.626 inserciones / 167 borrados** en 10 archivos versionados,
más 4 archivos fuente nuevos, un script de coreografía, y todo un pipeline de entrenamiento sin trackear.

---

## Qué cambió, por tema

### 1. Menú: de 7 a 11 opciones

`src/ui/UserMenu.h` / `.cpp` — el prompt pasa a `(1-11)` y se agregan cuatro modos:

| Opción | Modo | Qué hace |
|--------|------|----------|
| **8** | `AUTOMATED_TRACK` | Modo Pista Automatizada (1 cámara PTZ, verifica esferas) |
| **9** | `LIVE_PREVIEW` | Detección YOLO en vivo → NDI `VIB_Pista_Raw` / `VIB_Pista_Boxes` |
| **10** | `TRAINING_CAPTURE` | Video NDI `VIB_Training`; ESPACIO guarda frame para etiquetar |
| **11** | `ARRIVAL_TEST` | Orden de llegada en vivo con motor dedicado → `VIB_Llegada_Raw` / `_Boxes` |

### 2. `src/core/main.cpp` — +1.014 líneas

Cuatro funciones nuevas: `RunAutomatedTrackMode()`, `RunLivePreview()`, `RunTrainingCapture()`,
`RunArrivalOrderTest()`, más helpers de dibujo (fuente bitmap 3×5 hecha a mano para pintar dígitos
en los frames) y mapeo color↔ballID.

> **Detalle a tener en cuenta:** `src/core/main.cpp:3031` tiene `kArrivalLeftToRight = false` —
> el sentido izquierda→derecha de la llegada es un **flag que hay que verificar contra la pista física**.

### 3. Módulos nuevos (los dos que no existían en Yolo7)

**`src/verification/ArrivalOrderStabilizer.{h,cpp}`** — confirma el orden de llegada cuando la misma
secuencia de ballIDs se repite `stableFrames` (10) veces seguidas con al menos `minSpheres`
(**8**, no 10 — coherente con que en la llegada pueden quedar 8-10 esferas). Distingue motivos de
descarte (pocas esferas / ID duplicado) y tiene el invariante `HasDuplicateId()`: un ballID por color,
así que un swap o doble detección se detecta independientemente de cuántas bolas lleguen.

**`src/output/ArrivalEmitter.{h,cpp}`** — emite `{"order":[...]}`. **Es un stub**: hoy solo loguea el
payload. El punto a reemplazar por un HTTP POST real es `Send()` (`ArrivalEmitter.h:46`).

Ambos están registrados en `src/VIB.vcxproj`.

### 4. SphereVerifier: captura de frames reales

Este era el punto crítico que el propio plan marcaba como bloqueante (`CaptureAndDetect()` tenía un
TODO y no capturaba frames reales fuera de stub mode). Ahora tiene:

- `SetRealCaptureAvailable()`, `PushFrame()` y `WaitForFrame()` con mutex + `condition_variable` y
  timeout — o sea, **frames reales de DeckLink inyectados desde el hilo de captura**, con stream
  CUDA propio.
- `SetDetectionCallback()` para la vista previa en NDI.
- `SaveResultImage()`: dibuja cajas numeradas sobre el último frame, más un rectángulo ROI
  **hardcodeado** (`ROI_X1=552, ROI_Y1=324, ROI_X2=626, ROI_Y2=741`) y lo escribe directo sobre un `.ppm`.

### 5. Segundo motor de inferencia ("llegada")

- `src/core/Config.h` → `llegadaInferenceConfig`, separado de `inferenceConfig` (no toca
  `best.engine`): 1920×1088, `confidence 0.3` (bajado a propósito para no perder la bola oscura
  `bola_8`), NMS 0.4, 10 clases, `skipResize=false`.
- `src/config.json` → secciones `inference_engine_llegada` y `arrival_order`.
- **`src/ai/InferenceEngine.cpp`** → el NMS pasó de `m_config.nmsThreshold` (0.4) a **0.7
  hardcodeado**, y se eliminó todo el logging de debug de NMS (`[NMS_DEBUG]`, `[NMS_SUMMARY]`).

### 6. Pipeline de entrenamiento (nuevo, sin trackear)

`training/` — plan, dataset y scripts para el **modelo dedicado de llegada**: `prepare_dataset.py`,
`train_llegada.py`, `export_llegada.py`, `validate_llegada.py`, `confusion_holdout.py`, base
`yolo26s.pt`, dataset de 208 fotos etiquetadas en CVAT. Hay runs de entrenamiento (`smoke_test`,
`yolo26s_llegada_run_1`) y el motor exportado existe:
`src/bin/Release/models/llegada_best.engine` (25 MB, 14-ago). El `.gitignore` se amplió para excluir
`training/.venv/`, `*.pt`, `*.engine`, `*.onnx`.

### 7. Coreografía de la pista — incompleta

`config/choreography/pista_automatizada.json` existe pero está en **v0.1** y solo implementa 2 de las
7 fases del plan: PTZ a preset 2 (closeup salida) + `SpherePresenceCheck` (10 esferas, 15 s). Falta
todo lo demás: largada, `SphereArrivalWait`, captura de posiciones, cierre con ESP32, `StopRecording`.

### 8. Estado de compilación

`seguimientoYolo7LMDS/src/bin/Release/VIB.exe` es del **14-ago 13:11**; el de `seguimientoYolo7` es
del **30-abr**. Los `.obj` de los kernels CUDA también difieren, así que LMDS es el build más
reciente de los dos.

---

## Resumen de brechas abiertas

Tres cosas que el plan prometía y **no están cerradas**:

1. **La coreografía de opción 8 está a un 25%** (2 de 7 fases) — la opción 8 se puede lanzar, pero
   no corre una carrera completa.
2. **`ArrivalEmitter` es un stub** — no hay endpoint HTTP; el orden confirmado solo se loguea.
3. **El umbral NMS quedó en 0.7 hardcodeado** en `InferenceEngine.cpp`, ignorando el `nms_threshold`
   del config. Afecta a *todos* los modos, incluido el tracking de 4 cámaras de carrera, no solo a la
   llegada — vale la pena confirmar que fue intencional y no un ajuste de prueba que quedó puesto.

---

## Archivos afectados (resumen)

**Modificados (versionados):**

| Archivo | Cambio |
|---------|--------|
| `.gitignore` | +10 líneas (training, `*.pt`, `*.engine`, `*.onnx`) |
| `src/VIB.vcxproj` | +4 líneas (registra los 2 módulos nuevos) |
| `src/ai/InferenceEngine.cpp` | NMS a 0.7 fijo, se quita logging de debug |
| `src/config.json` | +22 líneas (`inference_engine_llegada`, `arrival_order`) |
| `src/core/Config.h` | +31 líneas (`llegadaInferenceConfig`, configs de llegada) |
| `src/core/main.cpp` | +1.017 líneas (opciones 8-11) |
| `src/ui/UserMenu.cpp` | +29 líneas (display y routing 8-11) |
| `src/ui/UserMenu.h` | +4 líneas (enum) |
| `src/verification/SphereVerifier.cpp` | +565 líneas (captura real, SaveResultImage) |
| `src/verification/SphereVerifier.h` | +59 líneas (nuevas APIs) |

**Nuevos (sin trackear):**

- `src/output/ArrivalEmitter.{h,cpp}`
- `src/verification/ArrivalOrderStabilizer.{h,cpp}`
- `config/choreography/pista_automatizada.json`
- `plan_opcion8_pista_automatizada.md`
- `training/` (pipeline completo + dataset + motor exportado)
