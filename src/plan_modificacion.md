📋 PLAN ACTUALIZADO: Sistema de Seguimiento con Triggers por Radar/Eventos
Con grupos originales preservados y disparo por detección
🎯 RESUMEN DEL SISTEMA
Arquitectura Final:

VIB captura solo 4 canales PTZ/RADAR (1080p@30fps) para seguimiento con YOLO
vMix captura directamente los 8 slots SDI del VideoHub (4K@30fps)
SceneManager cambia configuraciones basándose en eventos de radar/YOLO, no en thresholds fijos
Los grupos de cámaras se mantienen exactamente como están definidos
📊 ESPECIFICACIONES TÉCNICAS
Componente	Resolución	FPS	Formato
Cámaras Streaming (12)	3840×2160 (4K)	30p	SDI
Cámaras PTZ/Radar (4)	1920×1080 (HD)	30p	SDI
VideoHub Slots (G1-G8)	4K pass-through	30p	SDI
vMix Inputs	4K	30p	SDI directo
YOLO Inference	640×640	30fps	RGB
🎬 CONFIGURACIONES DE GRUPOS (SIN CAMBIOS)
Code
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                     CONFIGURACIONES DE GRUPOS PRESERVADAS                            │
└─────────────────────────────────────────────────────────────────────────────────────┘

╔═══════════════════════════════════════════════════════════════════════════════════════╗
║ CONFIG_A: Configuración Inicial (Salida/Primera Curva)                                ║
╠═══════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                       ║
║   G1-G4 (Grupo Principal):     G5-G8 (Grupo Secundario):                              ║
║   ┌─────┬─────┬─────┬─────┐   ┌─────┬─────┬─────┬─────┐                              ║
║   │ G1  │ G2  │ G3  │ G4  │   │ G5  │ G6  │ G7  │ G8  │                              ║
║   │CAM1 │CAM2 │CAM3 │CAM12│   │CAM4 │CAM5 │CAM6 │CAM7 │                              ║
║   └─────┴─────┴─────┴─────┘   └─────┴─────┴─────┴─────┘                              ║
║                                                                                       ║
║   Trigger: RADAR detecta líder saliendo de zona de salida                            ║
║                                                                                       ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝

╔═══════════════════════════════════════════════════════════════════════════════════════╗
║ CONFIG_B: Configuración Media (Mitad de Pista)                                        ║
╠═══════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                       ║
║   G1-G4 (Grupo Principal):     G5-G8 (Grupo Secundario):                              ║
║   ┌─────┬─────┬─────┬─────┐   ┌─────┬─────┬─────┬─────┐                              ║
║   │ G1  │ G2  │ G3  │ G4  │   │ G5  │ G6  │ G7  │ G8  │                              ║
║   │CAM8 │CAM9 │CAM10│CAM12│   │CAM4 │CAM5 │CAM6 │CAM7 │                              ║
║   └─────┴─────┴─────┴─────┘   └─────┴─────┴─────┴─────┘                              ║
║                                                                                       ║
║   Trigger: RADAR detecta líder entrando en sector medio                              ║
║                                                                                       ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝

