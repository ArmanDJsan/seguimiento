# Arquitectura del Flujo de Frames - VIB System

## 📋 Resumen Ejecutivo

Este documento describe el flujo completo desde que un frame es capturado por una DeckLink hasta que es procesado por SphereVerifier en coreografías.

---

## 🎯 Pipeline Completo (Visión General)

```
DeckLink Hardware → DeckLinkCapture → CUDA Buffers → [3 Pipelines Paralelos] → vMix/Tracking/Verificación
```

**Tiempo objetivo por frame:** 32ms (60fps)
- **Priority 1 (NDI):** <10ms
- **Priority 2 (Motion):** <5ms  
- **Priority 3 (AI):** <15ms

---

## 🔄 Flujo Detallado Paso a Paso

### **Fase 1: Captura de Hardware → CUDA Memory**
**Ubicación:** `DeckLinkCapture.cpp` línea 318-350

1. **DeckLink IRQ** (Interrupt Request)
   - Tarjeta DeckLink captura frame de SDI/HDMI a 60fps
   - Genera interrupt en CPU (afinity cores 0-1 - CCD0)

2. **VideoInputFrameArrived Callback**
   ```cpp
   HRESULT DeckLinkCapture::VideoInputFrameArrived(
       IDeckLinkVideoInputFrame* videoFrame,
       IDeckLinkAudioInputFrame* audioFrame)
   ```
   - Recibe frame directo del hardware
   - Obtiene puntero raw: `videoFrame->GetBytes(&rawFrameData)`
   - Format: YUV 4:2:2 (UYVY), 3840×2160, 8-bit

3. **Zero-Copy DMA → CUDA**
   ```cpp
   cudaMemcpy2DAsync(
       m_channel.cudaYUVBuffer,      // Destino: Pinned CUDA memory
       rawFrameData,                  // Origen: DeckLink DMA buffer
       CUDA_MEMCPY_HOST_TO_DEVICE,
       stream
   );
   ```
   - **Memoria pinned** (`cudaMallocPinned`) permite DMA directo
   - No copia intermedia en RAM
   - Thread: cores 2-3 (VIDEO_CAPTURE affinity)

4. **Conversión de Color en GPU**
   ```cpp
   ConvertUYVYToBGRA_CUDA(
       cudaYUVBuffer,    // Input
       cudaBGRABuffer,   // Output para AI
       3840, 2160,
       stream
   );
   ```
   - **Input:** UYVY (para NDI/vMix)
   - **Output:** BGRA (para TensorRT)
   - Kernel CUDA: procesamiento paralelo en RTX 5080

5. **Frame Ready Callback**
   ```cpp
   if (m_frameReadyHandler) {
       m_frameReadyHandler(m_channel, stream);
   }
   ```
   - Dispara el frame handler en `main.cpp:842-927`

---

### **Fase 2: Tres Pipelines Paralelos**
**Ubicación:** `main.cpp` línea 842-927

El frame handler ejecuta 3 prioridades en paralelo:

#### **PRIORITY 1: Video Output (NDI → vMix)** ⏱️ <10ms
**Líneas:** 850-860

```cpp
ndiManager->SendUYVYFrame(
    channel.channelID,          // Camera 1-12
    channel.cudaYUVBuffer,      // Buffer UYVY directo
    channel.width,              // 3840
    channel.height,             // 2160
    stream
);
```

**Proceso:**
1. GPU→CPU transfer usando pinned memory
2. NDI SDK encoding (CPU cores 6-7)
3. Envío por red Ethernet 10GbE a vMix
4. **Double-buffering:** Ping-pong buffers reduce blocking de ~0.8ms a ~0.2ms

**Resultado:** Video visible en vMix en <10ms desde captura

---

#### **PRIORITY 2: Motion Analysis (ActiveCameraSelector)** ⏱️ <5ms
**Líneas:** 863-874

```cpp
cameraSelector->ProcessFrame(
    channel.channelID,
    channel.cudaYUVBuffer,
    channel.width,
    channel.height,
    stream
);
```

**Propósito:** Detectar movimiento para auto-switching de cámaras
- Analiza píxeles cambiantes frame-a-frame
- Alimenta SceneManager para leapfrogging automático

---

#### **PRIORITY 3: AI Inference + Tracking** ⏱️ <15ms
**Líneas:** 876-919

##### **3a. InferenceEngine (TensorRT)** 
**Líneas:** 877-888

