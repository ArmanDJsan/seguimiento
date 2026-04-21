┌─────────────────────────────────────────────────────────────────────────────────────┐
│               FLUJO DE FRAME COMPLETO CON GENERACIÓN DE EVENTOS (33.3ms @ 30fps)     │
└─────────────────────────────────────────────────────────────────────────────────────┘

╔═══════════════════════════════════════════════════════════════════════════════════════╗
║ TIMELINE DE UN FRAME (33.3ms totales disponibles)                                     ║
╠═══════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                       ║
║  t=0ms     t=0.5ms     t=3ms       t=8ms       t=10ms      t=12ms      t=15ms        ║
║    │          │          │           │            │           │           │          ║
║    ▼          ▼          ▼           ▼            ▼           ▼           ▼          ║
║  ┌────┐  ┌───────┐  ┌────────┐  ┌─────────┐  ┌────────┐  ┌────────┐  ┌────────┐     ║
║  │PTZ │  │ Zero- │  │YUV→RGB │  │  YOLO   │  │ Track  │  │ Event  │  │ Scene  │     ║
║  │SDI │─▶│ Copy  │─▶│  640   │─▶│ Infer  │─▶│+ Zone  │─▶│  Gen   │─▶│Manager │     ║
║  │IRQ │  │Buffer │  │Kernel │  │ Batch4 │  │ Check  │  │        │  │        │     ║
║  └────┘  └───────┘  └────────┘  └─────────┘  └────────┘  └────────┘  └────┬───┘     ║
║                                                                           │          ║
║                                                           Si hay evento ──┘          ║
║                                                                  │                   ║
║                                                                  ▼                   ║
║                                                          ┌───────────────┐           ║
║                                                          │   VideoHub    │           ║
║                                                          │   Routing     │           ║
║                                                          │   Change      │           ║
║                                                          └───────────────┘           ║
║                                                                                       ║
║  ┌─────────────────────────────────────────────────────────────────────────────────┐ ║
║  │ TIEMPO USADO: ~15ms   │   TIEMPO LIBRE: ~18ms   │   MARGEN: 54%                │ ║
║  └─────────────────────────────────────────────────────────────────────────────────┘ ║
║                                                                                       ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝

