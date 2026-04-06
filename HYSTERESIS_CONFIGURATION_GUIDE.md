# Configuración de Hysteresis - Guía de Referencia

## Descripción

El sistema de hysteresis evita el "flickering" (oscilación rápida) entre cámaras cuando varias tienen scores de movimiento similares. Esta configuración permite ajustar el comportamiento sin recompilar.

---

## Parámetros en config.json

```json
{
  "detection_optimization": {
    "hysteresis": {
      "switch_threshold": 0.20,      // Umbral de cambio (ver explicación abajo)
      "min_active_frames": 15,        // Mínimo de frames activa
      "decay_factor": 0.95            // Factor de decaimiento
    }
  }
}
```

---

## Parámetro 1: switch_threshold

**Valor por defecto**: `0.20` (20%)

**Descripción**: Porcentaje adicional de score que debe tener una cámara nueva para reemplazar a la cámara activa actual.

**Rango válido**: `0.10` - `0.40`

**Ejemplos**:

| Valor | Significado | Cuándo usar |
|-------|-------------|-------------|
| `0.10` | Muy sensible (10% más) | Cámaras con ángulos muy similares, acción rápida |
| `0.15` | Moderadamente sensible | Múltiples cámaras con FOV similar |
| **`0.20`** | **Balanceado (DEFAULT)** | **Caso general, recomendado** |
| `0.25` | Conservador | Cámaras con ángulos/lentes muy diferentes |
| `0.30` | Muy conservador | Evitar cambios excepto con diferencia muy clara |

**Ejemplo de cálculo**:
```
Cámara activa: score = 0.50
switch_threshold = 0.20

Nueva cámara necesita: 0.50 × (1 + 0.20) = 0.60
Solo si nueva_score > 0.60, se hace el cambio
```

---

## Parámetro 2: min_active_frames

**Valor por defecto**: `15` (500ms @ 30fps)

**Descripción**: Número mínimo de frames que una cámara debe permanecer activa antes de poder ser reemplazada.

**Rango válido**: `5` - `45`

**Ejemplos**:

| Valor | Tiempo @ 30fps | Cuándo usar |
|-------|----------------|-------------|
| `5` | 167ms | Deportes muy rápidos (tenis, ping-pong) |
| `10` | 333ms | Acción rápida (fútbol, básquetbol) |
| **`15`** | **500ms (DEFAULT)** | **Caso general, carreras** |
| `20` | 667ms | Acción moderada (golf, atletismo) |
| `30` | 1000ms | Escenas estáticas o lentas |
| `45` | 1500ms | Máxima estabilidad, cambios muy conservadores |

**Nota**: Valores muy bajos (<10) pueden causar flickering. Valores muy altos (>30) pueden hacer que el sistema sea lento para reaccionar.

---

## Parámetro 3: decay_factor

**Valor por defecto**: `0.95` (5% de decay por frame)

**Descripción**: Factor de decaimiento aplicado a los scores de cámaras inactivas cada frame. Evita que una cámara que acaba de ser desactivada vuelva a activarse inmediatamente.

**Rango válido**: `0.85` - `0.98`

**Ejemplos**:

| Valor | Decay por frame | Cuándo usar |
|-------|-----------------|-------------|
| `0.85` | 15% decay | Olvidar rápido (cambios frecuentes) |
| `0.90` | 10% decay | Decay acelerado |
| **`0.95`** | **5% decay (DEFAULT)** | **Balanceado** |
| `0.97` | 3% decay | Decay suave |
| `0.98` | 2% decay | Memoria larga (evita re-activación) |

**Ejemplo de decaimiento**:

```
Frame 0: Cámara desactivada, score = 0.60
Frame 1: score_decayed = 0.60 × 0.95 = 0.57
Frame 2: score_decayed = 0.57 × 0.95 = 0.54
Frame 3: score_decayed = 0.54 × 0.95 = 0.51
...
```

Después de ~14 frames con `decay_factor=0.95`, el score habrá caído a la mitad.

---

## Configuraciones Recomendadas por Escenario

### Escenario 1: Pista de Carreras (DEFAULT)

```json
{
  "detection_optimization": {
    "hysteresis": {
      "switch_threshold": 0.20,
      "min_active_frames": 15,
      "decay_factor": 0.95
    }
  }
}
```

**Razón**: Balanceado para acción moderada con cámaras de pista fijas.

---

### Escenario 2: Ángulos de Cámara Muy Diferentes

```json
{
  "detection_optimization": {
    "hysteresis": {
      "switch_threshold": 0.25,      // Más conservador
      "min_active_frames": 20,        // Más tiempo activa
      "decay_factor": 0.96            // Decay más lento
    }
  }
}
```

**Razón**: Evita cambios frecuentes cuando cámaras tienen diferentes focales o ángulos.

---

### Escenario 3: Acción Rápida (Deportes)

```json
{
  "detection_optimization": {
    "hysteresis": {
      "switch_threshold": 0.15,      // Más sensible
      "min_active_frames": 10,        // Menos tiempo activa
      "decay_factor": 0.92            // Decay más rápido
    }
  }
}
```

**Razón**: Respuesta rápida a cambios de acción para deportes veloces.

---

### Escenario 4: Máxima Estabilidad (Escenas Lentas)

```json
{
  "detection_optimization": {
    "hysteresis": {
      "switch_threshold": 0.30,      // Muy conservador
      "min_active_frames": 30,        // Mucho tiempo activa
      "decay_factor": 0.97            // Decay muy lento
    }
  }
}
```

**Razón**: Minimiza cambios de cámara, ideal para escenas lentas o estáticas.

---

## Calibración en Campo