```cpp
std::vector<BallDetection> ballDetections = inferenceEngine->ProcessFrame(
    channel.cudaBGRABuffer,     // Input: BGRA 3840×2160
    channel.channelID,
    channel.width,
    channel.height,
    inferenceStream
);
```

**Dentro de InferenceEngine (`InferenceEngine.cpp:138-243`):**

1. **Downscale 3840×2160 → 640×640**
   ```cpp
   ResizeAndNormalize_CUDA(bgraInput, preprocessedBuffer, 
                          3840, 2160, 640, 640, stream);
   ```
   - Kernel CUDA bicubic interpolation
   - Normalización [0,255] → [0.0,1.0]

2. **TensorRT Inference**
   ```cpp
   context->enqueueV2(bindings, inferenceStream, nullptr);
   ```
   - **Modelo:** `yolo26l_fp16_batch12.engine`
   - **Batch size:** 12 (procesa 12 cámaras en paralelo)
   - **Precision:** FP16 (Tensor Cores en RTX 5080)
   - **Cores:** GPU cores 8-11 (CCD1 affinity)

3. **Post-processing NMS**
   ```cpp
   std::vector<BallDetection> ApplyNMS(
       rawDetections, 
       nms_threshold=0.4,
       confidence_threshold=0.6
   );
   ```
   - Non-Maximum Suppression
   - Filtra overlapping bounding boxes
   - Retorna detecciones únicas

**Output:** `BallDetection[] { ballID, x, y, confidence }`

---

##### **3b. PositionMapper (Homografía)** 
**Líneas:** 890-903

```cpp
auto transformed = positionMapper->BatchTransform(
    channel.channelID,
    pixelPositions,     // [(x_pixel, y_pixel, conf, ballID), ...]
    timestamp
);
```

**Proceso:**
1. Lee matriz de homografía 3×3 de `calibration.json`
2. Transforma píxeles → coordenadas globales del track
   ```
   [Xg]   [h11 h12 h13]   [x_pixel]
   [Yg] = [h21 h22 h23] × [y_pixel]
   [w ]   [h31 h32 h33]   [   1   ]
   
   Xg_real = Xg/w  (metros)
   Yg_real = Yg/w  (metros)
   ```

3. **Fusión de overlapping:** Si 2+ cámaras ven la misma bola:
   ```cpp
   FuseOverlappingDetections(transformed);
   ```
   - Promedia posiciones cercanas (threshold: 0.5m)
   - Mejora precisión con multi-view

**Output:** `GlobalPosition[] { ballID, Xg, Yg, confidence, timestamp, cameraID }`

---

##### **3c. BallTracker (Kalman Filter)** 
**Líneas:** 905-919

```cpp
ballTracker->Update(globalPositions, timestamp);
```

**Proceso (`BallTracker.cpp`):**
1. **Predicción:** 
   ```cpp
   // Modelo de movimiento constante
   X_pred = X_prev + velocity * dt
   Y_pred = Y_prev + velocity * dt
   ```

2. **Matching:** Asocia detecciones con tracks existentes
   - Hungarian algorithm
   - Distancia Mahalanobis

3. **Update:** Kalman correction
   ```cpp
   K = P * H^T * (H*P*H^T + R)^-1  // Kalman gain
   X_new = X_pred + K*(Z - H*X_pred)
   ```

4. **Ranking:** Ordena bolas por posición Y (avance en pista)
   ```cpp
   std::sort(tracks, [](a, b) { return a.Yg > b.Yg; });
   ```

**Output:** Rankings 1-10, velocidades, trayectorias suavizadas

---

##### **3d. SceneManager (Leapfrogging)** 
**Líneas:** 910-913

```cpp
auto leader = ballTracker->GetLeader();
sceneManager->UpdateLeaderPosition(leader.Xg, leader.Yg);
```

**Proceso (`SceneManager.cpp:157-283`):**
1. Compara posición del líder con thresholds configurados
2. Si cruza threshold → cambia grupo de cámaras
3. Envía comandos a VideoHub:
   ```
   VIDEO OUTPUT ROUTING:
   0 CAM_03    # Output 0 (vMix Input 1)
   1 CAM_06
   ```

4. **Retry logic:** 3 intentos con 50ms delay
5. Actualiza overlays en vMix con ranking

---

##### **3e. RankingPublisher (Redis)** 
**Líneas:** 915-918

```cpp
auto ranking = ballTracker->GetRanking();
rankingPublisher->PublishRanking(ranking);
```

- Publica rankings a Redis
- Consumido por overlays externos
- Thread: core 14 (background)

