# Implementation Guide

This document provides implementation details for each component of the Visual Intelligence Bypass system.

## 1. DeckLink Capture Implementation

### Custom Memory Allocator

The custom allocator is the heart of zero-copy DMA. It must:

1. Allocate memory pages that are visible to both CPU and GPU
2. Provide physical addresses to the DeckLink card
3. Maintain a pool of buffers to prevent overwrites

### Key Implementation Steps

```cpp
// 1. Create DirectX 11 staging texture with CPU access
D3D11_TEXTURE2D_DESC stagingDesc = {};
stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
stagingDesc.Usage = D3D11_USAGE_STAGING;

// 2. Map the texture to get CPU pointer
D3D11_MAPPED_SUBRESOURCE mapped;
context->Map(stagingTexture, 0, D3D11_MAP_READ_WRITE, 0, &mapped);

// 3. Provide this pointer to DeckLink allocator
void* bufferPtr = mapped.pData;

// 4. DeckLink writes directly to this memory
// When frame arrives, texture is already on GPU
```

### VideoInputFrameArrived Callback

The callback must be extremely lightweight (< 1ms execution time):

```cpp
HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame) {
    // 1. Add reference to prevent deletion
    frame->AddRef();
    
    // 2. Push to queue (lock-free if possible)
    frameQueue.push(frame);
    
    // 3. Signal processing thread
    frameReadyEvent.Set();
    
    // 4. Return immediately
    return S_OK;
}
```

## 2. Pixel Shader Pipeline

### Shader Compilation

Shaders should be compiled as part of the build process:

```batch
fxc.exe /T ps_5_0 /E main ColorConversion.hlsl /Fo ColorConversion.cso
fxc.exe /T vs_5_0 /E main VertexShader.hlsl /Fo VertexShader.cso
```

### Runtime Shader Loading

```cpp
// Load compiled shader
std::vector<char> shaderBlob = ReadFile("ColorConversion.cso");

// Create pixel shader
ID3D11PixelShader* pixelShader;
device->CreatePixelShader(shaderBlob.data(), shaderBlob.size(), 
                          nullptr, &pixelShader);
```

### Render Pipeline Setup

```cpp
// 1. Set render target (RGB texture)
context->OMSetRenderTargets(1, &rgbRenderTargetView, nullptr);

// 2. Set shader resources (YUV texture)
context->PSSetShaderResources(0, 1, &yuvTextureView);

// 3. Set shader
context->PSSetShader(pixelShader, nullptr, 0);

// 4. Draw full-screen quad
context->Draw(4, 0);

// Result: RGB texture ready for Spout and YOLO
```

## 3. Spout Integration

### Sender Initialization

```cpp
#include "SpoutSDK.h"

SpoutSender sender;
sender.CreateSender("Camera_01", width, height);
```

### Texture Sharing

Spout uses DirectX texture sharing:

```cpp
// Share the RGB texture handle
IDXGIResource* dxgiResource;
rgbTexture->QueryInterface(__uuidof(IDXGIResource), 
                           (void**)&dxgiResource);

HANDLE sharedHandle;
dxgiResource->GetSharedHandle(&sharedHandle);

// Send to Spout
sender.SendTexture(rgbTexture, width, height);
```

### Performance Notes

- Spout uses GPU memory directly (zero copy)
- Latency is typically < 0.5ms
- vMix can receive multiple Spout sources simultaneously

## 4. YOLO/TensorRT Integration

### Model Preparation

1. Export YOLO model to ONNX format
2. Convert ONNX to TensorRT engine:

```bash
trtexec --onnx=yolov8n.onnx --saveEngine=yolov8n.trt --fp16
```

### Engine Loading

```cpp
// Read engine file
std::vector<char> engineData = ReadFile("yolov8n.trt");

// Deserialize engine
IRuntime* runtime = createInferRuntime(logger);
ICudaEngine* engine = runtime->deserializeCudaEngine(
    engineData.data(), engineData.size());

// Create execution context
IExecutionContext* context = engine->createExecutionContext();
```

### Batch Processing

For maximum efficiency with multiple cameras:

