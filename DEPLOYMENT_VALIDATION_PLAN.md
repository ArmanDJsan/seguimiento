# PLAN DE DESPLIEGUE Y VALIDACIÓN - HITO 2 (Pre-Hardware Real)

**Fecha de Pruebas**: Mañana  
**Hardware**: RTX 5080 16GB + 3x DeckLink 8K Pro Mini (12 cámaras 4K30)  
**Versión**: VIB v2.0 HITO 2

---

## TAREA 1: GOLDEN METRICS Y UMBRALES

### Tabla de Métricas Esperadas (Hardware Real RTX 5080)

| Métrica | Valor Esperado (ms) | Alerta Amarilla (ms) | Alerta Roja (ms) | Notas |
|---------|---------------------|----------------------|------------------|-------|
| **capture_ms** | 0.3-0.5 | >0.8 | >1.2 | DeckLink DMA + frame ready callback overhead |
| **selector_ms** | 0.3-0.5 | >0.8 | >1.5 | CUDA motion detection (12 cams, 4K) |
| **yolo_ms (batch 4)** | 10-14 | >18 | >25 | TensorRT FP16 on RTX 5080 Tensor Cores |
| **ndi_ms** | 0.6-0.9 | >1.5 | >2.5 | GPU→CPU pinned + NDI async send |
| **redis_ms** | 0.1-0.3 | >0.5 | >1.0 | Async worker thread publish |
| **Total** | **12-17** | **>28** | **>33** | **CRÍTICO**: >33ms = frame drop @ 30fps |

### Métricas de Hardware (RTX 5080)

| Métrica | Rango Normal | Alerta Amarilla | Alerta Roja | Comando de Verificación |
|---------|--------------|-----------------|-------------|-------------------------|
| **GPU Utilization** | 40-65% | >75% | >85% | `nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits` |
| **VRAM Utilizada** | 1.6-2.0 GB | >3.0 GB | >4.0 GB | `nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits` (en MiB) |
| **Temperatura GPU** | 55-70°C | >78°C | >85°C | `nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits` |
| **GPU Clock** | 2400-2600 MHz | <2000 MHz | <1800 MHz | `nvidia-smi --query-gpu=clocks.gr --format=csv,noheader,nounits` |
| **Power Draw** | 180-250W | >300W | >350W | `nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits` |

### Desglose de VRAM (Total: ~1.6-1.65 GB)

| Componente | Memoria (MB) | % del Total | Detalles |
|------------|--------------|-------------|----------|
| **(a) 12 Texturas 4K** | **1,159.52** | **70.7%** | |
| - UYVY buffers (double) | 379.68 | 23.1% | 15.82 MB × 2 × 12 cámaras |
| - Motion detection maps | 400.16 | 24.4% | 33.18 MB × 12 cámaras |
| - BGRA conversion | 379.68 | 23.1% | 31.64 MB × 12 cámaras |
| **(b) YOLO Buffers** | **20.26** | **1.2%** | |
| - Input batch (FP16) | 9.38 | 0.6% | 640×640×3×2 bytes × 4 |
| - Output batch | 10.88 | 0.7% | 8400×85×4 bytes × 4 |
| **(c) TensorRT Engine** | **220** | **13.4%** | |
| - YOLOv8n weights (FP16) | 6.4 | 0.4% | 3.2M params × 2 bytes |
| - Workspace/activations | 213.6 | 13.0% | Internal TensorRT buffers |
| **(d) Overhead** | **240-290** | **14.6%** | |
| - NDI pinned memory | 189.84 | 11.6% | 15.82 MB × 12 channels |
| - CUDA runtime | 50-100 | 3.0-6.1% | Context, streams, modules |
| **TOTAL** | **~1,640-1,690** | **100%** | **10.3% de 16GB RTX 5080** |

---

## TAREA 2: SCRIPT DE STRESS TEST (15 MINUTOS)

### 2.1 Procedimiento de Carga de VRAM

**Objetivo**: Verificar que VRAM no satura y que NDI + TensorRT coexisten sin colisiones