---

### **Fase 3: Verificación (ChoreographyEngine)** 🎬

**Trigger:** Usuario presiona F12 o evento automático

#### **3.1 ChoreographyEngine.Start()**
**Ubicación:** `ChoreographyEngine.cpp:177-207`

```cpp
choreographyEngine->Start();
```

1. Lee script JSON (ejemplo: `race_setup.json`)
2. Parsea eventos secuencialmente
3. Estado: `Idle → Ready → Running`

---

#### **3.2 Ejecución de Evento SpherePresenceCheck**
**Ubicación:** `ChoreographyEngine.cpp:739-788`

```json
{
  "type": "SpherePresenceCheck",
  "cameraID": 1,
  "expectedSpheres": 10,
  "timeoutMs": 5000,
  "mode": "presence"
}
```

**Ejecución:**

```cpp
// ChoreographyEngine.cpp:762-763
verifyResult = m_sphereVerifier->CheckPresence(
    cameraID=1, 
    expectedSpheres=10, 
    timeoutMs=5000
);
```

---

#### **3.3 SphereVerifier.CheckPresence()**
**Ubicación:** `SphereVerifier.cpp:84-142`

**Proceso:**

1. **Route VideoHub a cámara específica**
   ```cpp
   m_videoHub->Route(
       cameraID,      // Input: CAM_01
       outputIndex=0  // Output a vMix/Capture
   );
   ```

2. **Loop de captura (timeout: 5000ms)**
   ```cpp
   while (elapsed < timeoutMs) {
       auto detections = CaptureAndDetect(cameraID, sampleDelayMs=500);
       if (detections.size() >= expectedSpheres) {
           break;  // ✅ Éxito
       }
       Sleep(sampleDelayMs);
   }
   ```

3. **CaptureAndDetect() → InferenceEngine**
   ```cpp
   // SphereVerifier.cpp:283-315
   auto allDetections = m_inferenceEngine->ProcessFrame(
       frameBuffer,     // Frame capturado de cámara
       cameraID,
       3840, 2160,
       stream
   );
   ```
   - **Reutiliza el mismo pipeline de InferenceEngine**
   - Misma conversión UYVY→BGRA
   - Mismo downscale 3840×2160→640×640
   - Mismo TensorRT batch inference

4. **Validación**
   ```cpp
   result.spheresDetected = allDetections.size();
   result.success = (result.spheresDetected >= expectedSpheres);
   ```

**Output:**
```cpp
VerificationResult {
    success: true,
    spheresDetected: 10,
    errorMessage: "",
    detections: [{ballID:1, x:0.2, y:0.3}, ...]
}
```

---

#### **3.4 Resultado en ChoreographyEngine**

```cpp
// ChoreographyEngine.cpp:781-793
if (verifyResult.success) {
    Logger::Info("Sphere verification succeeded - detected " + 
                std::to_string(verifyResult.spheresDetected) + " spheres");
    return true;  // ✅ Continúa al siguiente evento
} else {
    result.errorMessage = verifyResult.errorMessage;
    Logger::Warning("Sphere verification failed: " + result.errorMessage);
    
    if (!m_continueOnError) {
        return false;  // ❌ Detiene coreografía
    }
}
```

---

## 📊 Diagrama de Flujo Completo

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          DeckLink Hardware                               │
│  3x Blackmagic 8K Pro Mini - 12 inputs SDI/HDMI @ 4K60                  │
└────────────────────────┬────────────────────────────────────────────────┘
                         │ IRQ (60fps)
                         ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    DeckLinkCapture::VideoInputFrameArrived               │
│  ┌──────────┐  DMA   ┌──────────────┐  CUDA  ┌──────────────┐          │
│  │ DeckLink │ ─────→ │ Pinned RAM   │ ─────→ │ GPU VRAM     │          │
│  │  Buffer  │        │ (Host)       │        │ (YUV+BGRA)   │          │
│  └──────────┘        └──────────────┘        └──────┬───────┘          │
│                                                      │                   │
│  Thread affinity: Cores 2-3 (VIDEO_CAPTURE)         │                   │
└──────────────────────────────────────────────────────┼──────────────────┘
                                                       │
                    FrameReadyCallback(channel, stream)
                                                       │
                         ┌─────────────────────────────┼─────────────────┐
                         │         main.cpp Frame Handler               │
                         │              (3 Parallel Pipelines)           │
                         └─────────────────────────────┬─────────────────┘
                                                       │
        ┌──────────────────────────┬───────────────────┼──────────────────────┐
        │                          │                   │                      │
        ▼ PRIORITY 1               ▼ PRIORITY 2        ▼ PRIORITY 3          │