╔═══════════════════════════════════════════════════════════════════════════════════════╗
║ CONFIG_C: Configuración Final (Llegada/Meta)                                          ║
╠═══════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                       ║
║   G1-G4 (Grupo Principal):     G5-G8 (Grupo Secundario):                              ║
║   ┌─────┬─────┬─────┬─────┐   ┌─────┬─────┬─────┬─────┐                              ║
║   │ G1  │ G2  │ G3  │ G4  │   │ G5  │ G6  │ G7  │ G8  │                              ║
║   │CAM8 │CAM9 │CAM10│CAM12│   │CAM11│CAM1 │CAM2 │CAM3 │                              ║
║   └─────┴─────┴─────┴─────┘   └─────┴─────┴─────┴─────┘                              ║
║                                                                                       ║
║   Trigger: RADAR detecta líder aproximándose a meta                                  ║
║                                                                                       ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝
📊 DIAGRAMA: ARQUITECTURA COMPLETA
Code
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                    ARQUITECTURA VIB - CAPTURA PTZ + TRIGGERS RADAR                   │
│                    (Todo a 30fps, PTZ a 1080p, Streaming a 4K)                       │
└─────────────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────────────────────┐
│                        CÁMARAS EN ESTADIO/PISTA                                     │
├────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                    │
│  ┌─────────────────────────────────────────────────────────────────────────────┐  │
│  │            12 CÁMARAS DE STREAMING (4K@30fps) - NO CAPTURADAS POR VIB       │  │
│  │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐                           │  │
│  │  │CAM1 │ │CAM2 │ │CAM3 │ │CAM4 │ │CAM5 │ │CAM6 │                           │  │
│  │  │ 4K  │ │ 4K  │ │ 4K  │ │ 4K  │ │ 4K  │ │ 4K  │                           │  │
│  │  │30fps│ │30fps│ │30fps│ │30fps│ │30fps│ │30fps│                           │  │
│  │  └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘                           │  │
│  │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐                           │  │
│  │  │CAM7 │ │CAM8 │ │CAM9 │ │CAM10│ │CAM11│ │CAM12│                           │  │
│  │  │ 4K  │ │ 4K  │ │ 4K  │ │ 4K  │ │ 4K  │ │ 4K  │  (Cámara fija especial)  │  │
│  │  │30fps│ │30fps│ │30fps│ │30fps│ │30fps│ │30fps│                           │  │
│  │  └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘                           │  │
│  └─────┼──────┼──────┼──────┼──────┼──────┼──────────────────────────────────┘  │
│        │      │      │      │      │      │                                      │
│        │      │    SDI 4K directo al VideoHub                                    │
│        │      │      │      │      │      │                                      │
│  ┌─────┼──────┼──────┼──────┼──────┼──────┼──────────────────────────────────┐  │
│  │     │   4 CÁMARAS PTZ / RADAR (1080p@30fps) - CAPTURADAS POR VIB          │  │
│  │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐                                           │  │
│  │  │PTZ1 │ │PTZ2 │ │PTZ3 │ │PTZ4 │     ← Cámaras para tracking/detección    │  │
│  │  │RADAR│ │RADAR│ │RADAR│ │RADAR│     ← Cubren diferentes sectores          │  │
│  │  │1080p│ │1080p│ │1080p│ │1080p│        de la pista                        │  │
│  │  │30fps│ │30fps│ │30fps│ │30fps│                                           │  │
│  │  └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘                                           │  │
│  └─────┼──────┼──────┼──────┼────────────────────────────────────────────────┘  │
│        │      │      │      │                                                    │
└────────┼──────┼──────┼──────┼────────────────────────────────────────────────────┘
         │      │      │      │
         ▼      ▼      ▼      ▼
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                           VIDEOHUB 16×16 SDI MATRIX                                  │
├─────────────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────────────────────────────┐   │
│  │                              INPUTS (16 SDI)                                 │   │
│  │  ┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐│
│  │  │ 1  │ 2  │ 3  │ 4  │ 5  │ 6  │ 7  │ 8  │ 9  │ 10 │ 11 │ 12 │ 13 │ 14 │ 15 │ 16 ││
│  │  │CAM1│CAM2│CAM3│CAM4│CAM5│CAM6│CAM7│CAM8│CAM9│CA10│CA11│CA12│PTZ1│PTZ2│PTZ3│PTZ4││
│  │  │ 4K │ 4K │ 4K │ 4K │ 4K │ 4K │ 4K │ 4K │ 4K │ 4K │ 4K │ 4K │1080│1080│1080│1080││
│  │  └────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┘│
│  └─────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                     │
│                         SceneManager controla OUTPUTS 1-8                           │
│                         Basándose en EVENTOS DE RADAR                               │
│                                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────────────┐   │
│  │                             OUTPUTS (16 SDI)                                 │   │
│  │  ┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐│
│  │  │ 1  │ 2  │ 3  │ 4  │ 5  │ 6  │ 7  │ 8  │ 9  │ 10 │ 11 │ 12 │ 13 │ 14 │ 15 │ 16 ││
│  │  │ G1 │ G2 │ G3 │ G4 │ G5 │ G6 │ G7 │ G8 │RAD1│RAD2│RAD3│RAD4│    │    │    │    ││
│  │  │vMix│vMix│vMix│vMix│vMix│vMix│vMix│vMix│ VIB│ VIB│ VIB│ VIB│    │    │    │    ││
│  │  └──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴──┬─┴────┴────┴────┴────┘│
│  └─────┼────┼────┼────┼────┼────┼────┼────┼────┼────┼────┼────────────────────────┘   │
└────────┼────┼────┼────┼────┼────┼────┼────┼────┼────┼────┼────────────────────────────┘
         │    │    │    │    │    │    │    │    │    │    │
         ▼    ▼    ▼    ▼    ▼    ▼    ▼    ▼    ▼    ▼    ▼    ▼
