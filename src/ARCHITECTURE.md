# Architecture Decisions Document

This document records the key architectural decisions made for the Visual Intelligence Bypass (VIB) system based on the contexto.MD conversation with Gemini AI.

## Core Philosophy

**"When working, performance is what matters, not personal comfort"**

This principle drives every technical decision in the system. We optimize for hardware utilization and minimal latency, even when it means more complex implementation.

## Decision 1: Zero-Copy DMA Architecture

### Context
Standard video capture involves multiple memory copies:
1. Capture card → System RAM
2. System RAM → GPU VRAM
3. Multiple intermediate copies for processing

At 4K@60fps with 12 cameras, this creates an unacceptable bottleneck.

### Decision
Implement custom `IDeckLinkMemoryAllocator` to provide GPU-visible memory directly to capture cards.

### Rationale
- **Eliminates CPU overhead**: No memcpy operations in main data path
- **PCIe efficiency**: Data travels directly from capture card to GPU via DMA
- **Scalability**: Enables 12x 4K@60fps on single system
- **Latency**: Sub-frame latency achievable

### Trade-offs
- **Complexity**: Requires deeper understanding of memory management
- **Platform-specific**: DirectX 11 and Windows-specific implementation
- **Debugging**: Harder to trace data flow issues

### Implementation
```cpp
class CustomAllocator : public IDeckLinkMemoryAllocator {
    void* AllocateBuffer(unsigned int bufferSize) {
        // Provide GPU-pinned memory pointer
        return pinnedGPUMemory;
    }
};
```

## Decision 2: GPU Pixel Shader for Color Conversion

### Context
Blackmagic cards output YUV 4:2:2 (UYVY format). Both vMix and YOLO need RGBA.

### Decision
Use HLSL pixel shader to convert YUV → RGB entirely on GPU.

### Rationale
- **Parallelism**: RTX 5080 can process 8.3M pixels (4K) simultaneously
- **Speed**: Conversion takes < 1ms vs potentially 10-20ms on CPU
- **Efficiency**: Data stays on GPU for subsequent processing
- **Quality**: Hardware-accelerated BT.709 conversion

### Alternative Considered
CPU-based conversion using threading
- Rejected: Would consume significant Threadripper cores
- Would create synchronization bottlenecks
- Slower than GPU approach

### Implementation
See `src/shaders/ColorConversion.hlsl`

## Decision 3: NDI for vMix Integration

### Context
Need to get video into vMix with minimal latency. vMix has native NDI support but does NOT support Spout natively.

### Decision
Use NDI (Network Device Interface) for video output to vMix.

### Rationale
- **Native vMix support**: vMix has built-in NDI receiver (no plugins needed)
- **Zero-copy with async callbacks**: NDI SDK supports async completion callbacks for zero-copy sending
- **UYVY format support**: NDI accepts UYVY directly (native DeckLink format), avoiding color conversion
- **Network transparent**: Works across machines if needed (future scalability)
- **Industry standard**: Widely used in broadcast and streaming

### Implementation Details
- Use `NDIlib_send_send_video_async_v2` with completion callbacks for zero-copy
- Send UYVY format directly (vMix converts to 32-bit float 4:4:4 internally)
- Per-channel CUDA events for GPU->CPU transfer synchronization
- Pinned memory (cudaMallocHost) for efficient DMA transfer

### Performance Characteristics
- Latency: ~1-2ms per frame (GPU transfer + async send)
- CPU usage: Minimal (async sending, no encoding)
- Format: UYVY 4:2:2 (matches DeckLink output)

### vMix Configuration
For optimal performance with 12 cameras:
- Enable "High Input Performance Mode" (requires GPU with >3GB VRAM)
- Disable "Show preview thumbnails for NDI sources"
- NDI sources appear as "VIB_CAM_01" through "VIB_CAM_12"

### Previous Decision (Deprecated)
Spout was originally considered but rejected because:
- vMix does NOT have native Spout support
- Would require third-party plugin or workaround
- NDI provides equivalent performance with native support

## Decision 4: Redis for Data Exchange

### Context
YOLO detections need to reach vMix for graphics overlay, but vMix and C++ app run independently.

### Decision
Use Redis as in-memory data bus between C++ and vMix.

### Rationale
- **Decoupling**: C++ and vMix don't need to know about each other
- **Speed**: Sub-millisecond access times (in RAM)
- **Flexibility**: Other consumers can read data independently
- **Reliability**: If vMix crashes, C++ continues working
- **Debugging**: Can inspect data with redis-cli

### Alternatives Considered

**Direct socket communication**
- Rejected: Tight coupling between apps
- Requires protocol design and error handling
- Blocks if receiver is slow

**Shared memory**
- Rejected: Complex synchronization required
- Platform-specific implementation
- Harder to debug

**File-based**
- Rejected: Too slow (disk I/O)
- File locking issues

### Implementation
```cpp
// C++ Producer (60Hz)
redis.set("VMIX_DATA_STREAM", jsonData);
redis.publish("vmix_update", "new_data");

// vMix Consumer
redis.subscribe("vmix_update", handleUpdate);
```

## Decision 5: Async Worker for Redis Updates

### Context
Redis updates must not block video processing pipeline.

### Decision
Dedicated worker thread reads detection array and publishes at 60Hz.