### Paso 1: Monitoreo Inicial

Ejecutar con valores por defecto y observar logs:

```
[PERF] Cap:2.1ms Sel:0.4ms YOLO:12.3ms NDI:0.8ms Redis:0.2ms Total:15.8ms Active:4/12
```

Observar también:
```
[STATUS] Active cameras: [3,7,9,11], Avg motion: 0.45
```

### Paso 2: Identificar Problemas

**Síntoma 1**: Cambios muy frecuentes (cada 1-2 segundos)
- **Solución**: Aumentar `switch_threshold` a 0.25-0.30
- **Solución alternativa**: Aumentar `min_active_frames` a 20-25

**Síntoma 2**: Sistema lento para reaccionar a nueva acción
- **Solución**: Reducir `switch_threshold` a 0.15
- **Solución alternativa**: Reducir `decay_factor` a 0.92-0.93

**Síntoma 3**: Oscilación entre 2 cámaras específicas
- **Solución**: Aumentar `decay_factor` a 0.97
- **Solución alternativa**: Aumentar `min_active_frames` a 20

### Paso 3: Ajuste Incremental

1. Modificar `config.json` con nuevos valores
2. Reiniciar VIB (lectura de config solo al inicio)
3. Observar comportamiento por 5 minutos
4. Repetir hasta óptimo

### Paso 4: Validación

Criterios de éxito:
- ✅ No hay cambios de cámara en <500ms (excepto acción clara)
- ✅ Sistema responde a nueva acción en <1 segundo
- ✅ No oscilación entre 2 cámaras con scores similares
- ✅ Logs muestran cambios suaves y justificados

---

## Validación de Valores

El sistema valida automáticamente los valores al cargar:

**Validaciones automáticas**:
- `switch_threshold`: Si está fuera de rango [0.10, 0.40], se usa 0.20 (default)
- `min_active_frames`: Si está fuera de rango [5, 45], se usa 15 (default)
- `decay_factor`: Si está fuera de rango [0.85, 0.98], se usa 0.95 (default)

**Logs de validación**:
```
ActiveCameraSelector initialized: Top-4 from 12 cameras with hysteresis (threshold=0.20, frames=15, decay=0.95)
```

Si hay un error de parsing, se usarán los valores por defecto:
```
Warning: Error al parsear detection_optimization; usando valores por defecto
```

---

## Troubleshooting

### Problema: Config.json no se lee

**Síntomas**: Siempre usa valores por defecto (0.20, 15, 0.95)

**Soluciones**:
1. Verificar que `config.json` esté en el directorio de ejecución
2. Verificar sintaxis JSON válida (usar JSONLint.com)
3. Verificar que la sección `detection_optimization` esté correctamente anidada

### Problema: Cambios no tienen efecto

**Causa**: Config solo se lee al inicio

**Solución**: Reiniciar VIB completamente después de modificar config.json

### Problema: Valores fuera de rango

**Síntoma**: Log muestra valores por defecto aunque config.json tenga otros

**Causa**: Valores fuera de rangos válidos

**Solución**: Verificar que:
- `switch_threshold`: 0.10 ≤ valor ≤ 0.40
- `min_active_frames`: 5 ≤ valor ≤ 45
- `decay_factor`: 0.85 ≤ valor ≤ 0.98

---

## Ejemplo Completo config.json

```json
{
  "videohub": {
    "ip": "192.168.1.50",
    "port": 9990
  },
  "esp32": {
    "ip": "192.168.88.114",
    "port": 80,
    "endpoints": {
      "test": "/test",
      "iniciar": "/iniciar",
      "cerrar": "/cerrar",
      "recargar": "/recargar"
    }
  },
  "redis": {
    "enabled": true,
    "host": "127.0.0.1",
    "port": 6379
  },
  "yolo": {
    "enabled": true,
    "model_path": "models/yolov8n.engine",
    "fallback": "stub",
    "batch_size": 4,
    "confidence_threshold": 0.5,
    "nms_threshold": 0.4,
    "use_fp16": true
  },
  "camera_selector": {
    "enabled": true,
    "top_k": 4,
    "motion_threshold": 0.05,
    "edge_handover_margin": 0.1
  },
  "detection_optimization": {
    "hysteresis": {
      "switch_threshold": 0.20,
      "min_active_frames": 15,
      "decay_factor": 0.95
    }
  },
  "ports": [
    { "index": 1, "name": "CAM_01", "role": "stream", "is_output": true },
    { "index": 2, "name": "CAM_02", "role": "stream" },
    { "index": 3, "name": "CAM_03", "role": "stream" },
    { "index": 4, "name": "CAM_04", "role": "stream" },
    { "index": 5, "name": "CAM_05", "role": "stream" },
    { "index": 6, "name": "CAM_06", "role": "stream" },
    { "index": 7, "name": "CAM_07", "role": "stream" },
    { "index": 8, "name": "CAM_08", "role": "stream" },
    { "index": 9, "name": "CAM_09", "role": "stream" },
    { "index": 10, "name": "CAM_10", "role": "stream" },
    { "index": 11, "name": "CAM_11", "role": "stream" },
    { "index": 12, "name": "CAM_12", "role": "stream" },
    { "index": 13, "name": "RADAR_01", "role": "radar" },
    { "index": 14, "name": "RADAR_02", "role": "radar" },
    { "index": 15, "name": "RADAR_03", "role": "radar" },
    { "index": 16, "name": "RADAR_04", "role": "radar" }
  ],
  "target_spheres": 10
}
```

---

**Configuración Flexible**: Ajustar en caliente sin recompilar ✅  
**Listo para Calibración en Campo**: Mañana en hardware real ✅
