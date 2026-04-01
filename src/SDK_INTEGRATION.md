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

## 2. Spout SDK

### Download

```bash
git clone https://github.com/leadedge/Spout2.git
```

### Installation

1. Navigate to `Spout2/SPOUTSDK/SpoutSDK`
2. Copy the following to your project:
   - `Spout.h`
   - `Spout.cpp`
   - `SpoutSender.h`
   - `SpoutSender.cpp`

3. Or link against pre-built library:
   - Build the Spout SDK project
   - Link `SpoutLibrary.lib` in your project

### Visual Studio Configuration

Project Properties → Linker → Input → Additional Dependencies:
- Add: `SpoutLibrary.lib`

### Integration in Code

```cpp
#include "SpoutSender.h"

SpoutSender sender;
sender.CreateSender("MyCameraName", 3840, 2160);

// Send texture
sender.SendTexture(textureID, GL_TEXTURE_2D, 3840, 2160);
```

### Note: DirectX 11 Support

Spout supports both OpenGL and DirectX 11. For DirectX 11:

```cpp
#include "SpoutDirectX.h"

spoutDirectX spout;
spout.CreateSender("MyCameraName", 3840, 2160);
spout.SendTexture(d3d11Texture);
```

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
- [ ] Spout SDK integrated or library linked
- [ ] Redis C++ client (redis-plus-plus) installed
- [ ] CUDA Toolkit 12.x installed
- [ ] TensorRT 8.x installed and configured
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
#include "DeckLinkAPI_h.h"
#include "SpoutSender.h"
#include <sw/redis++/redis++.h>
#include <NvInfer.h>

int main() {
    std::cout << "Testing SDK integration..." << std::endl;
    
    // Test DeckLink
    IDeckLinkIterator* iter;
    HRESULT hr = CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, 
                                   CLSCTX_ALL, IID_IDeckLinkIterator, 
                                   (void**)&iter);
    std::cout << "DeckLink SDK: " << (SUCCEEDED(hr) ? "OK" : "FAILED") << std::endl;
    
    // Test Spout
    SpoutSender spout;
    std::cout << "Spout SDK: OK" << std::endl;
    
    // Test Redis
    try {
        sw::redis::Redis redis("tcp://127.0.0.1:6379");
        std::cout << "Redis: OK" << std::endl;
    } catch (...) {
        std::cout << "Redis: FAILED (is server running?)" << std::endl;
    }
    
    // Test TensorRT
    nvinfer1::ILogger* logger;
    std::cout << "TensorRT SDK: OK" << std::endl;
    
    return 0;
}
```

If all tests pass, your environment is ready for development!

## Troubleshooting

### Common Issues

**"Cannot open include file: 'DeckLinkAPI_h.h'"**
- Verify SDK installation path
- Check include directories in project properties

**"Unresolved external symbol"**
- Check that required .lib files are in linker input
- Verify library directories are correct

**Runtime errors with DLLs**
- Ensure DLL files are in the same directory as executable
- Or add DLL directories to system PATH

**CUDA errors**
- Update NVIDIA drivers to latest version
- Verify GPU supports required compute capability

### Support Resources

- Blackmagic: [Developer Forums](https://forum.blackmagicdesign.com/viewforum.php?f=12)
- Spout: [GitHub Issues](https://github.com/leadedge/Spout2/issues)
- Redis: [Documentation](https://redis.io/docs/)
- TensorRT: [Developer Guide](https://docs.nvidia.com/deeplearning/tensorrt/)
