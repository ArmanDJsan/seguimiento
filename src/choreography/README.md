# Choreography Engine - Sistema de Automatización vMix

## Descripción General

El **ChoreographyEngine** es un motor de automatización que permite ejecutar secuencias programadas de comandos vMix y cambios de routing NDI. Está diseñado específicamente para manejar **12 cámaras de streaming** a través de **8 slots NDI**, dejando 4 slots disponibles para seguimiento.

## Arquitectura

```
┌─────────────────────────────────────────────────────────────┐
│                    ChoreographyEngine                       │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐       │
│  │   Script    │   │   Events    │   │  Execution  │       │
│  │   Parser    │   │   Queue     │   │   Thread    │       │
│  └─────┬───────┘   └──────┬──────┘   └──────┬──────┘       │
│        │                  │                  │              │
│        └──────────────────┼──────────────────┘              │
│                           │                                 │
│            ┌──────────────┼──────────────┐                 │
│            ▼              ▼              ▼                  │
│    ┌─────────────┐ ┌─────────────┐ ┌─────────────┐        │
│    │ VMixController │ │ SceneManager │ │ VideoHub   │        │
│    │ (TCP 8099)  │ │ (8 slots)  │ │ (Routing)  │        │
│    └─────────────┘ └─────────────┘ └─────────────┘        │
└─────────────────────────────────────────────────────────────┘
```

## Estrategia: 12 Cámaras → 8 Slots NDI

```
                    SLOTS NDI (8 para streaming)
    ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐
    │ G1   │ G2   │ G3   │ G4   │ G5   │ G6   │ G7   │ G8   │
    │Slot 0│Slot 1│Slot 2│Slot 3│Slot 4│Slot 5│Slot 6│Slot 7│
    └──┬───┴──┬───┴──┬───┴──┬───┴──┬───┴──┬───┴──┬───┴──┬───┘
       │      │      │      │      │      │      │      │
       └──────┴──────┴──────┼──────┴──────┴──────┴──────┘
                            │
                    VideoHub Routing
                            │
       ┌──────┬──────┬──────┼──────┬──────┬──────┬──────┬──────┐
       │CAM_01│CAM_02│CAM_03│CAM_04│CAM_05│CAM_06│CAM_07│CAM_08│
       └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘
       ┌──────┬──────┬──────┬──────┐
       │CAM_09│CAM_10│CAM_11│CAM_12│
       └──────┴──────┴──────┴──────┘
              12 Cámaras Físicas
```

La coreografía puede cambiar dinámicamente qué cámara física está conectada a qué slot NDI durante la transmisión.

## Eventos Soportados

### Comandos vMix

| Evento | Descripción | Parámetros |
|--------|-------------|------------|
| `CutDirect` | Corte directo a input | `guid`, `layer` (opcional) |
| `QuickPlay` | QuickPlay input | `guid` |
| `Play` | Reproducir input | `guid` |
| `Pause` | Pausar input | `guid` |
| `Restart` | Reiniciar input | `guid` |
| `AudioOn` | Activar audio | `guid` |
| `AudioOff` | Desactivar audio | `guid` |
| `StartRecording` | Iniciar grabación | - |
| `StopRecording` | Detener grabación | - |
| `BrowserReload` | Recargar browser | `guid` |

### Comandos de Overlay

| Evento | Descripción | Parámetros |
|--------|-------------|------------|
| `OverlayInputIn` | Mostrar overlay | `guid`, `layer` (1-4) |
| `OverlayInputOut` | Ocultar overlay | `layer` (1-4) |

### Comandos de Replay

| Evento | Descripción | Parámetros |
|--------|-------------|------------|
| `ReplayStartRecording` | Iniciar grabación replay | - |
| `ReplayStopRecording` | Detener grabación replay | - |
| `ReplayMarkIn` | Marcar punto entrada | - |
| `ReplayMarkOut` | Marcar punto salida | - |
| `ReplayLive` | Cambiar a live | - |
| `ReplaySetSpeed` | Velocidad replay | `speed` (0-200%) |
| `ReplaySelectLastEvent` | Seleccionar último evento | - |

### Comandos Internos/Custom

| Evento | Descripción | Parámetros |
|--------|-------------|------------|
| `Timer` | Espera en ms | `ms` |
| `NDISlotChange` | Cambiar cámara en slot | `slot` (0-7), `camera` (1-12) |
| `SceneSwitch` | Cambiar config escena | `config` (índice) |
| `Comment` | Comentario (no-op) | `text` |

## Formatos de Script

### Formato JSON (Recomendado)

```json
{
  "name": "Mi Coreografía",
  "description": "Descripción opcional",
  "version": "1.0",
  "events": [
    { "type": "CutDirect", "guid": "fd442a6a-7ba8-4393-a650-90f7e1295664", "layer": -1 },
    { "type": "Timer", "ms": 2000 },
    { "type": "NDISlotChange", "slot": 0, "camera": 9 },
    { "type": "QuickPlay", "guid": "a273aa8e-d54b-42b6-a8bc-b5d46414a117" }
  ]
}
```

