# Plan: Opción 8 — Modo Pista Automatizada

**Proyecto:** `C:\vs\proyectos\seguimientoYolo7LMDS`
**Referencia:** `C:\vs\proyectos\seguimientoYolo7` (intacto)
**Producción:** `D:\production\programas\controller`

---

## Contexto

La pista física mide ~1.8m en línea recta (~1.2m recorrido efectivo). Tiene una zona de salida en un extremo y llegada en el otro. Actualmente se usa la **opción 2** (Running Mode) para grabar video y la **opción 7** (vMix UTC Choreography) para post-producir animaciones de posiciones. El tracking existe pero no funciona bien.

Se necesita una **nueva opción 8** que automatice completamente la pista usando una sola cámara PTZ. Las prioridades son:

| Prioridad | Requisito |
|-----------|-----------|
| **P1** | Check de esferas en la salida funcione 100% |
| **P2** | Check de esferas + cálculo de posiciones en llegada funcione 100% |
| **P3** | Seguimiento continuo entre salida y llegada (etapa posterior) |

### Hardware disponible

- **1 cámara PTZ** conectada al VideoHub: **entrada 29, salida 13**
- **Preset 1**: Vista amplia de todo el tramo (salida + llegada)
- **Preset 2**: Vista cercana de la salida
- **Preset 3**: Vista cercana de la llegada
- **ESP32**: Control de compuerta y ascensor
- **vMix**: Grabación y producción

---

## Enfoque: Minimalista, componiendo módulos existentes

En lugar de crear clases nuevas complejas, se sigue el patrón de `RunVMixUTCChoreography` (~100 líneas en `main.cpp`): inicializar controladores, crear `ChoreographyEngine`, cargar script JSON, ejecutar con monitoreo.

**No se necesita:**
- `ActiveCameraSelector` (solo 1 cámara)
- `SceneManager` completo (no hay zonas múltiples)
- `ZoneChecker` (pista muy corta)
- `BallTracker`/`RouteMapper` (P3, después)
- 12 DeckLink captures (solo 1)

---

## Archivos a modificar

### 1. `src/ui/UserMenu.h` — Agregar enum

```cpp
enum class MenuOption {
    // ... existentes ...
    VMIX_UTC_CHOREOGRAPHY = 7,
    AUTOMATED_TRACK = 8,      // <-- NUEVO
    INVALID = -1
};
```

### 2. `src/ui/UserMenu.cpp` — Agregar display y routing

**`PrintMenuOptions()`**: Agregar después de opción 7:
```
  |   [8] MODO PISTA AUTOMATIZADA                                     |
  |       Control automatizado de pista fisica (1 camara PTZ)         |
  |       - Verifica esferas en salida y llegada                      |
  |       - Controla compuertas y ascensor (ESP32)                    |
  |       - Grabacion automatica con vMix                             |
```

**`GetUserChoice()`**: Cambiar prompt a `"(1-8): "`

**`IntToMenuOption()`**: Agregar `case 8: return MenuOption::AUTOMATED_TRACK;`

### 3. `src/core/main.cpp` — Nueva función `RunAutomatedTrackMode()`

Función de ~150 líneas que sigue el patrón de `RunVMixUTCChoreography`:

```
RunAutomatedTrackMode():
  1. Cargar config.json
  2. Conectar VideoHub → rutear entrada 29 a salida 13
  3. Inicializar PTZController (1 cámara, índice 0)
  4. Inicializar TrackPhysicalController (ESP32)
  5. Inicializar VMixController (opcional)
  6. Inicializar InferenceEngine (puede usar stub si no hay .engine)
  7. Inicializar SphereVerifier
  8. Crear ChoreographyEngine, wirear todos los controladores
  9. Cargar script pista_automatizada.json
  10. Mostrar resumen y pedir ENTER para iniciar
  11. Ejecutar engine.Start() + loop de monitoreo (ESC para abortar)
  12. Reportar resultado
```

Agregar el case en `main()`:
```cpp
case MenuOption::AUTOMATED_TRACK:
    if (!RunAutomatedTrackMode()) {
        Logger::Warning("Modo Pista Automatizada finalizo con advertencias");
    }
    break;
```

### 4. `config/choreography/pista_automatizada.json` — Script de coreografía (NUEVO)

Define la secuencia completa como eventos del `ChoreographyEngine`:

| Fase | Eventos | Descripción |
|------|---------|-------------|
| **Setup** | PTZ preset 1, Timer 3s | Cámara a vista amplia |
| **Verif. salida** | SpherePresenceCheck (cam 29, 10 esferas, 15s timeout) | Confirma que las 10 esferas están en la salida |
| **Captura inicial** | PTZ preset 2, Timer 2s, SpherePositionCapture | Closeup salida, captura posiciones de partida |
| **Largada** | StartRecording, ESP32 "iniciar", Timer 1s | Inicia grabación, abre compuerta |
| **Espera llegada** | PTZ preset 3, Timer 2s, SphereArrivalWait (120s timeout) | Closeup llegada, espera las 10 esferas |
| **Cierre** | ESP32 "finalizar", Timer 2s, SpherePositionCapture | Cierra compuertas, captura posiciones finales |
| **Limpieza** | StopRecording, ESP32 "recargar", PTZ preset 1 | Detiene grabación, abre ascensor, cámara a wide |