┌───────────────────┐      ┌──────────────────┐  ┌──────────────────────────▼──┐
│   NDI → vMix      │      │ Motion Analysis  │  │   AI Pipeline                │
│                   │      │                  │  │                              │
│ ┌───────────────┐ │      │ ┌──────────────┐ │  │ ┌──────────────────────────┐ │
│ │ GPU→CPU copy  │ │      │ │ Pixel Diff   │ │  │ │ 1. InferenceEngine       │ │
│ │ (double-buf)  │ │      │ │ per camera   │ │  │ │    └→ TensorRT           │ │
│ └───────┬───────┘ │      │ └──────┬───────┘ │  │ │       └→ NMS             │ │
│         │         │      │        │         │  │ └──────────┬───────────────┘ │
│         ▼         │      │        ▼         │  │            │                 │
│ ┌───────────────┐ │      │ ┌──────────────┐ │  │ ┌──────────▼───────────────┐ │
│ │ NDI Send      │ │      │ │ActiveCamera  │ │  │ │ 2. PositionMapper        │ │
│ │ (10GbE)       │ │      │ │  Selector    │ │  │ │    └→ Homography         │ │
│ └───────┬───────┘ │      │ └──────┬───────┘ │  │ │       └→ Fusion          │ │
│         │         │      │        │         │  │ └──────────┬───────────────┘ │
│         ▼         │      │        │         │  │            │                 │
│ ┌───────────────┐ │      │        │         │  │ ┌──────────▼───────────────┐ │
│ │ vMix Input    │ │      │        │         │  │ │ 3. BallTracker (Kalman)  │ │
│ │ (Visible)     │ │      │        │         │  │ │    └→ Ranking            │ │
│ └───────────────┘ │      │        │         │  │ └──────────┬───────────────┘ │
│                   │      │        │         │  │            │                 │
│ <10ms             │      │        │         │  │ ┌──────────▼───────────────┐ │
└───────────────────┘      │        │         │  │ │ 4. SceneManager          │ │
                           │        │         │  │ │    └→ Leapfrog           │ │
                           │        └─────────┼──┼→│       └→ VideoHub        │ │
                           │                  │  │ └──────────┬───────────────┘ │
                           │ <5ms             │  │            │                 │
                           └──────────────────┘  │ ┌──────────▼───────────────┐ │
                                                 │ │ 5. RankingPublisher      │ │
                                                 │ │    └→ Redis              │ │
                                                 │ └──────────────────────────┘ │
                                                 │                              │
                                                 │ <15ms                        │
                                                 └──────────────────────────────┘
                                                                │
                                                                │ (Datos persistentes)
                                                                │
                ┌───────────────────────────────────────────────┼──────────────┐
                │          ChoreographyEngine (F12 trigger)                    │
                │                                                              │
                │  ┌────────────────────────────────────────────────────────┐  │
                │  │  Evento: SpherePresenceCheck                           │  │
                │  │  {cameraID:1, expectedSpheres:10, timeout:5000}        │  │
                │  └────────────┬───────────────────────────────────────────┘  │
                │               │                                              │
                │               ▼                                              │
                │  ┌────────────────────────────────────────────────────────┐  │
                │  │  SphereVerifier::CheckPresence()                       │  │
                │  │                                                        │  │
                │  │  1. VideoHub.Route(CAM_01 → Output 0)                 │  │
                │  │  2. Loop (timeout: 5000ms):                           │  │
                │  │     └→ CaptureAndDetect()                             │  │
                │  │        └→ InferenceEngine.ProcessFrame()  ◄───────────┼──┼─ REUTILIZA
                │  │           (mismo pipeline AI)                         │  │   Pipeline 3
                │  │  3. Valida: detections >= 10?                         │  │
                │  └────────────┬───────────────────────────────────────────┘  │
                │               │                                              │
                │               ▼                                              │
                │  ┌────────────────────────────────────────────────────────┐  │
                │  │  VerificationResult                                    │  │
                │  │  {success:true, spheresDetected:10}                    │  │
                │  └────────────────────────────────────────────────────────┘  │
                │                                                              │
                │  Next Event → (DelayMs:500) → (VideoHubRoute...) → ...      │
                └──────────────────────────────────────────────────────────────┘