```batch
@echo off
echo ========================================
echo VRAM SATURATION TEST
echo ========================================

echo [1/4] Baseline VRAM usage...
nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits > vram_baseline.txt
set /p BASELINE=<vram_baseline.txt
echo Baseline: %BASELINE% MiB

echo [2/4] Launching VIB application...
start "" "x64\Release\VIB.exe"
timeout /t 10 /nobreak

echo [3/4] Monitoring VRAM during operation...
for /L %%i in (1,1,30) do (
    nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits >> vram_load.txt
    timeout /t 1 /nobreak >nul
)

echo [4/4] Analyzing VRAM usage...
powershell -Command "(Get-Content vram_load.txt | Measure-Object -Average).Average" > vram_avg.txt
set /p AVG=<vram_avg.txt
echo Average VRAM: %AVG% MiB

echo Expected: ~1640-1690 MiB
echo Measured: %AVG% MiB

if %AVG% GTR 2500 (
    echo [FAIL] VRAM usage exceeds threshold!
    exit /b 1
) else (
    echo [PASS] VRAM usage within normal range
)

del vram_baseline.txt vram_load.txt vram_avg.txt
```

**Herramienta Alternativa**: NVIDIA Nsight Systems
```bash
# Capturar profiling completo de 30 segundos
nsys profile --duration=30 --trace=cuda,nvtx --output=vib_profile x64\Release\VIB.exe

# Analizar después con Nsight Systems GUI
```

### 2.2 Simulación de Fallos de Redis

**Test 1: Matar proceso Redis durante operación**

```batch
@echo off
echo ========================================
echo REDIS FAILURE SIMULATION
echo ========================================

echo [1/3] Starting VIB application...
start "" "x64\Release\VIB.exe"
timeout /t 15 /nobreak

echo [2/3] Killing Redis server...
taskkill /F /IM redis-server.exe
echo Redis server killed at %TIME%

echo [3/3] Monitoring VIB for 60 seconds...
echo Expected: VIB continues, logs "Redis disconnected", retries 5 times
timeout /t 60 /nobreak

echo [MANUAL CHECK] Verify in logs:
echo   - "Redis publish failed" messages
echo   - "Attempting reconnect" messages
echo   - Video pipeline CONTINUES (no crash)
echo   - NDI sources still visible in Studio Monitor
```

**Test 2: Bloqueo de red (firewall)**

```powershell
# PowerShell (ejecutar como Administrador)
Write-Host "Blocking Redis port 6379..."
New-NetFirewallRule -DisplayName "Block Redis Test" -Direction Outbound -LocalPort 6379 -Protocol TCP -Action Block

Write-Host "Wait 30 seconds..."
Start-Sleep -Seconds 30

Write-Host "Unblocking Redis port..."
Remove-NetFirewallRule -DisplayName "Block Redis Test"

Write-Host "Test complete. Check VIB logs for retry behavior."
```

**Verificación Esperada**:
1. ✅ main.cpp NO se bloquea
2. ✅ RedisWorker logs "Connection timeout" o "Publish failed"
3. ✅ Sistema intenta reconexión (max 5 intentos, 1s delay)
4. ✅ Video NDI continúa fluyendo sin interrupciones
5. ✅ Logs: `[STATUS] Redis: Disconnected, Retry count: X`

### 2.3 Monitoreo de Temperatura GPU

**Script de Monitoreo Continuo** (`gpu_temp_monitor.bat`):

```batch
@echo off
echo GPU Temperature Monitor - Press Ctrl+C to stop
echo Logging to gpu_temp.log
echo Timestamp,Temperature_C,Utilization_%%,Power_W > gpu_temp.log

:loop
for /f "tokens=*" %%a in ('nvidia-smi --query-gpu=timestamp,temperature.gpu,utilization.gpu,power.draw --format=csv,noheader') do (
    echo %%a
    echo %%a >> gpu_temp.log
)
timeout /t 5 /nobreak >nul
goto loop
```

**Análisis de Temperatura**:

```powershell
# PowerShell - Analizar log de temperatura
$temps = Import-Csv gpu_temp.log
$avgTemp = ($temps.Temperature_C | Measure-Object -Average).Average
$maxTemp = ($temps.Temperature_C | Measure-Object -Maximum).Maximum

Write-Host "Average Temperature: $avgTemp °C"
Write-Host "Maximum Temperature: $maxTemp °C"

if ($maxTemp -gt 85) {
    Write-Host "[CRITICAL] Temperature exceeded 85°C threshold!" -ForegroundColor Red
} elseif ($maxTemp -gt 78) {
    Write-Host "[WARNING] Temperature above 78°C" -ForegroundColor Yellow
} else {
    Write-Host "[OK] Temperature within safe range" -ForegroundColor Green
}
```

**Umbrales de Alerta**:
- **<70°C**: Normal (verde)
- **70-78°C**: Vigilar (amarillo)
- **78-85°C**: Alerta (naranja) - verificar ventilación
- **>85°C**: Crítico (rojo) - DETENER pruebas, verificar cooling