```cpp
// Allocate batch input (e.g., 4 cameras at once)
const int batchSize = 4;
const int inputSize = 640 * 640 * 3;
float* batchInput;
cudaMalloc(&batchInput, batchSize * inputSize * sizeof(float));

// Copy all 4 camera textures to batch
for (int i = 0; i < batchSize; i++) {
    CopyTextureToBuffer(cameras[i].texture, 
                       batchInput + i * inputSize);
}

// Single inference for all 4 cameras
context->executeV2(buffers);
```

## 5. Redis Integration

### Connection Setup

```cpp
#include <sw/redis++/redis++.h>

using namespace sw::redis;

Redis redis("tcp://127.0.0.1:6379");
```

### Publishing Detection Data

Two approaches for vMix consumption:

#### Approach A: Key-Value

```cpp
// Set JSON data
redis.set("VMIX_DATA_STREAM", jsonData);

// vMix reads periodically
auto data = redis.get("VMIX_DATA_STREAM");
```

#### Approach B: Pub/Sub (Recommended)

```cpp
// Publisher (C++)
redis.publish("vmix_detections", jsonData);

// Subscriber (vMix script or bridge)
auto sub = redis.subscriber();
sub.subscribe("vmix_detections");
sub.consume();
```

### Data Serialization

Use a fast JSON library like RapidJSON:

```cpp
#include "rapidjson/document.h"
#include "rapidjson/writer.h"

StringBuffer buffer;
Writer<StringBuffer> writer(buffer);

writer.StartObject();
writer.Key("detections");
writer.StartArray();

for (auto& det : detections) {
    writer.StartObject();
    writer.Key("cameraID");
    writer.Int(det.cameraID);
    writer.Key("x");
    writer.Double(det.x);
    // ... more fields
    writer.EndObject();
}

writer.EndArray();
writer.EndObject();

std::string json = buffer.GetString();
```

## 6. Multi-Threading Strategy

### Thread Responsibilities

```
Main Thread:
  - DirectX 11 device/context management
  - Rendering loop (apply pixel shader)
  - Spout texture sending
  - Window message pump

Capture Threads (per DeckLink):
  - Frame arrival callbacks
  - Queue management
  - Minimal processing only

YOLO Worker Thread:
  - Dequeue frames from all cameras
  - Batch preparation
  - TensorRT inference
  - Post-processing (NMS)

Redis Worker Thread:
  - Read detection array
  - JSON serialization
  - Redis publishing
  - 60Hz timing
```

### Synchronization

Use lock-free queues where possible:

```cpp
#include <concurrent_queue.h>

concurrency::concurrent_queue<IDeckLinkVideoInputFrame*> frameQueue;

// Producer (callback)
frameQueue.push(frame);

// Consumer (worker thread)
IDeckLinkVideoInputFrame* frame;
if (frameQueue.try_pop(frame)) {
    ProcessFrame(frame);
}
```

## 7. Performance Optimization Tips

### GPU Memory Management

- Pre-allocate all textures at startup
- Use texture pools to avoid allocations during runtime
- Monitor VRAM usage with GPU-Z or similar tools

### CPU Optimization

- Pin worker threads to specific CPU cores
- Use NUMA-aware allocations on Threadripper
- Minimize mutex contention with lock-free structures

### PCIe Bandwidth

- Ensure DeckLink cards are in x8 or x16 slots
- Check BIOS settings for PCIe bifurcation
- Monitor PCIe bandwidth with tools like GPU-Z

### DirectX Best Practices

- Batch state changes
- Minimize render target switches
- Use asynchronous queries for profiling
- Enable debug layer during development only

## 8. Debugging and Profiling

### Tools

- **NVIDIA Nsight Graphics**: Profile GPU performance
- **PIX for Windows**: DirectX debugging and profiling
- **Visual Studio Profiler**: CPU profiling
- **GPU-Z**: Real-time GPU monitoring

### Common Issues

**Dropped Frames**:
- Check frame queue sizes
- Verify GPU isn't thermal throttling
- Ensure worker threads aren't blocking

**High Latency**:
- Profile pixel shader execution time
- Check Spout receiver configuration in vMix
- Verify DirectX device is using discrete GPU

**Memory Leaks**:
- Ensure proper Release() calls on COM objects
- Check frame reference counting
- Monitor memory usage over time

## Next Steps

1. Complete SDK integrations (Blackmagic, Spout, TensorRT)
2. Implement buffer pooling for zero-copy
3. Add performance counters and telemetry
4. Create test harness for benchmarking
5. Optimize batch sizes for your specific GPU
