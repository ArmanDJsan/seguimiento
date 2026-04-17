# Análisis de Flujos: Desde la Captura hasta el Final

## Diagrama de Flujo General

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          SISTEMA VIB (Visual Intelligence Bypass)            │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│  1. CAPTURA DE VIDEO                                                         │
│  ─────────────────────                                                       │
│  ┌────────────────────┐    DMA Zero-Copy    ┌────────────────────────────┐  │
│  │ Blackmagic DeckLink│ ──────────────────► │ CUDA Pinned Memory (GPU)   │  │
│  │ 8K Pro Mini (x3)   │    (UYVY 4:2:2)     │ cudaHostAllocMapped        │  │
│  │ 12 x 4K@60fps      │                     │                            │  │
│  └────────────────────┘                     └────────────────────────────┘  │
│         ▼                                              │                     │
│  IDeckLinkVideoBufferAllocator                         │                     │
│  Custom allocator para memoria GPU                     │                     │
└───────────────────────────────────────────────────────┼─────────────────────┘
                                                         │
                                                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  2. BIFURCACIÓN DEL FLUJO (Paralelo)                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                               │                                              │
│         ┌─────────────────────┴───────────────────────┐                     │
│         ▼                                             ▼                      │
│  ┌──────────────────┐                     ┌──────────────────────┐          │
│  │ FLUJO DE VIDEO   │                     │ FLUJO DE IA          │          │
│  │ (Salida a vMix)  │                     │ (Detección YOLO)     │          │
│  └──────────────────┘                     └──────────────────────┘          │
└─────────────────────────────────────────────────────────────────────────────┘
```

## FLUJO A: Video a vMix (NDI)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  3A. SALIDA NDI (Video a vMix)                                              │
│  ─────────────────────────────                                               │
│                                                                              │
│  ┌─────────────────┐      ┌─────────────────┐      ┌───────────────────┐   │
│  │ CUDA UYVY       │ ──►  │ cudaMemcpyAsync │ ──►  │ Pinned Host       │   │
│  │ Buffer (GPU)    │      │ (Double Buffer) │      │ Memory            │   │
│  └─────────────────┘      └─────────────────┘      └───────────────────┘   │
│                                                             │                │
│                                                             ▼                │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ NDIManager                                                           │   │
│  │ ├─ 12 NDI Senders: VIB_CAM_01 a VIB_CAM_12                          │   │
│  │ ├─ Formato: UYVY (nativo DeckLink, evita conversión)                │   │
│  │ ├─ Async send via NDIlib_send_send_video_async_v2()                 │   │
│  │ └─ Ping-pong buffers para overlap GPU→CPU con NDI send             │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                     │                                        │
│                                     ▼                                        │
│                          ┌─────────────────────┐                            │
│                          │ vMix (Receptor NDI) │                            │
│                          │ ├─ 12 inputs NDI    │                            │
│                          │ └─ RGB interno      │                            │
│                          └─────────────────────┘                            │
└─────────────────────────────────────────────────────────────────────────────┘
```