### 2.4 Checklist de Ejecución de Stress Test (15 minutos)

```
[ ] Tiempo 0:00 - Ejecutar gpu_temp_monitor.bat (dejar corriendo)
[ ] Tiempo 0:30 - Ejecutar vram_saturation_test.bat
[ ] Tiempo 3:00 - Verificar VRAM avg ~1.6GB
[ ] Tiempo 5:00 - Ejecutar redis_failure_test.bat
[ ] Tiempo 8:00 - Verificar logs de retry en VIB
[ ] Tiempo 10:00 - Restaurar Redis, verificar reconexión
[ ] Tiempo 12:00 - Verificar temperatura GPU <78°C
[ ] Tiempo 15:00 - Detener VIB, analizar gpu_temp.log
[ ] Tiempo 15:30 - PASS/FAIL decision

CRITERIOS DE ÉXITO:
✅ VRAM usage: 1.6-2.0 GB (no leaks)
✅ Redis failure: Sistema continúa, retries visibles
✅ GPU temp max: <78°C
✅ No crashes o excepciones en logs
```

---

## TAREA 3: ANÁLISIS DE CUDA STREAMS

### Estado Actual: ✅ ÓPTIMO

**Ubicación**: `src/core/main.cpp`

**Creación** (línea 590-596):
```cpp
// Create dedicated CUDA stream for YOLO processing (non-blocking)
cudaStream_t yoloStream = nullptr;
cudaError_t err = cudaStreamCreate(&yoloStream);
if (err != cudaSuccess) {
    Logger::Error("Failed to create YOLO CUDA stream: " + std::string(cudaGetErrorString(err)));
    throw std::runtime_error("CUDA stream creation failed");
}
Logger::Info("Created dedicated CUDA stream for YOLO (non-blocking pipeline)");
```

**Uso** (línea 515):
```cpp
auto detections = yoloProcessor->ProcessFrame(
    channel.cudaBGRABuffer,
    channel.channelID,
    channel.width,
    channel.height,
    yoloStream  // Separate stream - does NOT block capture stream
);
```

**Destrucción** (línea 680-682):
```cpp
// Destroy YOLO CUDA stream
if (yoloStream) {
    cudaStreamDestroy(yoloStream);
    Logger::Info("YOLO CUDA stream destroyed");
}
```

### Análisis de Performance

**✅ Implementación Correcta**:
1. Stream creado **UNA VEZ** al inicio (no por frame)
2. Stream es **persistente** durante toda la ejecución
3. Stream destruido solo en **cleanup** final
4. **Overhead de creación**: ~0 (una sola vez al inicio)

**❌ Anti-patrón Evitado** (NO presente en el código):
```cpp
// MAL - NO hacer esto
void ProcessFrame() {
    cudaStream_t stream;
    cudaStreamCreate(&stream);  // Overhead ~50-100μs POR FRAME
    // ... procesamiento ...
    cudaStreamDestroy(stream);  // Overhead adicional
}
```

**Overhead Real**:
- Creación inicial: ~0.05-0.1 ms (una vez)
- Por frame: **0 ms** (stream ya existe)
- Destrucción final: ~0.01 ms (una vez)

### Recomendación: ✅ NO CAMBIAR

El código actual es óptimo. Mover a miembro estático/clase NO mejoraría performance y complicaría lifecycle management.

**Posible Mejora Futura** (baja prioridad):
- Crear múltiples streams YOLO (uno por cámara activa) para mayor paralelismo
- Requeriría refactor de YOLOProcessor para batch real asíncrono
- Beneficio marginal: ~5-10% reducción en yolo_ms

---

## TAREA 4: CHECKLIST GO/NO-GO (Operador)

### PRE-DEPLOYMENT VALIDATION CHECKLIST

**Fecha**: ___________  **Operador**: ___________  **Hora Inicio**: ___________

---

#### 1. DeckLink Cards en Modo "4 Channels" ✅❌

**Verificación**:
```batch
# Ejecutar DeckLink Configuration Utility
"C:\Program Files\Blackmagic Design\Desktop Video\DesktopVideoConfig.exe"
```

**Checklist Manual**:
- [ ] Card 1: Modo "4 Channels" seleccionado (Settings → Card Mode)
- [ ] Card 2: Modo "4 Channels" seleccionado
- [ ] Card 3: Modo "4 Channels" seleccionado
- [ ] Restart confirmado después de cambio de modo
- [ ] 12 inputs visibles en total (3 cards × 4 channels)