┌──────────────────────────────────────┐    ┌───────────────────────────────────────┐
│         vMix (PC Producción)         │    │        VIB (PC con GPU RTX)           │
│                                      │    │                                       │
│  ┌────────────────────────────────┐  │    │  ┌─────────────────────────────────┐  │
│  │      DeckLink 8K Pro Mini      │  │    │  │    DeckLink Duo 2 (4 inputs)    │  │
│  │                                │  │    │  │                                 │  │
│  │  8 inputs SDI directo:         │  │    │  │  4 inputs SDI (solo PTZ):       │  │
│  │  ┌────┬────┬────┬────┐        │  │    │  │  ┌─────┬─────┬─────┬─────┐      │  │
│  │  │ G1 │ G2 │ G3 │ G4 │        │  │    │  │  │RAD1 │RAD2 │RAD3 │RAD4 │      │  │
│  │  │ 4K │ 4K │ 4K │ 4K │        │  │    │  │  │1080p│1080p│1080p│1080p│      │  │
│  │  │30p │30p │30p │30p │        │  │    │  │  │ 30p │ 30p │ 30p │ 30p │      │  │
│  │  └────┴────┴────┴────┘        │  │    │  │  └─────┴─────┴─────┴─────┘      │  │
│  │  ┌────┬────┬────┬────┐        │  │    │  └─────────────────────────────────┘  │
│  │  │ G5 │ G6 │ G7 │ G8 │        │  │    │              │                        │
│  │  │ 4K │ 4K │ 4K │ 4K │        │  │    │              ▼                        │
│  │  │30p │30p │30p │30p │        │  │    │  ┌─────────────────────────────────┐  │
│  │  └────┴────┴────┴────┘        │  │    │  │      YOLO + Tracking Pipeline   │  │
│  └────────────────────────────────┘  │    │  │      Genera EVENTOS DE RADAR   │  │
│                                      │    │  └───────────┬─────────────────────┘  │
│  Producción en tiempo real:          │    │              │                        │
│  • MultiView 8 cámaras               │    │              ▼                        │
│  • Transiciones                      │    │  ┌─────────────────────────────────┐  │
│  • Recording/Streaming               │    │  │       SceneManager (Nuevo)      │  │
│                                      │    │  │  Dispara cambios por EVENTOS    │  │
└──────────────────────────────────────┘    │  │  no por thresholds de posición  │  │
                                            │  └───────────┬─────────────────────┘  │
                                            │              │                        │
                                            │              ▼                        │
                                            │  ┌─────────────────────────────────┐  │
                                            │  │       VideoHub Client           │  │
                                            │  │  Cambia routing G1-G8           │  │
                                            │  └─────────────────────────────────┘  │
                                            └───────────────────────────────────────┘
🔄 SISTEMA DE EVENTOS DE RADAR
Code
┌─────────────────────────────────────────────────────────────────────────────────────┐
│                     SISTEMA DE EVENTOS (Reemplaza Thresholds)                        │
└─────────────────────────────────────────────────────────────────────────────────────┘

