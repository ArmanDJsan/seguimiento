# SDK Integration Guide

This document provides step-by-step instructions for integrating the required SDKs.

## 1. Blackmagic DeckLink SDK

### Download

1. Visit [Blackmagic Design Developer](https://www.blackmagicdesign.com/developer)
2. Register for a developer account
3. Download "Desktop Video SDK" (latest version)

### Installation

1. Extract the SDK to `C:\Program Files\Blackmagic Design\DeckLink SDK`
2. Add include path in Visual Studio:
   - Project Properties → C/C++ → Additional Include Directories
   - Add: `C:\Program Files\Blackmagic Design\DeckLink SDK\Win\include`

### Key Files

- `DeckLinkAPI.h` - Main API header
- `DeckLinkAPI_i.c` - COM interface definitions
- `DeckLinkAPIVersion.h` - Version information

### Integration in Code

```cpp
#include "DeckLinkAPI_h.h"

// Create DeckLink iterator
IDeckLinkIterator* deckLinkIterator;
CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                 IID_IDeckLinkIterator, (void**)&deckLinkIterator);

// Enumerate devices
IDeckLink* deckLink;
while (deckLinkIterator->Next(&deckLink) == S_OK) {
    // Found a device
}
```

## 2. NDI SDK (Network Device Interface)

NDI is used for sending video to vMix. It provides native support in vMix without any plugins.

### Download

1. Visit [NDI SDK Downloads](https://ndi.video/for-developers/ndi-sdk/)
2. Register for a free developer account (requires name, email, and organization info)
3. Download "NDI 5 SDK" for Windows

### Installation

1. Run the NDI SDK installer
2. Default installation path: `C:\Program Files\NDI\NDI 5 SDK`

### Visual Studio Configuration

#### Include Directories
Project Properties → C/C++ → Additional Include Directories:
- Add: `C:\Program Files\NDI\NDI 5 SDK\Include`

#### Library Directories
Project Properties → Linker → General → Additional Library Directories:
- Add: `C:\Program Files\NDI\NDI 5 SDK\Lib\x64`

#### Additional Dependencies
Project Properties → Linker → Input → Additional Dependencies:
- Add: `Processing.NDI.Lib.x64.lib`

### Runtime DLL

The NDI runtime DLL must be available at runtime:
- `Processing.NDI.Lib.x64.dll`

Options:
1. Install NDI Tools (includes runtime): https://ndi.video/tools/
2. Copy DLL to application directory
3. Add NDI bin directory to PATH

### Integration in Code

```cpp
#include "Processing.NDI.Lib.h"

// Initialize NDI
if (!NDIlib_initialize()) {
    // Error: NDI not available
    return false;
}

// Create sender
NDIlib_send_create_t send_desc;
send_desc.p_ndi_name = "VIB_CAM_01";
send_desc.p_groups = nullptr;
send_desc.clock_video = true;
send_desc.clock_audio = false;

NDIlib_send_instance_t pNDI_send = NDIlib_send_create(&send_desc);

// Send video frame (UYVY format for optimal performance)
NDIlib_video_frame_v2_t video_frame;
video_frame.xres = 3840;
video_frame.yres = 2160;
video_frame.FourCC = NDIlib_FourCC_type_UYVY;
video_frame.frame_rate_N = 30000;
video_frame.frame_rate_D = 1001;
video_frame.picture_aspect_ratio = 16.0f / 9.0f;
video_frame.line_stride_in_bytes = 3840 * 2;  // UYVY = 2 bytes per pixel
video_frame.p_data = frameData;

NDIlib_send_send_video_v2(pNDI_send, &video_frame);

// Cleanup
NDIlib_send_destroy(pNDI_send);
NDIlib_destroy();
```

### Zero-Copy Async Sending

For maximum performance, use async sending with completion callbacks:

```cpp
// Set up async completion callback
NDIlib_send_set_video_async_completion(pNDI_send,
    [](void* instance, const NDIlib_video_frame_v2_t* frame, void* user_data) {
        // Frame is no longer needed - can reuse buffer
        FramePool* pool = static_cast<FramePool*>(user_data);
        pool->ReleaseFrame(frame->p_data);
    },
    &framePool);

// Send asynchronously (returns immediately)
NDIlib_send_send_video_async_v2(pNDI_send, &video_frame);
```

### vMix Configuration for Multiple NDI Sources

When using 12 cameras with vMix:

| Option | Setting | Reason |
|--------|---------|--------|
| High Input Performance Mode | ✅ ENABLE | Required for >8 cameras, needs GPU with >3GB VRAM |
| Show preview thumbnails for NDI sources | ❌ DISABLE | Reduces network traffic with many sources |

## 3. Redis C++ Client

### Using redis-plus-plus

#### Installation via vcpkg (Recommended)

```bash
vcpkg install redis-plus-plus:x64-windows
```

#### Manual Installation

1. Clone repository:
```bash
git clone https://github.com/sewenew/redis-plus-plus.git
```

2. Build with CMake:
```bash
cd redis-plus-plus
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

3. Link in Visual Studio:
   - Add include directory: `redis-plus-plus/src`
   - Add library: `redis++.lib`
   - Add dependency: `hiredis.lib`

### Integration in Code

```cpp
#include <sw/redis++/redis++.h>

using namespace sw::redis;

// Connect
Redis redis("tcp://127.0.0.1:6379");

// Set value
redis.set("key", "value");

// Get value
auto value = redis.get("key");

// Publish
redis.publish("channel", "message");
```

## 4. NVIDIA CUDA and TensorRT

### Prerequisites

- NVIDIA GPU with compute capability ≥ 7.0 (RTX 5080 is 8.9)
- Latest NVIDIA drivers

### Download CUDA Toolkit

1. Visit [NVIDIA CUDA Downloads](https://developer.nvidia.com/cuda-downloads)
2. Download CUDA Toolkit 12.x for Windows
3. Run installer (default installation path: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.x`)

### Download TensorRT

1. Visit [NVIDIA TensorRT](https://developer.nvidia.com/tensorrt)
2. Download TensorRT 8.x or higher for Windows
3. Extract to `C:\Program Files\NVIDIA\TensorRT-8.x`

### Visual Studio Configuration

#### Include Directories
- `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.x\include`
- `C:\Program Files\NVIDIA\TensorRT-8.x\include`

#### Library Directories
- `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.x\lib\x64`
- `C:\Program Files\NVIDIA\TensorRT-8.x\lib`

#### Additional Dependencies
- `cudart.lib`
- `nvinfer.lib`
- `nvonnxparser.lib`
- `nvinfer_plugin.lib`

### Integration in Code

```cpp
#include <NvInfer.h>
#include <cuda_runtime.h>

using namespace nvinfer1;

// Create TensorRT logger
class Logger : public ILogger {
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING)
            std::cout << msg << std::endl;
    }
} logger;

// Create runtime
IRuntime* runtime = createInferRuntime(logger);

// Load engine
ICudaEngine* engine = runtime->deserializeCudaEngine(data, size);

// Create context
IExecutionContext* context = engine->createExecutionContext();
```

## 5. Additional Dependencies

### Windows SDK

Required for DirectX 11:
- Included with Visual Studio 2022
- Ensure Windows 10 SDK (10.0.19041.0 or later) is installed

### Libraries Needed

In Project Properties → Linker → Input:
```
d3d11.lib
dxgi.lib
d3dcompiler.lib
```

## 6. RapidJSON (Optional, for fast JSON parsing)

### Installation

RapidJSON is header-only:

1. Download from [GitHub](https://github.com/Tencent/rapidjson)
2. Extract headers to project
3. Add include directory

### Usage

```cpp
#include "rapidjson/document.h"

rapidjson::Document doc;
doc.Parse(jsonString.c_str());

if (doc.HasMember("detections")) {
    auto& detections = doc["detections"];
    // Process array
}
```

## 7. Project Setup Checklist

- [ ] Visual Studio 2022 installed with C++ workload
- [ ] Windows 10/11 SDK installed
- [ ] Blackmagic DeckLink SDK installed and path configured
- [ ] NDI 5 SDK installed and path configured
- [ ] NDI Tools installed (for runtime DLL) or DLL copied to app directory
- [ ] Redis C++ client (redis-plus-plus) installed
- [ ] CUDA Toolkit 12.x installed
- [ ] TensorRT 8.x+ installed and configured
- [ ] All include paths added to project
- [ ] All library paths added to project
- [ ] All required .lib files added to linker input
- [ ] Project builds successfully in Debug mode
- [ ] Project builds successfully in Release mode

## 8. Verification

### Test SDK Integration

Create a simple test program:

```cpp
#include <iostream>
#include <windows.h>
#include <objbase.h>
#include "DeckLinkAPI_h.h"
#include "Processing.NDI.Lib.h"
#include <sw/redis++/redis++.h>
#include <NvInfer.h>

int main() {
    std::cout << "Testing SDK integration..." << std::endl;
    
    // Initialize COM
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    // Test DeckLink
    IDeckLinkIterator* iter;
    HRESULT hr = CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, 
                                   CLSCTX_ALL, IID_IDeckLinkIterator, 
                                   (void**)&iter);
    std::cout << "DeckLink SDK: " << (SUCCEEDED(hr) ? "OK" : "FAILED") << std::endl;
    if (iter) iter->Release();
    
    // Test NDI
    if (NDIlib_initialize()) {
        std::cout << "NDI SDK: OK" << std::endl;
        
        // Create a test sender
        NDIlib_send_create_t send_desc;
        send_desc.p_ndi_name = "VIB_Test";
        send_desc.p_groups = nullptr;
        send_desc.clock_video = false;
        send_desc.clock_audio = false;
        
        NDIlib_send_instance_t sender = NDIlib_send_create(&send_desc);
        if (sender) {
            std::cout << "NDI Sender: OK" << std::endl;
            NDIlib_send_destroy(sender);
        }
        
        NDIlib_destroy();
    } else {
        std::cout << "NDI SDK: FAILED" << std::endl;
    }
    
    // Test Redis
    try {
        sw::redis::Redis redis("tcp://127.0.0.1:6379");
        std::cout << "Redis: OK" << std::endl;
    } catch (...) {
        std::cout << "Redis: FAILED (is server running?)" << std::endl;
    }
    
    // Test TensorRT
    std::cout << "TensorRT SDK: OK" << std::endl;
    
    CoUninitialize();
    return 0;
}
```

If all tests pass, your environment is ready for development!

## Troubleshooting

### Common Issues

**"Cannot open include file: 'DeckLinkAPI_h.h'"**
- Verify SDK installation path
- Check include directories in project properties

**"Cannot open include file: 'Processing.NDI.Lib.h'"**
- Verify NDI SDK installation path
- Check include directories (should be `C:\Program Files\NDI\NDI 5 SDK\Include`)

**"Unresolved external symbol"**
- Check that required .lib files are in linker input
- Verify library directories are correct
- For NDI: ensure `Processing.NDI.Lib.x64.lib` is linked

**Runtime errors with DLLs**
- Ensure DLL files are in the same directory as executable
- Or add DLL directories to system PATH
- For NDI: install NDI Tools or copy `Processing.NDI.Lib.x64.dll`

**CUDA errors**
- Update NVIDIA drivers to latest version
- Verify GPU supports required compute capability

**NDI sender not visible in vMix**
- Ensure NDI Tools runtime is installed
- Check Windows Firewall settings (NDI uses network discovery)
- Verify sender name doesn't conflict with existing sources

### Support Resources

- Blackmagic: [Developer Forums](https://forum.blackmagicdesign.com/viewforum.php?f=12)
- Spout: [GitHub Issues](https://github.com/leadedge/Spout2/issues)
- Redis: [Documentation](https://redis.io/docs/)
- TensorRT: [Developer Guide](https://docs.nvidia.com/deeplearning/tensorrt/)