**Comando Alternativo** (verificación):
```powershell
# PowerShell - Verifica que hay exactamente 12 DeckLink devices
$devices = Get-WmiObject Win32_PnPEntity | Where-Object {$_.Name -like "*DeckLink*"}
$count = ($devices | Measure-Object).Count
if ($count -eq 12) {
    Write-Host "[PASS] 12 DeckLink inputs detected" -ForegroundColor Green
} else {
    Write-Host "[FAIL] Expected 12, found $count DeckLink inputs" -ForegroundColor Red
}
```

---

#### 2. VRAM Disponible >12 GB ✅❌

**Comando**:
```batch
nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits
```

**Criterios**:
- [ ] VRAM libre: **>12,288 MiB** (12 GB)
- [ ] Si <12 GB: cerrar otras aplicaciones GPU (Chrome, OBS, etc.)
- [ ] Reiniciar GPU driver si necesario: `nvidia-smi -r`

**Script de Validación**:
```powershell
$vramFree = (nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits)
if ([int]$vramFree -gt 12288) {
    Write-Host "[PASS] VRAM available: $vramFree MiB" -ForegroundColor Green
} else {
    Write-Host "[FAIL] Insufficient VRAM: $vramFree MiB (need >12288)" -ForegroundColor Red
}
```

---

#### 3. Redis Conectado ✅❌

**Comando**:
```batch
redis-cli ping
```

**Respuesta Esperada**: `PONG`

**Checklist**:
- [ ] Redis server running: `tasklist | findstr redis-server`
- [ ] Redis responde a ping: `redis-cli ping` → PONG
- [ ] Puerto 6379 abierto: `netstat -an | findstr 6379`
- [ ] Config.json: `redis.enabled = true` y `redis.host = "127.0.0.1"`

**Script de Validación**:
```batch
redis-cli ping >nul 2>&1
if %errorlevel% equ 0 (
    echo [PASS] Redis is running and responsive
) else (
    echo [FAIL] Redis is not responding
    echo Starting Redis server...
    start "" "C:\Program Files\Redis\redis-server.exe"
    timeout /t 3 /nobreak
)
```

---

#### 4. NDI Fuentes Visibles en Studio Monitor ✅❌

**Herramienta**: NDI Studio Monitor

**Verificación**:
1. Abrir NDI Studio Monitor
2. Esperar 5 segundos para discovery
3. Verificar lista de fuentes

**Checklist**:
- [ ] 12 fuentes NDI visibles: `VIB_CAM_01` a `VIB_CAM_12`
- [ ] Todas las fuentes muestran estado "Available" (no "Offline")
- [ ] Test: Click en `VIB_CAM_01` → video visible (aunque sea negro/test pattern)

**Comando Alternativo** (NDI Discovery Tool):
```batch
"C:\Program Files\NDI\NDI 6 Tools\Discovery\NDI Discovery.exe"
```

**Script de Verificación** (Python, si NDI SDK instalado):
```python
# Verificar fuentes NDI programáticamente
import NDIlib as ndi

ndi.initialize()
finder = ndi.find_create_v2()
ndi.find_wait_for_sources(finder, 5000)  # Wait 5 seconds

sources = ndi.find_get_current_sources(finder)
vib_sources = [s for s in sources if 'VIB_CAM' in s.ndi_name]

if len(vib_sources) == 12:
    print("[PASS] All 12 VIB NDI sources detected")
else:
    print(f"[FAIL] Expected 12 sources, found {len(vib_sources)}")

ndi.find_destroy(finder)
ndi.destroy()
```

---

#### 5. Config.json Validado ✅❌

**Ubicación**: `config.json`

**Checklist de Parámetros Críticos**:
```json
{
  "yolo": {
    "enabled": true,                          ✅❌
    "model_path": "models/yolov8n.engine",   ✅❌ (archivo existe)
    "batch_size": 4,                          ✅❌
    "use_fp16": true                          ✅❌
  },
  "camera_selector": {
    "enabled": true,                          ✅❌
    "top_k": 4                                ✅❌
  },
  "detection_optimization": {
    "hysteresis": {
      "switch_threshold": 0.20,               ✅❌ (0.15-0.30 válido)
      "min_active_frames": 15,                ✅❌ (10-30 válido)
      "decay_factor": 0.95                    ✅❌ (0.90-0.98 válido)
    }
  },
  "redis": {
    "enabled": true,                          ✅❌
    "host": "127.0.0.1",                      ✅❌
    "port": 6379                              ✅❌
  }
}
```