╔═══════════════════════════════════════════════════════════════════════════════════════╗
║ DETALLE: FLUJO DE DATOS PASO A PASO                                                   ║
╠═══════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 1: CAPTURA SDI (IRQ driven) - Solo PTZ 1080p@30fps                          │║
║  │                                                                                  │║
║  │   PTZ/RADAR 1      PTZ/RADAR 2      PTZ/RADAR 3      PTZ/RADAR 4                 │║
║  │   ┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐                │║
║  │   │ 1920×1080│     │ 1920×1080│     │ 1920×1080│     │ 1920×1080│                │║
║  │   │ UYVY 422 │     │ UYVY 422 │     │ UYVY 422 │     │ UYVY 422 │                │║
║  │   │ 4.15 MB  │     │ 4.15 MB  │     │ 4.15 MB  │     │ 4.15 MB  │                │║
║  │   │ Sector 1 │     │ Sector 2 │     │ Sector 3 │     │ Sector 4 │                │║
║  │   └────┬─────┘     └────┬─────┘     └────┬─────┘     └────┬─────┘                │║
║  │        │ SDI            │ SDI            │ SDI            │ SDI                  │║
║  │        ▼                ▼                ▼                ▼                      │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                    DeckLink Duo 2 (4 inputs)                            │   │║
║  │   │                    IRQ → Callback → Frame Queue                         │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 2: ZERO-COPY DMA A GPU                                                      │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                      CUDA Pinned Host Memory                            │   │║
║  │   │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐                │   │║
║  │   │  │ Buffer 0 │  │ Buffer 1 │  │ Buffer 2 │  │ Buffer 3 │                │   │║
║  │   │  │ 4.15 MB  │  │ 4.15 MB  │  │ 4.15 MB  │  │ 4.15 MB  │                │   │║
║  │   │  │ PTZ1 YUV │  │ PTZ2 YUV │  │ PTZ3 YUV │  │ PTZ4 YUV │                │   │║
║  │   │  └──────────┘  └──────────┘  └──────────┘  └──────────┘                │   │║
║  │   │                                                                         │   │║
║  │   │  Total memoria: ~32MB (vs 398MB si capturáramos 4K)                     │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 3: CONVERSIÓN YUV→RGB640 (Kernel CUDA)             /no escalar solo convertir                          │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │              CUDA Kernel: UYVY_1080p_to_RGB_640                         │   │║
║  │   │                                                                         │   │║
║  │   │  Operaciones fusionadas:                                                │   │║
║  │   │  1. Leer UYVY de memoria host-mapped                                    │   │║
║  │   │  2. Convertir YUV→RGB                                                   │   │║
║  │   │  3. Resize bilinear 1920×1080 →1920×1080                               │   │║
║  │   │  4. Normalizar 0-255 → 0.0-1.0                                          │   │║
║  │   │  5. Reordenar HWC → CHW (para TensorRT)                                 │   │║
║  │   │                                                                         │   │║
║  │   │  Output: 4 × [3, 640, 640] tensors en GPU                               │   │║
║  │   │  Tiempo: ~0.5ms para 4 frames                                           │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 4: YOLO v8 INFERENCE (TensorRT)                                             │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                      YOLO v8 TensorRT Engine                            │   │║
║  │   │                                                                         │   │║
║  │   │  Input: Batch de 4 × [3,  1080, 1920]                                    │   │║
║  │   │  Model: yolov8n.engine (optimizado FP16)                                │   │║
║  │   │  Detección: personas, vehículos, objetos específicos                    │   │║
║  │   │                                                                         │   │║
║  │   │  Output por cámara:                                                     │   │║
║  │   │  ┌───────────────────────────────────────────────────────────────────┐ │   │║
║  │   │  │ PTZ1: 3 detecciones → [{x:120, y:340, class:"person", conf:0.92}] │ │   │║
║  │   │  │ PTZ2: 2 detecciones → [{x:450, y:280, class:"person", conf:0.88}] │ │   │║
║  │   │  │ PTZ3: 4 detecciones → [{x:320, y:190, class:"person", conf:0.95}] │ │   │║
║  │   │  │ PTZ4: 1 detección  → [{x:510, y:420, class:"person", conf:0.87}] │ │   │║
║  │   │  └───────────────────────────────────────────────────────────────────┘ │   │║
║  │   │                                                                         │   │║
║  │   │  Tiempo inferencia: ~5ms (RTX 5080)                                     │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 5: TRACKING + ZONE CHECKING                                                 │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                         ObjectTracker                                   │   │║
║  │   │                                                                         │   │║
║  │   │  1. Asociar detecciones con tracks existentes (Hungarian)               │   │║
║  │   │  2. Kalman predict + update para cada track                             │   │║
║  │   │  3. Identificar LÍDER (objeto más adelantado en dirección de carrera)   │   │║
║  │   │                                                                         │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                         ZoneChecker (NUEVO)                             │   │║
║  │   │                                                                         │   │║
║  │   │  Zonas definidas por calibración (polígonos en coordenadas de pista):   │   │║
║  │   │                                                                         │   │║
║  │   │  ┌─────────────────────────────────────────────────────────────────┐   │   │║
║  │   │  │                                                                 │   │   │║
║  │   │  │   ZONE_START        ZONE_MID           ZONE_FINISH             │   │   │║
║  │   │  │   ┌────────┐       ┌─────────┐        ┌─────────┐              │   │   │║
║  │   │  │   │ 0-25m  │       │ 25-60m  │        │ 60-100m │              │   │   │║
║  │   │  │   │config_a│  ───▶ │config_b │  ───▶  │config_c │              │   │   │║
║  │   │  │   └────────┘       └─────────┘        └─────────┘              │   │   │║
║  │   │  │                                                                 │   │   │║
║  │   │  │   PTZ1 cubre      PTZ2+PTZ3 cubren    PTZ4 cubre               │   │   │║
║  │   │  │   esta zona       esta zona            esta zona                │   │   │║
║  │   │  │                                                                 │   │   │║
║  │   │  └─────────────────────────────────────────────────────────────────┘   │   │║
║  │   │                                                                         │   │║
║  │   │  Verificación por frame:                                                │   │║
║  │   │  • ¿El líder cruzó de ZONE_START a ZONE_MID? → Generar evento          │   │║
║  │   │  • ¿El líder cruzó de ZONE_MID a ZONE_FINISH? → Generar evento         │   │║
║  │   │                                                                         │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 6: GENERACIÓN DE EVENTOS                                                    │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                         EventGenerator (NUEVO)                          │   │║
║  │   │                                                                         │   │║
║  │   │  Genera eventos cuando hay cambios significativos:                      │   │║
║  │   │                                                                         │   │║
║  │   │  ┌────────────────────────────────────────────────────────────────┐    │   │║
║  │   │  │ ZONE_ENTRY Event:                                              │    │   │║
║  │   │  │ {                                                              │    │   │║
║  │   │  │   "type": "ZONE_ENTRY",                                        │    │   │║
║  │   │  │   "timestamp": 1682106234567,                                  │    │   │║
║  │   │  │   "zone": "ZONE_MID",                                          │    │   │║
║  │   │  │   "leader_id": "track_42",                                     │    │   │║
║  │   │  │   "position": {"x": 26.5, "y": 4.2},                           │    │   │║
║  │   │  │   "confidence": 0.95,                                          │    │   │║
║  │   │  │   "suggested_config": "config_b"                               │    │   │║
║  │   │  │ }                                                              │    │   │║
║  │   │  └────────────────────────────────────────────────────────────────┘    │   │║
║  │   │                                                                         │   │║
║  │   │  El evento se envía a SceneManager para decisión                        │   │║
║  │   │                                                                         │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 7: SCENE MANAGER - DECISIÓN                                                 │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                    SceneManager (Modificado)                            │   │║
║  │   │                                                                         │   │║
║  │   │  ANTES: Evaluaba thresholds de posición cada frame                      │   │║
║  │   │  AHORA: Reacciona a EVENTOS de radar/zona                               │   │║
║  │   │                                                                         │   │║
║  │   │  Lógica de decisión:                                                    │   │║
║  │   │  ┌────────────────────────────────────────────────────────────────┐    │   │║
║  │   │  │ on_event(event):                                               │    │   │║
║  │   │  │   if event.type == "ZONE_ENTRY":                               │    │   │║
║  │   │  │     if event.zone == "ZONE_START":                             │    │   │║
║  │   │  │       apply_config("config_a")  # G1-G4: 1,2,3,12              │    │   │║
║  │   │  │                                 # G5-G8: 4,5,6,7               │    │   │║
║  │   │  │     elif event.zone == "ZONE_MID":                             │    │   │║
║  │   │  │       apply_config("config_b")  # G1-G4: 8,9,10,12             │    │   │║
║  │   │  │                                 # G5-G8: 4,5,6,7               │    │   │║
║  │   │  │     elif event.zone == "ZONE_FINISH":                          │    │   │║
║  │   │  │       apply_config("config_c")  # G1-G4: 8,9,10,12             │    │   │║
║  │   │  │                                 # G5-G8: 11,1,2,3              │    │   │║
║  │   │  │                                                                │    │   │║
║  │   │  │   elif event.type == "EXTERNAL_TRIGGER":                       │    │   │║
║  │   │  │     apply_config(event.config)  # Forzar config específica     │    │   │║
║  │   │  └────────────────────────────────────────────────────────────────┘    │   │║
║  │   │                                                                         │   │║
║  │   │  Histéresis: Requiere 3+ frames confirmando antes de cambiar            │   │║
║  │   │  Cooldown: Mínimo 500ms entre cambios de config                         │   │║
║  │   │                                                                         │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ PASO 8: VIDEOHUB ROUTING (Solo cuando hay cambio de config)                      │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                     VideoHubClient                                      │   │║
║  │   │                                                                         │   │║
║  │   │  Ejemplo: Cambio de config_a → config_b                                 │   │║
║  │   │                                                                         │   │║
║  │   │  Config_A actual:               Config_B nuevo:                         │   │║
║  │   │  G1←CAM1, G2←CAM2              G1←CAM8, G2←CAM9                         │   │║
║  │   │  G3←CAM3, G4←CAM12    →        G3←CAM10, G4←CAM12                       │   │║
║  │   │  G5←CAM4, G6←CAM5              G5←CAM4, G6←CAM5  (sin cambio)           │   │║
║  │   │  G7←CAM6, G8←CAM7              G7←CAM6, G8←CAM7  (sin cambio)           │   │║
║  │   │                                                                         │   │║
║  │   │  Comandos enviados (solo los que cambian):                              │   │║
║  │   │  ┌────────────────────────────────────────────────────────────────┐    │   │║
║  │   │  │ VIDEO OUTPUT ROUTING:                                          │    │   │║
║  │   │  │ 0 7     ← Output 1 (G1) conectar a input 8 (CAM8)              │    │   │║
║  │   │  │ 1 8     ← Output 2 (G2) conectar a input 9 (CAM9)              │    │   │║
║  │   │  │ 2 9     ← Output 3 (G3) conectar a input 10 (CAM10)            │    │   │║
║  │   │  │                                                                │    │   │║
║  │   │  └────────────────────────────────────────────────────────────────┘    │   │║
║  │   │                                                                         │   │║
║  │   │  Protocolo: TCP puerto 9990                                             │   │║
║  │   │  Latencia comando: <1ms                                                 │   │║
║  │   │                                                                         │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
║  ┌──────────────────────────────────────────────────────────────────────────────────┐║
║  │ RESULTADO FINAL: vMix ve el cambio instantáneo                                   │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │                                                                         │   │║
║  │   │  VideoHub cambia routing → vMix recibe nuevas fuentes SDI               │   │║
║  │   │                                                                         │   │║
║  │   │  Latencia total desde detección hasta cambio visible en vMix:           │   │║
║  │   │  • YOLO inference: ~5ms                                                 │   │║
║  │   │  • Tracking + Zone check: ~2ms                                          │   │║
║  │   │  • Event generation: <1ms                                               │   │║
║  │   │  • SceneManager decision: <1ms                                          │   │║
║  │   │  • VideoHub command: <1ms                                               │   │║
║  │   │  • SDI switch: instantáneo                                              │   │║
║  │   │  • vMix frame buffer: 33ms (1 frame @ 30fps)                            │   │║
║  │   │  ─────────────────────────────────────────                              │   │║
║  │   │  TOTAL: ~43ms (~1.3 frames de latencia)                                 │   │║
║  │   │                                                                         │   │║
║  │   └─────────────────────────────────────────────────────────────────────────┘   │║
║  └──────────────────────────────────────────────────────────────────────────────────┘║
║                                                                                       ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝

