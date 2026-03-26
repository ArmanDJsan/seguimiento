# Quick Start Guide

Get the Visual Intelligence Bypass system running in 5 steps.

## Prerequisites

Before starting, ensure you have:
- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ workload
- NVIDIA GPU (RTX series recommended)
- Administrator privileges

## Step 1: Install Dependencies (15 minutes)

### 1.1 Install Visual Studio 2022
- Download from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/)
- Select "Desktop development with C++"
- Include Windows 10 SDK

### 1.2 Install NVIDIA CUDA Toolkit
- Download from [NVIDIA CUDA](https://developer.nvidia.com/cuda-downloads)
- Install CUDA Toolkit 12.x
- Restart after installation

### 1.3 Install TensorRT
- Download from [NVIDIA TensorRT](https://developer.nvidia.com/tensorrt)
- Extract to `C:\Program Files\NVIDIA\TensorRT-8.x`

### 1.4 Install Redis (Optional for testing)
- Download Redis for Windows
- Or use WSL: `sudo apt install redis-server`

## Step 2: Download SDKs (10 minutes)

### 2.1 Blackmagic DeckLink SDK
1. Register at [Blackmagic Design Developer](https://www.blackmagicdesign.com/developer)
2. Download "Desktop Video SDK"
3. Extract to `C:\Program Files\Blackmagic Design\DeckLink SDK`

### 2.2 Spout SDK
```bash
git clone https://github.com/leadedge/Spout2.git C:\Spout2
```

### 2.3 Redis C++ Client (using vcpkg)
```bash
# Install vcpkg if not already installed
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install

# Install redis-plus-plus
.\vcpkg install redis-plus-plus:x64-windows
```

## Step 3: Configure the Project (5 minutes)

### 3.1 Open the Solution
1. Navigate to the `src` directory
2. Double-click `VIB.sln` to open in Visual Studio 2022

### 3.2 Update Include Paths
Right-click the VIB project → Properties:

**C/C++ → General → Additional Include Directories**, add:
```
C:\Program Files\Blackmagic Design\DeckLink SDK\Win\include
C:\Spout2\SPOUTSDK\SpoutSDK
C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.x\include
C:\Program Files\NVIDIA\TensorRT-8.x\include
```

**Linker → General → Additional Library Directories**, add:
```
C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.x\lib\x64
C:\Program Files\NVIDIA\TensorRT-8.x\lib
```

**Linker → Input → Additional Dependencies**, add:
```
d3d11.lib
dxgi.lib
cudart.lib
nvinfer.lib
nvonnxparser.lib
```

### 3.3 Select Configuration
- Configuration: **Release**
- Platform: **x64**

## Step 4: Build the Project (2 minutes)

1. Press `Ctrl+Shift+B` or select Build → Build Solution
2. Wait for compilation to complete
3. Verify no errors in the Output window
4. Executable will be in `src\bin\Release\VIB.exe`

### Troubleshooting Build Errors

**Missing include files:**
- Double-check SDK paths in project properties
- Ensure SDKs are installed correctly

**Linker errors:**
- Verify library directories are correct
- Check that .lib files exist in specified paths

## Step 5: First Run (5 minutes)

### 5.1 Start Redis (if using)
```bash
# Windows with WSL
wsl
sudo service redis-server start

# Or Windows native Redis
redis-server.exe
```

### 5.2 Connect Hardware
- Connect Blackmagic DeckLink card(s)
- Connect video sources to DeckLink inputs
- Ensure latest Blackmagic Desktop Video drivers installed

### 5.3 Run the Application
```bash
cd src\bin\Release
VIB.exe
```

### 5.4 Connect vMix
1. Open vMix
2. Click **Add Input**
3. Select **NDI / Desktop Capture** → **Spout** tab
4. Look for "Camera_XX" senders
5. Select a camera and click OK
6. Video should appear with minimal latency

## Verification Checklist

After following these steps, verify:

- [ ] VIB.exe starts without errors
- [ ] Console shows "Visual Intelligence Bypass v2.0 Starting..."
- [ ] DeckLink devices are detected (check console output)
- [ ] Spout senders appear in vMix input list
- [ ] Video appears in vMix with low latency
- [ ] Redis connection successful (if enabled)
- [ ] No dropped frames in console logs
- [ ] GPU usage visible in Task Manager

## What's Next?

### Basic Configuration

Edit settings in `main.cpp` or create a configuration file:
- Number of cameras to capture
- Video resolution (default: 4K)
- Frame rate (default: 60fps)
- YOLO model path
- Redis connection details

### Adding YOLO Model

1. Download or train a YOLO model
2. Convert to TensorRT:
```bash
trtexec --onnx=yolov8n.onnx --saveEngine=yolov8n.trt --fp16
```
3. Update model path in code

### Testing Detection Data

Monitor Redis output:
```bash
redis-cli
SUBSCRIBE vmix_detections
```

Or read the data key:
```bash
redis-cli GET VMIX_DATA_STREAM
```

### Performance Tuning

Monitor performance:
1. GPU usage (should be 60-80% with 4K streams)
2. Frame drops (should be 0)
3. Latency (should be < 33ms total)
4. VRAM usage (should be < 10GB with 4 streams)

## Common Issues

### "No DeckLink devices found"
- Install Desktop Video drivers
- Check Device Manager for DeckLink cards
- Run as Administrator

### "Spout not visible in vMix"
- Ensure both apps use same GPU
- Check Spout sender names
- Restart vMix

### "High CPU usage"
- Check if using integrated GPU instead of discrete
- Verify zero-copy is working
- Profile with Visual Studio

### "Redis connection failed"
- Start Redis server
- Check port 6379 is not blocked
- Verify Redis is listening on 127.0.0.1

## Getting Help

### Documentation
- See `README.md` for complete system overview
- See `IMPLEMENTATION.md` for technical details
- See `SDK_INTEGRATION.md` for SDK setup help

### Debugging
Enable debug logging:
```cpp
Logger::SetLogLevel(LogLevel::DEBUG);
```

Check log files in application directory.

### Support Resources
- Blackmagic Developer Forum
- Spout GitHub Issues
- NVIDIA Developer Forums

## Congratulations!

If you've reached this point and everything works, you now have a high-performance video processing system capable of:
- Capturing multiple 4K@60fps streams
- Real-time AI object detection
- Ultra-low latency output to vMix
- Asynchronous data publishing

Ready to process some video! 🎥🚀
