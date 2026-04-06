# Visual Intelligence Bypass (VIB) - v2.0

High-performance video capture and AI processing system for vMix with zero-copy DMA architecture.

## Overview

VIB is a C++ application designed for ultra-low latency video capture and AI-powered object detection. It captures multiple 4K@30fps streams from Blackmagic DeckLink cards, processes them with YOLO AI, and delivers the video to vMix via NDI while publishing detection metadata to Redis.

## Architecture Philosophy

**Performance over Comfort**: This system prioritizes maximum hardware utilization and minimal latency over development convenience.

### Key Features

- **Zero-Copy DMA**: Custom memory allocator provides GPU memory directly to DeckLink cards
- **GPU-Accelerated Color Conversion**: CUDA kernel converts YUV to BGRA for YOLO processing
- **NDI Video Output**: NDI delivers video to vMix with native support and zero-copy async sending
- **Batch AI Processing**: TensorRT processes multiple camera streams efficiently
- **Asynchronous Data Pipeline**: Redis worker decouples metadata updates from video flow

## System Requirements

### Hardware (Target Configuration)

- **CPU**: AMD Threadripper Pro 9955WX (64 cores)
- **GPU**: NVIDIA RTX 5080 (16GB VRAM)
- **Capture**: 3x Blackmagic DeckLink 8K Pro Mini
- **RAM**: 128GB DDR5
- **Storage**: NVMe SSD for YOLO models

### Software

- **OS**: Windows 10/11 (64-bit)
- **Compiler**: Visual Studio 2022 with C++20 support
- **DirectX**: DirectX 11 Runtime (for CUDA interop)
- **CUDA**: NVIDIA CUDA Toolkit 12.x+
- **TensorRT**: NVIDIA TensorRT 10.x or higher

### Required SDKs