### Formato DSL (Compatible con formato original)

```
*CutDirect(fd442a6a-7ba8-4393-a650-90f7e1295664, -1)
*Timer(2000)
*NDISlotChange(0, 9)
*QuickPlay(a273aa8e-d54b-42b6-a8bc-b5d46414a117)
```

## Uso en C++

### Inicialización Básica

```cpp
#include "choreography/ChoreographyEngine.h"

// Crear el engine con dependencias
ChoreographyEngine choreography(&vmixController, &sceneManager);

// Configurar opciones
choreography.SetContinueOnError(true);
choreography.SetVMixRequired(false);

// Cargar script
if (choreography.Load("config/choreography/carrera_principal.json")) {
    // Iniciar ejecución
    choreography.Start();
}
```

### Callbacks para Monitoreo

```cpp
// Estado del engine
choreography.SetStateCallback([](EngineState state) {
    Logger::Info("Choreography state: " + std::to_string((int)state));
});

// Inicio de evento
choreography.SetEventStartCallback([](size_t index, const ChoreographyEvent& event) {
    Logger::Info("Executing event " + std::to_string(index) + ": " + event.GetTypeName());
});

// Evento completado
choreography.SetEventCompleteCallback([](const EventResult& result) {
    if (!result.success) {
        Logger::Error("Event " + std::to_string(result.eventIndex) + " failed");
    }
});
```

### Control de Ejecución

```cpp
// Iniciar
choreography.Start();

// Pausar
choreography.Pause();

// Reanudar
choreography.Resume();

// Saltar evento actual
choreography.Skip();

// Detener
choreography.Stop();

// Estado actual
auto status = choreography.GetStatus();
Logger::Info("Progress: " + std::to_string(status.currentEventIndex) + 
             "/" + std::to_string(status.totalEvents));
```

### Ejecución de Eventos Individuales

```cpp
// Ejecutar un solo evento
auto result = choreography.ExecuteEvent(ChoreographyEvent::CutDirect("guid-here", -1));

// O crear eventos programáticamente
std::vector<ChoreographyEvent> events;
events.push_back(ChoreographyEvent::Timer(1000));
events.push_back(ChoreographyEvent::NDISlotChange(0, 9));
events.push_back(ChoreographyEvent::CutDirect("input-guid", -1));

Script script;
script.metadata.name = "Runtime Script";
script.events = events;
choreography.SetScript(script);
choreography.Start();
```

## Configuración en config.json

```json
{
  "choreography": {
    "enabled": true,
    "script_path": "config/choreography/carrera_principal.json",
    "auto_start": false,
    "continue_on_error": true,
    "vmix_required": false,
    "trigger_key": "F12",
    "description": "vMix choreography automation"
  }
}
```

## Integración con SceneManager

El `NDISlotChange` puede usar directamente el `VideoHubClient` o integrarse con `SceneManager` para mantener sincronizado el estado de los slots:

```cpp
// Usar SceneManager para cambios de routing coordinados
choreography.SetSceneManager(&sceneManager);

// O usar VideoHub directamente
choreography.SetVideoHubClient(&videoHubClient);
```

## Ejemplos de Scripts

### Carrera Principal (`carrera_principal.json`)

Script completo con la secuencia original de vMix incluyendo:
- CutDirect para cambios de cámara
- Recording/Replay
- Overlays
- Timers

### Demo NDI Slots (`demo_ndi_slots.json`)

Demostración de rotación de cámaras:
- Configuración inicial de 8 cámaras
- Rotación de slots 1-4 con cámaras 9-12
- Uso de SceneSwitch para cambios de configuración completa

## Rendimiento

- **High-Resolution Timer**: Usa `std::chrono::high_resolution_clock` para precisión sub-ms
- **Thread Dedicado**: La ejecución corre en thread separado para no bloquear video
- **Spin-Wait Híbrido**: Sleep para la mayoría del tiempo, spin-wait para los últimos ms

## Troubleshooting

### El script no carga
- Verificar que el archivo existe y es JSON/DSL válido
- Revisar logs para errores de parsing

### Comandos vMix no funcionan
- Verificar que vMix TCP está conectado (`vmixController.ConnectTcp()`)
- Verificar GUIDs de inputs

### NDISlotChange no cambia la cámara
- Verificar conexión con VideoHub
- Verificar que el slot y cameraID están en rango válido (0-7, 1-12)

## Archivos del Módulo

```
src/choreography/
├── ChoreographyEvent.h      # Definición de eventos
├── ChoreographyScript.h     # Parser de scripts
├── ChoreographyScript.cpp
├── ChoreographyEngine.h     # Motor de ejecución
├── ChoreographyEngine.cpp
└── README.md               # Esta documentación

config/choreography/
├── carrera_principal.json   # Script de carrera completa
└── demo_ndi_slots.json     # Demo de rotación de cámaras
```