## FLUJO B: Detección de Bolas con IA

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  3B. INFERENCIA YOLO (TensorRT)                                             │
│  ──────────────────────────────                                              │
│                                                                              │
│  ┌─────────────────┐      ┌─────────────────────────────────────────┐       │
│  │ CUDA UYVY       │ ──►  │ Fused Kernel (PreprocessKernel.cu)      │       │
│  │ Buffer (4K)     │      │ ├─ UYVY → RGB                           │       │
│  └─────────────────┘      │ ├─ Resize 4K → 640x640                  │       │
│                           │ └─ Normalize [0,1]                      │       │
│                           └─────────────────────────────────────────┘       │
│                                          │                                   │
│                                          ▼                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ InferenceEngine (TensorRT 10)                                         │  │
│  │ ├─ Modelo: YOLO con FP16                                              │  │
│  │ ├─ Batch Size: 12 (procesa 12 cámaras simultáneamente)               │  │
│  │ ├─ Output: BallDetection[]                                            │  │
│  │ │   └─ {ballID, cameraID, x, y, width, height, confidence}           │  │
│  │ └─ Performance: <15ms para batch de 12 frames                        │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                          │                                   │
│                                          ▼                                   │
│                           ┌──────────────────────────┐                      │
│                           │ Output: BallDetection[]  │                      │
│                           │ 10 bolas detectadas      │                      │
│                           └──────────────────────────┘                      │
└─────────────────────────────────────────────────────────────────────────────┘
```

## FLUJO C: Transformación y Tracking

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  4. MAPEO DE COORDENADAS Y TRACKING                                         │
│  ──────────────────────────────────                                          │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ PositionMapper (Homografía)                                           │  │
│  │ ├─ Matriz 3x3 por cámara (precalibrada)                              │  │
│  │ ├─ Pixel (x,y) → Global (Xg, Yg) metros                              │  │
│  │ ├─ Fusión multi-cámara para zonas superpuestas                       │  │
│  │ └─ 16 cámaras: 12 streaming + 4 zenith (RADAR_01-04)                 │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                          │                                   │
│                                          ▼                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ BallTracker (Filtro de Kalman)                                        │  │
│  │ ├─ 10 Kalman filters (uno por bola)                                  │  │
│  │ ├─ Hungarian algorithm para asociación detección-track               │  │
│  │ ├─ Manejo de oclusiones (predicción sin medición)                    │  │
│  │ ├─ Cálculo de velocidad (Vx, Vy)                                     │  │
│  │ └─ Output: TrackedBall {Xg, Yg, Vx, Vy, confidence, ranking}         │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                          │                                   │
│          ┌───────────────────────────────┼───────────────────────────┐      │
│          ▼                               ▼                           ▼      │
│  ┌──────────────┐              ┌───────────────┐           ┌────────────┐  │
│  │ SceneManager │              │ RedisWorker   │           │ Ranking    │  │
│  │ (Leapfrog)   │              │ (60Hz async)  │           │ Publisher  │  │
│  └──────────────┘              └───────────────┘           └────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

## FLUJO D: Control de Escenas y Enrutamiento

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  5. GESTIÓN DE ESCENAS (SceneManager)                                       │
│  ────────────────────────────────────                                        │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ SceneManager (Leapfrogging Strategy)                                  │  │
│  │ ├─ 8 slots físicos (G1-G8) para 12 cámaras streaming                 │  │
│  │ ├─ 4 cámaras zenith fijas (f1-f4) - siempre conectadas               │  │
│  │ ├─ Modos: AUTO (posición líder) o MANUAL (teclado)                   │  │
│  │ ├─ Umbrales de trigger por posición X del líder                      │  │
│  │ └─ Mute temporal durante cambio de señal (200ms)                     │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                          │                                   │
│                                          ▼                                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ VideoHubClient (Blackmagic VideoHub)                                  │  │
│  │ ├─ TCP socket control                                                 │  │
│  │ ├─ RouteInputToOutput(slot, cameraID)                                │  │
│  │ └─ 12 entradas → 8 salidas routing dinámico                         │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                          │                                   │
│                                          ▼                                   │
│                              ┌─────────────────────┐                        │
│                              │ DeckLink Capture    │                        │
│                              │ (actualiza entrada) │                        │
│                              └─────────────────────┘                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

## FLUJO E: Publicación de Datos

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  6. PUBLICACIÓN DE DATOS (Redis + vMix)                                     │
│  ──────────────────────────────────────                                      │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ RedisWorker (Worker Thread @ 60Hz)                                    │  │
│  │ ├─ Lee TrackedBall[] del BallTracker                                 │  │
│  │ ├─ Serializa a JSON                                                  │  │
│  │ ├─ SET + PUBLISH a Redis                                             │  │
│  │ └─ Retry automático si Redis cae                                     │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                          │                                   │
│                         ┌────────────────┴────────────────┐                 │
│                         ▼                                 ▼                  │
│  ┌──────────────────────────┐              ┌──────────────────────────────┐│
│  │ Redis Server             │              │ vMix Script (VB.NET)         ││
│  │ ├─ VMIX_DATA_STREAM      │   ◄──────►   │ ├─ SUBSCRIBE vmix_update     ││
│  │ └─ vmix_update channel   │              │ └─ Overlay gráficos          ││
│  └──────────────────────────┘              └──────────────────────────────┘│
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ RankingPublisher                                                      │  │
│  │ └─ Publica posiciones de ranking actualizadas                        │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

## FLUJO F: Automatización y Coreografía

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  7. COREOGRAFÍA (Automatización de vMix)                                    │
│  ──────────────────────────────────────                                      │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ ChoreographyEngine                                                    │  │
│  │ ├─ Carga scripts JSON/DSL                                            │  │
│  │ ├─ Timer de alta resolución                                          │  │
│  │ ├─ Estados: Idle → Ready → Running → Paused → Stopping               │  │
│  │ └─ Eventos:                                                           │  │
│  │     ├─ VMixCommand: Fade, Cut, Transition                            │  │
│  │     ├─ NDISlotChange: Cambio de cámara en slot                       │  │
│  │     ├─ SceneSwitch: Cambio de configuración de grupo                 │  │
│  │     ├─ Timer: Pausa/espera                                           │  │
│  │     └─ SphereVerification: Verificar presencia de bolas              │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                          │                                   │
│          ┌───────────────────────────────┼───────────────────────────┐      │
│          ▼                               ▼                           ▼      │
│  ┌──────────────────┐         ┌───────────────┐         ┌────────────────┐ │
│  │ VMixController   │         │ SceneManager  │         │ SphereVerifier │ │
│  │ HTTP API calls   │         │ (routing)     │         │ (validación)   │ │
│  └──────────────────┘         └───────────────┘         └────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
```