╔═══════════════════════════════════════════════════════════════════════════════════════╗
║ TIPOS DE EVENTOS QUE DISPARAN CAMBIOS                                                 ║
╠═══════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                       ║
║  ┌────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ EVENTO: ZONE_ENTRY                                                              │  ║
║  │                                                                                 │  ║
║  │ Descripción: El líder entra en una zona específica de la pista                  │  ║
║  │ Fuente: YOLO detecta objeto cruzando línea virtual definida por calibración     │  ║
║  │                                                                                 │  ║
║  │ Zonas definidas:                                                                │  ║
║  │   • ZONE_START    (0-25m)   → Mantener config_a                                │  ║
║  │   • ZONE_MID      (25-60m)  → Cambiar a config_b                               │  ║
║  │   • ZONE_FINISH   (60-100m) → Cambiar a config_c                               │  ║
║  │                                                                                 │  ║
║  └────────────────────────────────────────────────────────────────────────────────┘  ║
║                                                                                       ║
║  ┌────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ EVENTO: LEADER_DETECTED                                                         │  ║
║  │                                                                                 │  ║
║  │ Descripción: Se identifica al objeto líder en una cámara PTZ específica         │  ║
║  │ Fuente: YOLO + Tracker identifica objeto más adelantado                         │  ║
║  │                                                                                 │  ║
║  │ Payload:                                                                        │  ║
║  │   • camera_id: PTZ que lo detecta                                              │  ║
║  │   • position: {x, y} en coordenadas de pista                                   │  ║
║  │   • confidence: nivel de certeza                                                │  ║
║  │   • velocity: velocidad estimada                                                │  ║
║  │                                                                                 │  ║
║  └────────────────────────────────────────────────────────────────────────────────┘  ║
║                                                                                       ║
║  ┌────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ EVENTO: EXTERNAL_TRIGGER                                                        │  ║
║  │                                                                                 │  ║
║  │ Descripción: Trigger manual o desde sistema externo (timing, scoring)           │  ║
║  │ Fuente: API REST, WebSocket, o tecla manual                                     │  ║
║  │                                                                                 │  ║
║  │ Casos de uso:                                                                   │  ║
║  │   • Sistema de cronometraje envía "LAP_START"                                  │  ║
║  │   • Operador presiona F1/F6/F7 para forzar config                              │  ║
║  │   • Sistema de scoring envía posiciones oficiales                              │  ║
║  │                                                                                 │  ║
║  └────────────────────────────────────────────────────────────────────────────────┘  ║
║                                                                                       ║
╚═══════════════════════════════════════════════════════════════════════════════════════╝
🔄 FLUJO COMPLETO DE CAPTURA A EVENTO
Code
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
║  │ PASO 3: CONVERSIÓN YUV→RGB640 (Kernel CUDA)                                      │║
║  │                                                                                  │║
║  │   ┌─────────────────────────────────────────────────────────────────────────┐   │║
║  │   │              CUDA Kernel: UYVY_1080p_to_RGB_640                         │   │║
║  │   │                                                                         │   │║
║  │   │  Operaciones fusionadas:                                                │   │║
║  │   │  1. Leer UYVY de memoria host-mapped                                    │   │║
║  │   │  2. Convertir YUV→RGB                                                   │   │║
║  │   │  3. Resize bilinear 1920×1080 → no rezise                                 │   │║
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
║  │   │  Input: Batch de 4 × [3,1080, 1920]                                │   │║
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
📝 CONFIGURACIÓN JSON ACTUALIZADA
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
📋 CHECKLIST DE IMPLEMENTACIÓN
Fase 1: Eliminar NDI y reducir captura (1-2 horas)
 Eliminar NDI Manager del código
 Cambiar captura a solo 4 canales PTZ (1080p@30fps)
 Actualizar constantes de resolución
Fase 2: Implementar ZoneChecker (2-3 horas)
 Crear clase ZoneChecker con definición de zonas
 Implementar calibración pixel→metros por PTZ
 Implementar detección de cruce de zona
Fase 3: Implementar EventGenerator (1-2 horas)
 Crear clase EventGenerator
 Definir estructura de eventos (ZONE_ENTRY, EXTERNAL_TRIGGER)
 Conectar con ZoneChecker y Tracker
Fase 4: Modificar SceneManager (2-3 horas)
 Cambiar lógica de threshold → event-driven
 Implementar histéresis y cooldown
 Mantener grupos exactamente como están definidos
 Agregar soporte para EXTERNAL_TRIGGER
Fase 5: Testing (2-3 horas)
 Test de captura 4 canales PTZ 1080p@30fps
 Test de detección YOLO
 Test de cruce de zonas
 Test de cambio de configuración por evento
 Test de latencia end-to-end