1. **Blackmagic DeckLink SDK**: Download from Blackmagic Design website
2. **NDI SDK 5**: Download from [ndi.video/for-developers](https://ndi.video/for-developers/ndi-sdk/)
3. **Redis C++ Client**: [sewenew/redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
4. **NVIDIA TensorRT**: For YOLO inference

## Project Structure

```
src/
├── core/
│   └── main.cpp              # Application entry point
├── capture/
│   ├── DeckLinkCapture.h     # DeckLink capture interface
│   ├── DeckLinkCapture.cpp   # Zero-copy allocator implementation
│   └── CudaColorConversion.cu # CUDA YUV to BGRA kernel
├── shaders/
│   ├── ColorConversion.hlsl  # YUV to RGB pixel shader (legacy)
│   └── VertexShader.hlsl     # Full-screen quad vertex shader
├── ndi/
│   ├── NDIManager.h          # NDI sender management
│   └── NDIManager.cpp        # Video output to vMix via NDI
├── ai/
│   ├── YOLOProcessor.h       # YOLO/TensorRT interface
│   └── YOLOProcessor.cpp     # Object detection implementation
├── redis/
│   ├── RedisWorker.h         # Async Redis worker
│   └── RedisWorker.cpp       # Metadata publishing
├── utils/
│   ├── Logger.h              # Logging utility
│   └── Logger.cpp            # Log implementation
├── VIB.sln                   # Visual Studio solution
└── VIB.vcxproj               # Visual Studio project
```

## Building the Project

### 1. Install Dependencies

Install all required SDKs and libraries listed above.

### 2. Configure SDK Paths

Update the Visual Studio project to include paths to:
- Blackmagic DeckLink SDK include/lib directories
- NDI SDK 5 include/lib directories
- Redis++ include/lib directories
- CUDA/TensorRT include/lib directories

📖 **Guía Detallada**: Para instrucciones paso a paso de configuración de VS2022, consulte [VS2022_CONFIGURATION_GUIDE.md](VS2022_CONFIGURATION_GUIDE.md) que incluye:
- Configuración completa de Include Directories
- Configuración de Library Directories
- Configuración de Linker Inputs
- Verificación y resolución de problemas

### 3. Build

Open `src/VIB.sln` in Visual Studio 2022:

1. Select configuration (Debug or Release)
2. Select platform (x64)
3. Build Solution (Ctrl+Shift+B)

The executable will be in `src/bin/Release/VIB.exe`

## Configuration

### DeckLink Cards

The system auto-detects connected DeckLink devices. Ensure your cards are:
- Installed with latest drivers
- Configured for 4K input (3840x2160@30fps)
- Connected to video sources

### Redis Server

Install and run Redis on localhost:

```bash
# Windows (using WSL or Redis Windows port)
redis-server --port 6379
```

### vMix Integration

#### Receiving Video (NDI)

1. In vMix, click **Add Input**
2. Select **NDI / Desktop Capture** → **NDI** tab
3. Choose your camera sender (e.g., "VIB_CAM_01")
4. Video appears with low latency

#### vMix Settings for Multiple NDI Sources

For optimal performance with 12 cameras:

| Setting | Recommended | Reason |
|---------|-------------|--------|
| High Input Performance Mode | ✅ ENABLE | Required for >8 cameras, needs GPU >3GB VRAM |
| Show preview thumbnails for NDI sources | ❌ DISABLE | Reduces network overhead |

#### Receiving Detection Data

Create a vMix script to read from Redis:

```vbnet
' Example vMix Script
Dim redis As New StackExchange.Redis.ConnectionMultiplexer
Dim db As StackExchange.Redis.IDatabase = redis.GetDatabase()

Do While True
    Dim data As String = db.StringGet("VMIX_DATA_STREAM")
    ' Parse JSON and update vMix titles
    System.Threading.Thread.Sleep(16) ' 60Hz
Loop
```

## Data Format

### Redis JSON Structure

```json
{
  "detections": [
    {
      "cameraID": 1,
      "objectID": 0,
      "x": 0.5,
      "y": 0.5,
      "width": 0.1,
      "height": 0.1,
      "label": "person",
      "confidence": 0.95,
      "timestamp": 1234567890
    }
  ],
  "timestamp": 1234567890
}
```

### Coordinate System

- All positions are normalized to [0.0, 1.0]
- Origin (0,0) is top-left corner
- (1,1) is bottom-right corner

## Performance Targets

| Metric | Target | Actual |
|--------|--------|--------|
| Video Latency | < 2 frames | TBD |
| Frame Rate | 30 fps | TBD |
| Max Streams | 12x 4K | TBD |
| VRAM Usage | < 8 GB | TBD |
| CPU Usage | < 30% | TBD |

## Technical Details

### Zero-Copy DMA Pipeline

1. **Allocation**: Custom allocator creates CUDA pinned memory
2. **DMA Transfer**: DeckLink writes directly to pinned memory via PCIe
3. **GPU Copy**: CUDA async memcpy to device memory
4. **Processing**: CUDA kernel converts YUV→BGRA for YOLO
5. **Distribution**: 
   - UYVY goes directly to NDI for vMix (no conversion)
   - BGRA goes to YOLO for AI inference

### NDI Video Output

The system uses NDI for vMix integration with the following advantages:
- **Native vMix support**: No plugins required
- **Zero-copy async sending**: Uses NDI's completion callbacks
- **UYVY format**: Sends DeckLink's native format directly (vMix converts internally)

### Pixel Shader Conversion (CUDA)

The YUV 4:2:2 (UYVY) to BGRA conversion uses BT.709 color space:

```
R = 1.1643(Y - 0.0625) + 1.5958(V - 0.5)
G = 1.1643(Y - 0.0625) - 0.3917(U - 0.5) - 0.8129(V - 0.5)
B = 1.1643(Y - 0.0625) + 2.017(U - 0.5)
```

This happens in parallel for all 8.3M pixels in a 4K frame.

### Threading Model

- **Main Thread**: Application control and exit handling
- **Capture Callbacks**: One per DeckLink card (lightweight, from DeckLink)
- **Capture Threads**: Process frames and send to NDI
- **YOLO Worker**: Batch inference processing
- **Redis Worker**: 60Hz metadata publishing

## Troubleshooting

### DeckLink SDK Not Detected (COM Initialization Error)

**Symptom**: The log shows `[WARN] DeckLink SDK not available; signal lock checks will be stubbed` even though DeckLinkAPI64.dll is registered with regsvr32.

**Root Cause**: COM (Component Object Model) was not initialized before attempting to create DeckLink interfaces.

**Solution**: The application now properly initializes COM at startup:

1. **CoInitializeEx()** is called in `main()` before any DeckLink operations
2. Uses `COINIT_MULTITHREADED` for thread safety
3. **CoUninitialize()** is called on shutdown to release COM resources
4. Proper error handling for COM initialization failures

**Required Libraries**:
- `ole32.lib` - Core COM library
- `oleaut32.lib` - OLE Automation (for BSTR strings)

**Verification**: 
- Check logs for "COM initialized successfully for DeckLink SDK"
- Look for "Found X DeckLink device(s)" message
- No more "[WARN] DeckLink SDK not available" warnings

### No DeckLink Devices Found

- Check driver installation
- Verify cards are detected in Windows Device Manager
- Ensure DeckLink SDK is properly installed

### NDI Sources Not Visible in vMix

- Verify NDI Tools runtime is installed
- Check Windows Firewall settings (NDI uses network discovery)
- Ensure sender name doesn't conflict with existing sources
- Try restarting vMix after the VIB application starts

### Poor Performance

- Check PCIe lane allocation (minimum x8 per DeckLink)
- Verify GPU is not thermal throttling
- Monitor VRAM usage (should be < 10GB)
- Ensure latest NVIDIA drivers installed

## Future Enhancements

- [ ] Multi-GPU support for >12 streams
- [ ] H.264/HEVC encoding for remote streaming
- [ ] Web dashboard for monitoring
- [ ] Automatic calibration and color correction
- [ ] Support for other capture cards (AJA, Magewell)

## License

This project is proprietary software. All rights reserved.

## Credits

Based on the technical specifications from the contexto.MD conversation with Gemini AI.

### Technologies Used

- Blackmagic DeckLink SDK
- NDI SDK by NewTek/Vizrt
- NVIDIA CUDA/TensorRT
- Redis by Redis Ltd.
- DirectX 11 by Microsoft (for CUDA interop)

## Contact

For technical support or questions about the system architecture, refer to the contexto.MD file for detailed discussions and design decisions.