## FLUJO G: Verificación de Esferas

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  8. VERIFICACIÓN (SphereVerifier)                                           │
│  ────────────────────────────────                                            │
│                                                                              │
│  Modos de verificación:                                                     │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ PRESENCE_CHECK    → Verifica que todas las bolas esperadas existen   │  │
│  │ POSITION_SNAPSHOT → Captura posiciones actuales de todas las bolas   │  │
│  │ ARRIVAL_ORDER     → Rastrea orden de llegada a línea de meta         │  │
│  │ CHECKPOINT_PASS   → Monitorea paso por zonas de control              │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
│  Flujo:                                                                     │
│  ┌──────────┐    ┌─────────────────┐    ┌───────────────────────────────┐  │
│  │ VideoHub │ ►  │ Ruta cámara     │ ►  │ InferenceEngine               │  │
│  │ routing  │    │ específica      │    │ (detección)                   │  │
│  └──────────┘    └─────────────────┘    └───────────────────────────────┘  │
│                                                      │                       │
│                                                      ▼                       │
│                                         ┌───────────────────────────────┐   │
│                                         │ VerificationResult            │   │
│                                         │ {success, sphereIDs,          │   │
│                                         │  positions, arrivalOrder}     │   │
│                                         └───────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Resumen de Componentes y Responsabilidades

| Componente | Responsabilidad | Performance Target |
|------------|----------------|-------------------|
| DeckLinkCapture | Captura zero-copy DMA desde hardware Blackmagic | Sub-frame latency |
| NDIManager | Salida de video a vMix via NDI | <2ms por frame |
| InferenceEngine | Detección YOLO con TensorRT | <15ms batch 12 frames |
| PositionMapper | Transformación pixel→global (homografía) | <0.1ms batch 100 |
| BallTracker | Tracking multi-objeto con Kalman | <1ms para 10 bolas |
| SceneManager | Gestión dinámica de cámaras (leapfrog) | <10ms cambio grupo |
| VideoHubClient | Control de routing Blackmagic VideoHub | TCP socket |
| RedisWorker | Publicación asíncrona de datos | 60Hz |
| ChoreographyEngine | Automatización de secuencias vMix | Alta resolución timer |
| SphereVerifier | Validación de presencia/posición de bolas | Configurable timeout |
| VMixController | Control HTTP de vMix | API REST |

## Arquitectura de Threading

```
Main Thread          → DirectX, inicialización
Capture Threads (12) → Frame arrival callbacks (lightweight)
YOLO Worker          → Batch inference processing
Redis Worker         → Data publishing (60Hz)
Choreography Thread  → Event execution
NDI Thread (interno) → Async sending
```

## Decisiones de Arquitectura Clave

1. **Zero-Copy DMA**: IDeckLinkMemoryAllocator personalizado con cudaHostAllocMapped
2. **GPU Pixel Shader**: Conversión YUV→RGB enteramente en GPU
3. **NDI sobre Spout**: vMix tiene soporte nativo NDI (sin plugins)
4. **Redis como Bus de Datos**: Desacopla video processing de metadata updates
5. **Batch Processing YOLO**: Maximiza uso de Tensor Cores
6. **No Frame Skipping**: Cada frame se analiza, errores se reportan