### 5. `src/config.json` y `D:\production\programas\controller\config.json` — Actualizar

Agregar sección `automated_track`:
```json
"automated_track": {
    "enabled": true,
    "script_path": "config/choreography/pista_automatizada.json",
    "ptz_camera_index": 0,
    "videohub_input": 29,
    "videohub_output": 13,
    "expected_spheres": 10,
    "presence_timeout_ms": 15000,
    "arrival_timeout_ms": 120000,
    "ptz_settle_ms": 2000
}
```

### 6. `src/core/Config.h` — Struct de configuración (opcional)

Agregar `AutomatedTrackConfig` con defaults para todos los campos anteriores.

---

## Flujo de estados

```
IDLE → INIT (conectar HW, rutear VH, verificar señal)
  → READY (mostrar resumen, esperar ENTER)
    → RUNNING (ChoreographyEngine ejecutando el script)
      → COMPLETED (carrera exitosa)
      → ERROR (fallo en verificación, timeout, o ESC)
```

---

## Manejo de errores

| Capa | Qué cubre | Acción |
|------|-----------|--------|
| Pre-flight | VideoHub no conecta, PTZ no disponible | Abortar con mensaje claro |
| Pre-flight | InferenceEngine en stub mode | Advertir pero continuar |
| Pre-flight | ESP32 no responde | Advertir pero continuar |
| Ejecución | SpherePresenceCheck falla | Abortar — no hay carrera sin esferas |
| Ejecución | SphereArrivalWait timeout | Flag "incompleta", continuar cleanup |
| Ejecución | ESP32 command falla | Log error, continuar |
| Usuario | ESC durante ejecución | Parar engine, cerrar compuertas, cleanup |

---

## Punto crítico: `SphereVerifier::CaptureAndDetect`

El agente de exploración detectó que `CaptureAndDetect()` tiene un TODO y no captura frames reales cuando no está en stub mode. Esto **debe resolverse** para que la verificación de esferas funcione en producción.

**Enfoque propuesto:** En `RunAutomatedTrackMode()`, crear un solo `DeckLinkCapture` para la entrada de VideoHub correspondiente. Inyectar un callback de frame en `SphereVerifier` para que `CaptureAndDetect()` use frames reales del DeckLink en lugar del stub.

```cpp
// En RunAutomatedTrackMode():
DeckLinkCapture capture;
capture.Initialize(/* puerto correspondiente a VH output 13 */);
capture.SetFrameReadyHandler([&](void* frameData, int width, int height) {
    sphereVerifier.SetLastFrame(frameData, width, height);
});
capture.Start();
```

Esto requerirá un cambio mínimo en `SphereVerifier`:
```cpp
// Nuevo método en SphereVerifier:
void SetLastFrame(void* frameData, int width, int height);
// Modificar CaptureAndDetect() para usar m_lastFrame si está disponible
```

---

## Plan de implementación (orden)

| # | Archivo | Cambio |
|---|---------|--------|
| 1 | `src/ui/UserMenu.h` | Agregar `AUTOMATED_TRACK = 8` |
| 2 | `src/ui/UserMenu.cpp` | Display opción 8, prompt (1-8), routing case 8 |
| 3 | `config/choreography/pista_automatizada.json` | Crear script de coreografía |
| 4 | `src/core/main.cpp` | Agregar `RunAutomatedTrackMode()` + case en `main()` |
| 5 | `src/core/Config.h` | Agregar `AutomatedTrackConfig` (leve) |
| 6 | `src/config.json` | Agregar sección `automated_track` |
| 7 | `src/verification/SphereVerifier.h/.cpp` | Agregar `SetLastFrame()`, fix `CaptureAndDetect()` |
| 8 | `D:\production\programas\controller\config.json` | Misma sección `automated_track` en producción |

---

## Verificación

1. **Compilar** en `seguimientoYolo7LMDS/src/` con VS2022 → `VIB.exe`
2. **Probar con stub** (sin TensorRT engine): opción 8 debe ejecutar la secuencia completa con esferas simuladas
3. **Probar en producción**: Copiar `VIB.exe`, `pista_automatizada.json` y `config.json` actualizado a `D:\production\programas\controller\`
4. **P1**: Colocar 10 esferas en salida, ejecutar opción 8, verificar que confirma presencia
5. **P2**: Ejecutar carrera completa, verificar que detecta llegada y captura posiciones
