# Visual Intelligence Bypass (VIB) - v2.0

High-performance video capture and AI processing system for vMix with zero-copy DMA architecture.

## Overview

VIB is a C++ application designed for ultra-low latency video capture and AI-powered object detection. It captures multiple 4K@60fps streams from Blackmagic DeckLink cards, processes them with YOLO AI, and delivers the video to vMix via Spout while publishing detection metadata to Redis.

## Architecture Philosophy

**Performance over Comfort**: This system prioritizes maximum hardware utilization and minimal latency over development convenience.

### Key Features

- **Zero-Copy DMA**: Custom memory allocator provides GPU memory directly to DeckLink cards
- **GPU-Accelerated Color Conversion**: HLSL pixel shader converts YUV to RGB in <1ms
- **Video Bypass**: Spout delivers video to vMix with sub-frame latency
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
- **DirectX**: DirectX 11 Runtime
- **CUDA**: NVIDIA CUDA Toolkit 12.x
- **TensorRT**: NVIDIA TensorRT 8.x or higher

### Required SDKs

1. **Blackmagic DeckLink SDK**: Download from Blackmagic Design website
2. **Spout SDK**: [Leadedge/Spout2](https://github.com/leadedge/Spout2)
3. **Redis C++ Client**: [sewenew/redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
4. **NVIDIA TensorRT**: For YOLO inference

## Project Structure

```
src/
├── core/
│   └── main.cpp              # Application entry point
├── capture/
│   ├── DeckLinkCapture.h     # DeckLink capture interface
│   └── DeckLinkCapture.cpp   # Zero-copy allocator implementation
├── shaders/
│   ├── ColorConversion.hlsl  # YUV to RGB pixel shader
│   └── VertexShader.hlsl     # Full-screen quad vertex shader
├── spout/
│   ├── SpoutManager.h        # Spout sender management
│   └── SpoutManager.cpp      # Video output to vMix
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
- Spout SDK include/lib directories
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
- Configured for 4K input (3840x2160@60fps)
- Connected to video sources

### Redis Server

Install and run Redis on localhost:

```bash
# Windows (using WSL or Redis Windows port)
redis-server --port 6379
```

### vMix Integration

#### Receiving Video (Spout)

1. In vMix, click **Add Input**
2. Select **NDI / Desktop Capture** → **Spout** tab
3. Choose your camera sender (e.g., "Camera_01")
4. Video appears with <1 frame latency

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
| Video Latency | < 1 frame | TBD |
| Frame Rate | 60 fps | TBD |
| Max Streams | 12x 4K | TBD |
| VRAM Usage | < 8 GB | TBD |
| CPU Usage | < 30% | TBD |

## Technical Details

### Zero-Copy DMA Pipeline

1. **Allocation**: Custom allocator creates GPU-visible memory
2. **DMA Transfer**: DeckLink writes directly to GPU memory via PCIe
3. **Processing**: Pixel shader converts YUV→RGB on GPU
4. **Distribution**: Same texture shared with Spout and YOLO

### Pixel Shader Conversion

The YUV 4:2:2 (UYVY) to RGBA conversion uses BT.709 color space:

```
R = 1.1643(Y - 0.0625) + 1.5958(V - 0.5)
G = 1.1643(Y - 0.0625) - 0.3917(U - 0.5) - 0.8129(V - 0.5)
B = 1.1643(Y - 0.0625) + 2.017(U - 0.5)
```

This happens in parallel for all 8.3M pixels in a 4K frame.

### Threading Model

- **Main Thread**: DirectX 11 rendering and Spout output
- **Capture Callbacks**: One per DeckLink card (lightweight)
- **YOLO Worker**: Batch inference processing
- **Redis Worker**: 60Hz metadata publishing

## Troubleshooting

### No DeckLink Devices Found

- Check driver installation
- Verify cards are detected in Windows Device Manager
- Ensure DeckLink SDK is properly installed

### Spout Not Visible in vMix

- Verify vMix has Spout support enabled
- Check that the application is running as Administrator
- Ensure GPU is shared between applications

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
- Spout2 by Lynn Jarvis
- NVIDIA CUDA/TensorRT
- Redis by Redis Ltd.
- DirectX 11 by Microsoft

## Contact

For technical support or questions about the system architecture, refer to the contexto.MD file for detailed discussions and design decisions.