**Script de Validación** (Python):
```python
import json
import os

with open('config.json', 'r') as f:
    config = json.load(f)

# Validar estructura
assert config['yolo']['enabled'] == True
assert config['yolo']['batch_size'] == 4
assert config['camera_selector']['top_k'] == 4
assert 0.15 <= config['detection_optimization']['hysteresis']['switch_threshold'] <= 0.30

# Validar archivo modelo existe
model_path = config['yolo']['model_path']
if os.path.exists(model_path):
    print(f"[PASS] Model file found: {model_path}")
else:
    print(f"[FAIL] Model file missing: {model_path}")

print("[PASS] Config.json validated")
```

---

#### 6. Temperatura GPU Inicial <50°C ✅❌

**Comando**:
```batch
nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits
```

**Criterios**:
- [ ] Temperatura GPU: **<50°C** (idle state)
- [ ] Si >50°C: Esperar 5 minutos con ventilación máxima
- [ ] Verificar cooling system funcionando correctamente

**Script de Validación**:
```powershell
$temp = [int](nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits)
Write-Host "GPU Temperature: $temp °C"

if ($temp -lt 50) {
    Write-Host "[PASS] GPU temperature baseline OK" -ForegroundColor Green
} elseif ($temp -lt 60) {
    Write-Host "[WARNING] GPU warm - wait for cooldown" -ForegroundColor Yellow
} else {
    Write-Host "[FAIL] GPU too hot - check cooling system" -ForegroundColor Red
}
```

---

### GO/NO-GO DECISION MATRIX

| Check | Status | Blocker | Action if Failed |
|-------|--------|---------|------------------|
| 1. DeckLink 4-channel mode | ✅❌ | **YES** | Configure cards, restart |
| 2. VRAM >12 GB | ✅❌ | **YES** | Close GPU apps, driver restart |
| 3. Redis connected | ✅❌ | **NO** | Start Redis (system can run without) |
| 4. NDI sources visible | ✅❌ | **YES** | Check network, restart VIB |
| 5. Config.json valid | ✅❌ | **YES** | Fix config, validate schema |
| 6. GPU temp <50°C | ✅❌ | **NO** | Wait for cooldown, check fans |

**GO Decision**: ALL blockers (1, 2, 4, 5) must be ✅  
**NO-GO Decision**: ANY blocker fails → Fix before proceeding

---

### RESUMEN DE COMANDOS RÁPIDOS

```batch
@echo off
echo ========================================
echo PRE-DEPLOYMENT VALIDATION
echo ========================================

echo [1/6] VRAM Check...
nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits

echo [2/6] GPU Temperature...
nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits

echo [3/6] Redis Check...
redis-cli ping

echo [4/6] DeckLink Devices...
powershell -Command "(Get-WmiObject Win32_PnPEntity | Where-Object {$_.Name -like '*DeckLink*'} | Measure-Object).Count"

echo [5/6] Config.json exists...
if exist "config.json" (echo FOUND) else (echo MISSING!)

echo [6/6] Model file exists...
if exist "models\yolov8n.engine" (echo FOUND) else (echo MISSING!)

echo ========================================
echo Validation complete. Review above.
echo ========================================
pause
```

---

## TAREA 5: HYSTERESIS CONFIGURABLE (IMPLEMENTADO)

Ver archivos modificados:
- `src/ai/ActiveCameraSelector.cpp` - Lee config desde JSON
- `src/core/main.cpp` - Carga hysteresis desde config
- `config.json` - Nueva sección `detection_optimization`

**Beneficio**: Ajuste en caliente sin recompilar, óptimo para calibración en campo con diferentes ángulos de cámara.

---

## ANEXO: TROUBLESHOOTING RÁPIDO

| Síntoma | Causa Probable | Solución |
|---------|----------------|----------|
| VRAM >3 GB | Memory leak | Reiniciar VIB, check logs |
| GPU temp >85°C | Cooling failure | Stop tests, check fans |
| Redis timeouts | Network issue | Check firewall, restart Redis |
| NDI sources offline | VIB no ejecutando | Start VIB, wait 10s |
| "Model file not found" | Path incorrecto | Verify model_path in config.json |
| DeckLink not detected | Card mode wrong | Set to "4 Channels", restart |

---

**Preparación Completa**: ✅  
**Listo para Hardware Real**: Mañana  
**Tiempo Estimado de Validación**: 15-20 minutos