CONFIGURACIÓN JSON ACTUALIZADA
JSON
{
  "scene_manager": {
    "enabled": true,
    "mute_timeout_ms": 200,
    "mode": "auto",
    "trigger_mode": "event",  // NUEVO: "event" en lugar de "threshold"
    "event_cooldown_ms": 500, // NUEVO: Mínimo tiempo entre cambios
    "hysteresis_frames": 3,   // NUEVO: Frames de confirmación
    
    "manual_keys": {
      "toggle_mode": "M",
      "config_select": ["F1", "F6", "F7"],
      "group_select": "G",
      "camera_select": ["1", "2", "3", "4"]
    },
    
    "zones": {  // NUEVO: Definición de zonas para eventos
      "ZONE_START": {
        "range_m": [0, 25],
        "triggers_config": "config_a",
        "covered_by_ptz": [1]
      },
      "ZONE_MID": {
        "range_m": [25, 60],
        "triggers_config": "config_b",
        "covered_by_ptz": [2, 3]
      },
      "ZONE_FINISH": {
        "range_m": [60, 100],
        "triggers_config": "config_c",
        "covered_by_ptz": [4]
      }
    },
    
    "groups": {
      "config_a": {
        "g1_g4": [1, 2, 3, 12],
        "g5_g8": [4, 5, 6, 7],
        "description": "Initial config - cameras 1-3,12 in G1-G4, cameras 4-7 in G5-G8"
      },
      "config_b": {
        "g1_g4": [8, 9, 10, 12],
        "g5_g8": [4, 5, 6, 7],
        "description": "Mid-track config - cameras 8-10,12 in G1-G4"
      },
      "config_c": {
        "g1_g4": [8, 9, 10, 12],
        "g5_g8": [11, 1, 2, 3],
        "description": "Late-track config - rotates G5-G8 to finish line cameras"
      }
    }
  },
  
  "capture": {
    "enabled_channels": ["RADAR_01", "RADAR_02", "RADAR_03", "RADAR_04"],
    "ptz_resolution": "1920x1080",
    "ptz_framerate": 30,
    "streaming_slots_vmix_direct": true
  }
}