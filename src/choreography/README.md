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
| `ReplayCamera` | Seleccionar cámara del canal de reproducción | `camera` (1-8), `channel` (`A` a `D`, por defecto `A`) |
| `ReplayMarkInOut` | Crear evento con los últimos N segundos | `value` (segundos) |
| `ReplayExportLastEvent` | Exportar el último evento a archivo | `value` (carpeta, opcional) |
| `ReplayDeleteLastEvent` | Borrar el último evento del replay | - |
| `WaitForExport` | Esperar a que el clip exportado termine de escribirse | `folder` (opcional), `timeout` (ms), `stable` (ms) |

> **Nota sobre nombres de vMix:** vMix no tiene una función `ReplaySelectCamera`. El
> número de cámara forma parte del nombre y depende del canal de reproducción
> (`ReplayACamera4`, `ReplayBCamera4`, etc.). Por eso la coreografía usa `ReplayCamera`
> con `camera` y `channel` como argumentos, y el motor construye el comando correcto.
> También se aceptan como alias los nombres `ReplaySelectCamera4` y `ReplayCamera4`.

> **Formato del export (MP4/AVI):** `ReplayExportLastEvent` solo acepta la carpeta
> (`value`) y, opcionalmente, el canal; la API de vMix **no** tiene un parámetro de
> formato o calidad. vMix exporta con la calidad elegida en **Replay → Export Clips**:
> `vMix AVI`, `MP4 High Quality` o `MP4 Medium Quality`. Esa selección queda guardada
> en el `replay*.xml` de la sesión como `<lastExportQuality>`.
>
> Los archivos `stream-*.avi` que están en la carpeta de la sesión de replay
> (por ejemplo `D:\replay v1.1\stream-*.avi`) son las **grabaciones crudas** del
> replay (vMix AVI), no los clips exportados. Para obtener MP4 hay que usar el export
> con la calidad en `MP4 High Quality`. Si el export sale AVI, cambiar esa calidad
> una vez en el diálogo de vMix; no se puede forzar desde la coreografía.

Ejemplo de exportación multicámara:

```json
{ "type": "ReplayCamera", "camera": 4, "channel": "A" },
{ "type": "ReplayMarkInOut", "value": 60 },
{ "type": "ReplayExportLastEvent" }
```

> **Exportar varias cámaras: `WaitForExport` + `ReplayDeleteLastEvent`.**
> `ReplayExportLastEvent` solo **inicia** la exportación: vMix codifica el clip en
> segundo plano (un MP4 4K de 60 s tarda ~30-40 s) y **descarta** cualquier export
> nuevo que llegue mientras el anterior sigue trabajando. Por eso, si se lanzan tres
> exports separados por un `Timer` corto, solo se guarda el primero.
>
> `ReplayExportLastEvent` exporta siempre el evento **más reciente por tiempo**, no
> el evento seleccionado. La secuencia correcta para varias cámaras es:
>
> 1. Crear todos los eventos `ReplayMarkInOut` seguidos, para que las ventanas de
>    N segundos queden alineadas (mismo momento de la carrera).
> 2. `ReplayExportLastEvent` (exporta el último = cámara creada al final).
> 3. `WaitForExport` (espera a que el archivo aparezca y deje de crecer).
> 4. `ReplayDeleteLastEvent` (quita ese evento, el anterior pasa a ser el último).
> 5. Repetir 2-4 para las cámaras restantes.
>
> `WaitForExport` vigila la carpeta indicada: parte de una foto de los archivos
> existentes, detecta el nuevo vídeo (`.mp4`, `.avi`, `.mov`, `.mkv`, `.mxf`) y
> espera a que su tamaño no cambie durante `stable` ms (por defecto 5000) antes de
> continuar. `timeout` (por defecto 300000 ms) evita quedarse colgado si el export
> falla. Si se omite `folder`, se usa la carpeta del último `ReplayExportLastEvent`
> que llevara `value`.
>
> Como el export se lanza sin `value`, vMix guarda el clip en su última carpeta de
> exportación (la del diálogo **Replay → Export Clips**). El `folder` de
> `WaitForExport` tiene que apuntar a esa misma carpeta; si se cambia la carpeta en
> vMix, hay que actualizarla también en el JSON.

> **Las ventanas de las cámaras no son exactamente iguales.** vMix procesa los
> comandos de replay en serie y cada `ReplayMarkInOut` tarda unos segundos, así que
> el evento de cada cámara cubre los N segundos contados desde el momento en que se
> procesó ese comando (quedan separados ~5 s). Crear todos los eventos antes de
> exportar minimiza la diferencia.

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

## Debug y Logging

El sistema de coreografía incluye logging detallado para facilitar la depuración:

### Habilitar Debug Mode

En `config.json`, establecer `"debug": true` en la sección choreography:

```json
"choreography": {
  "enabled": true,
  "script_path": "config/choreography/carrera_principal.json",
  "debug": true,
  ...
}
```

### Niveles de Log

- **INFO**: Carga de script, inicio/fin de ejecución, cambios de estado
- **DEBUG**: Parsing de eventos, tiempos de sleep, comandos vMix enviados
- **WARNING**: GUIDs vacíos, recursos no disponibles
- **ERROR**: Fallos de parsing, errores de comunicación

### Mensajes de Debug Típicos

```
[DEBUG] ChoreographyScript: Loading script from file: config/choreography/carrera_principal.json
[DEBUG] ChoreographyScript: Parsed Timer(ms=2000)
[DEBUG] ChoreographyEngine: Execution thread started, 70 events to process
[DEBUG] ChoreographyEngine: PreciseSleep starting for 2000ms
[DEBUG] ChoreographyEngine: vMix command sent successfully: FUNCTION CutDirect Input=...
```

## Troubleshooting

### El script no carga
- Verificar que el archivo existe y es JSON/DSL válido
- Revisar logs para errores de parsing
- Habilitar `"debug": true` para ver detalles de parsing

### Comandos vMix no funcionan
- Verificar que vMix TCP está conectado (`vmixController.ConnectTcp()`)
- Verificar GUIDs de inputs
- Revisar logs DEBUG para ver comandos enviados

### NDISlotChange no cambia la cámara
- Verificar conexión con VideoHub
- Verificar que el slot y cameraID están en rango válido (0-7, 1-12)
- Revisar logs para errores de routing

### Timing impreciso
- Habilitar debug para ver tiempos de sleep vs reales
- Verificar carga de CPU del sistema
- Los primeros 5ms usan spin-wait para precisión

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