### Rationale
- **Non-blocking**: Video processing never waits for Redis
- **Synchronized**: 60Hz matches frame rate for smooth updates
- **Simple**: Set-and-forget architecture
- **Testable**: Can disable Redis without affecting video

### Implementation
```cpp
void RedisWorker::WorkerLoop() {
    while (running) {
        // Read detection array
        auto detections = GetDetections();
        
        // Serialize and publish
        redis.set("data", SerializeJSON(detections));
        
        // Sleep to maintain 60Hz
        Sleep(16ms);
    }
}
```

## Decision 6: Batch Processing for YOLO

### Context
With multiple cameras, running inference sequentially would be slow.

### Decision
Batch multiple camera frames into single TensorRT inference call.

### Rationale
- **GPU utilization**: Maximizes Tensor Core usage
- **Throughput**: Process 4 cameras in ~same time as 1
- **Efficiency**: Better memory access patterns
- **Scalability**: Handles 12 cameras without linear time increase

### Implementation
```cpp
// Instead of:
for (auto camera : cameras) {
    yolo.Process(camera.texture);  // Sequential
}

// Do this:
yolo.ProcessBatch(getAllTextures());  // Parallel
```

## Decision 7: Multi-threaded Architecture

### Context
System has multiple independent workloads that can run concurrently.

### Decision
```
Main Thread: DirectX rendering, Spout output
Capture Threads: Frame arrival callbacks (lightweight)
YOLO Worker: Batch inference processing
Redis Worker: Data publishing
```

### Rationale
- **Parallelism**: Utilizes Threadripper's many cores
- **Isolation**: Failures in one thread don't crash others
- **Performance**: No thread waits unnecessarily
- **Scalability**: Easy to add more workers

### Synchronization Strategy
- Lock-free queues where possible
- Minimal mutex usage
- Atomic flags for state
- Thread-local storage for frame data

## Decision 8: DirectX 11 over OpenGL

### Context
Both APIs can work with GPU, need to choose one.

### Decision
Use DirectX 11 as primary graphics API.

### Rationale
- **vMix native**: vMix uses DirectX internally
- **Spout support**: Spout has excellent DX11 support
- **Windows optimized**: Better driver support on Windows
- **TensorRT integration**: Easier CUDA/DX interop
- **Blackmagic SDK**: Examples use DirectX

### Alternative
OpenGL
- Cross-platform (not needed here)
- Older Spout versions were OpenGL-only (no longer true)

## Decision 9: C++20 as Language Standard

### Context
Need modern C++ features for clean code.

### Decision
Use C++20 language standard.

### Rationale
- **Performance**: Zero-cost abstractions
- **Concurrency**: Better threading primitives
- **Memory safety**: Smart pointers, RAII
- **Compatibility**: Works with all required SDKs

### Key Features Used
- `std::thread` for workers
- `std::atomic` for lock-free operations
- `std::unique_ptr` for resource management
- `std::chrono` for precise timing

## Decision 10: No Frame Skipping

### Context
When system is under load, could drop frames to maintain framerate.

### Decision
Never intentionally skip frames. If can't keep up, report error.

### Rationale
- **Data integrity**: Every frame analyzed by YOLO
- **Debugging**: Easier to identify performance issues
- **Quality**: vMix users expect smooth video
- **Philosophy**: Fix performance issues, don't hide them

### Implementation
- Monitor queue depths
- Log warnings when queues grow
- Automatic detection of dropped frames by hardware

## Lessons from contexto.MD

### Evolution of Understanding

The conversation revealed important insights:

1. **vMix's YUV handling**: vMix receives YUV but converts to RGB internally using shaders - we do the same
2. **Custom allocator necessity**: Standard callbacks can't achieve 12x 4K@60fps
3. **Redis simplicity**: Turned complex synchronization into simple key-value operations
4. **Batch processing importance**: GPU architecture rewards batching

### Final Architecture

The final two responses in contexto.MD crystallized the architecture:

1. **Zero-Copy DMA** with `IDeckLinkMemoryAllocator`
2. **GPU Pixel Shader** for YUV→RGB conversion
3. **Spout Bypass** for immediate vMix delivery
4. **Batch YOLO** inference with TensorRT
5. **Redis Bridge** for async data updates

This architecture achieves:
- Sub-frame latency to vMix
- Real-time AI on all cameras
- Scalability to 12+ streams
- Clean separation of concerns

## Future Considerations

### Potential Improvements

**Multi-GPU Support**
- Split cameras across multiple GPUs
- Even higher throughput
- More complex memory management

**Hardware Encoding**
- Add H.264/HEVC encoding for remote delivery
- NVENC for GPU-accelerated encoding
- Minimal impact on main pipeline

**Dynamic Resolution**
- Adjust quality based on available resources
- Maintain framerate during load spikes
- More complex than fixed resolution

### Non-Goals

**Cross-platform support**: Windows-only is acceptable for target hardware
**Software capture**: Focus on hardware capture cards only
**Real-time editing**: That's vMix's job, we just deliver the video

## Conclusion

Every architectural decision in VIB prioritizes performance and scalability. The system is designed to maximize the capabilities of professional hardware (Threadripper, RTX 5080, DeckLink) rather than accommodate lower-end systems.

This architecture is validated by the contexto.MD conversation where each component was carefully analyzed for performance characteristics and real-world feasibility.