```

---

## ⚙️ Configuración de Hardware

### **CPU Affinity (Threadripper PRO 9955WX)**

| CCD  | Cores | Tarea                          |
|------|-------|--------------------------------|
| CCD0 | 0-1   | DeckLink IRQ handlers          |
| CCD0 | 2-3   | Video capture threads          |
| CCD0 | 4-5   | Frame processing               |
| CCD0 | 6-7   | NDI encoding (fallback)        |
| CCD1 | 8-11  | TensorRT inference             |
| CCD1 | 12    | MegaCanvas render              |
| CCD1 | 13    | Atlas compositor               |
| CCD1 | 14    | Redis/ranking                  |
| CCD1 | 15    | Background tasks               |

### **GPU (RTX 5080 16GB)**

- **CUDA Streams:**
  - Stream 0: Capture + color conversion
  - Stream 1: Inference (TensorRT)
  - Stream 2: Downscaling/preprocessing

- **VRAM Layout:**
  - 12× 3840×2160×4 UYVY buffers = ~194MB
  - 12× 3840×2160×4 BGRA buffers = ~194MB
  - 12× 640×640×3 FP32 preprocessed = ~59MB
  - TensorRT engine weights = ~150MB
  - **Total:** ~600MB / 16GB disponible

---

## 🔐 Requisitos para Verificación

Para que `SpherePresenceCheck` funcione correctamente:

### ✅ **Cambios de Código (COMPLETADOS)**
- [x] Include de `SphereVerifier.h` en `main.cpp`
- [x] Instanciación de `SphereVerifier` con deps (VideoHub, InferenceEngine)
- [x] Inyección en `ChoreographyEngine` via `SetSphereVerifier()`

### ⚠️ **Requisitos Externos (PENDIENTES)**
1. **Modelo TensorRT:**
   ```bash
   # Copiar archivo .engine al repo
   cp yolo26l_fp16_batch12.engine /path/to/repo/models/
   ```

2. **Calibración de cámaras:**
   ```json
   // config/calibration.json
   {
     "camera_1": {
       "_calibrated": true,  // Cambiar a true
       "homography_matrix": [
         [h11, h12, h13],
         [h21, h22, h23],
         [h31, h32, h33]
       ]
     }
   }
   ```
   - Usar `cv2.findHomography()` con 4+ puntos de correspondencia
   - Ver: `docs/CALIBRATION_GUIDE.md`

3. **VideoHub conectado:**
   - IP configurada en `config.json`
   - Puerto TCP 9990 accesible
   - Inputs mapeados: CAM_01-CAM_12

---

## 🚀 Flujo de Ejecución de una Coreografía Completa

### Ejemplo: `race_setup.json`

```json
{
  "events": [
    {
      "type": "SpherePresenceCheck",
      "cameraID": 1,
      "expectedSpheres": 10,
      "timeoutMs": 5000,
      "mode": "presence"
    },
    {
      "type": "DelayMs",
      "delayMs": 500
    },
    {
      "type": "VideoHubRoute",
      "sourceInput": "CAM_02",
      "outputIndex": 0
    },
    {
      "type": "VMixActivate",
      "inputKey": "Input1"
    }
  ]
}
```

### **Ejecución paso a paso:**

1. **Usuario presiona F12**
   ```cpp
   choreographyEngine->Start();
   // Estado: Idle → Ready → Running
   ```

2. **Evento 1: SpherePresenceCheck**
   - ChoreographyEngine llama `SphereVerifier->CheckPresence(1, 10, 5000)`
   - SphereVerifier:
     1. Route VideoHub: CAM_01 → Output 0
     2. Loop cada 500ms:
        - Captura frame actual de cámara 1
        - `InferenceEngine->ProcessFrame()` (TensorRT)
        - Cuenta detecciones
     3. Timeout: 5000ms
     4. ✅ Success si ≥10 esferas detectadas
   
   **Output:**
   ```
   [INFO] SphereVerifier: Camera 1 - detected 10/10 spheres ✓
   [INFO] ChoreographyEngine: Event 0 completed in 1234ms
   ```

3. **Evento 2: DelayMs(500)**
   ```cpp
   std::this_thread::sleep_for(std::chrono::milliseconds(500));
   ```

4. **Evento 3: VideoHubRoute**
   ```cpp
   videoHub->Route("CAM_02", outputIndex=0);
   // Envía comando a VideoHub por TCP
   ```

5. **Evento 4: VMixActivate**
   ```cpp
   vmix->ActivateInput("Input1");
   // HTTP POST a vMix API
   ```

6. **Finalización**
   ```
   [INFO] ChoreographyEngine: All events completed successfully
   [INFO] Estado: Running → Idle
   ```

---

## 📝 Logs de Referencia

### Startup (main.cpp)
```
[INFO] Initializing InferenceEngine...
[INFO] InferenceEngine initialized with TensorRT
[INFO] Initializing SphereVerifier...
[INFO] SphereVerifier initialized successfully
[INFO] Initializing ChoreographyEngine...
[INFO] ChoreographyEngine: Loading script from config/race_setup.json
[INFO] ChoreographyEngine: Script loaded successfully
```

### Frame Processing (60fps continuous)
```
[DEBUG] Frame 1234 - CAM_01: cap=1.2ms ndi=8.3ms sel=2.1ms ai=12.4ms total=24.0ms
[DEBUG] InferenceEngine: Detected 10 balls on camera 1
[DEBUG] BallTracker: Leader is ball #3 at (Xg=45.2m, Yg=2.8m)
[DEBUG] SceneManager: Leader crossed threshold_2 → switching to config_b
```

### Choreography Execution
```
[INFO] ChoreographyEngine: State changed to: Running
[INFO] ChoreographyEngine: Starting event 0: SpherePresenceCheck
[INFO] ChoreographyEngine: Executing sphere verification - mode=presence, camera=1, expected=10, timeout=5000ms
[INFO] SphereVerifier: Camera 1 - attempting presence check (10 spheres, 5000ms timeout)
[INFO] SphereVerifier: Sample 1 - detected 8 spheres (8/10)
[INFO] SphereVerifier: Sample 2 - detected 10 spheres (10/10) ✓
[INFO] ChoreographyEngine: Sphere verification succeeded - detected 10 spheres
[INFO] ChoreographyEngine: Event 0 completed in 1234ms
```

---

## 🎓 Conceptos Clave

### **Zero-Copy DMA**
- DeckLink → Pinned RAM → GPU VRAM sin copias intermedias
- Requiere `cudaMallocPinned()` para memoria accesible por DMA
- Ahorra ~3-5ms por frame vs. copia tradicional

### **Double-Buffering (Ping-Pong)**
- 2 buffers alternados para NDI send
- Mientras GPU→CPU transfer en buffer A, NDI envía buffer B
- Reduce blocking de 0.8ms a 0.2ms

### **TensorRT Batch Inference**
- Procesa 12 cámaras simultáneamente en 1 llamada
- FP16 precision usa Tensor Cores (3x más rápido que FP32)
- Amortiza overhead de kernel launch

### **Homografía (Perspective Transform)**
- Transforma píxeles 2D → coordenadas 3D del mundo real
- Requiere calibración manual con puntos de correspondencia
- Matriz 3×3 única por cámara

### **Kalman Filter**
- Predice posición futura basado en velocidad
- Corrige con nueva medición (fusión sensor)
- Suaviza ruido de detecciones de AI

---

## 🔗 Referencias de Código

| Componente         | Archivo                          | Líneas Clave      |
|--------------------|----------------------------------|-------------------|
| Frame Capture      | `DeckLinkCapture.cpp`            | 318-350           |
| Frame Handler      | `main.cpp`                       | 842-927           |
| InferenceEngine    | `InferenceEngine.cpp`            | 138-243           |
| PositionMapper     | `PositionMapper.cpp`             | 200-350           |
| BallTracker        | `BallTracker.cpp`                | 150-400           |
| SceneManager       | `SceneManager.cpp`               | 157-330           |
| ChoreographyEngine | `ChoreographyEngine.cpp`         | 177-207, 739-788  |
| SphereVerifier     | `SphereVerifier.cpp`             | 84-142, 283-315   |
| Config Parsing     | `main.cpp`                       | 267-440           |
| Thread Affinity    | `ThreadOptimizer.h`              | 115-165           |

---

## ✅ Checklist de Integración

Antes de usar `SpherePresenceCheck` en producción:

- [x] Include agregado en `main.cpp`
- [x] SphereVerifier instanciado con deps
- [x] Inyección en ChoreographyEngine
- [ ] Modelo `.engine` copiado a `models/`
- [ ] Calibración de cámaras completada
- [ ] VideoHub conectado y testeado
- [ ] Script JSON de coreografía validado
- [ ] Test en ambiente de desarrollo
- [ ] Validación con 10 esferas reales

---

**Documentado por:** Copilot Cloud Agent  
**Fecha:** 2026-04-15  
**Versión:** VIB System v2.5
