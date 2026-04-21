# Smart Device Capture Reuse in SphereVerifier

## Overview

The SphereVerifier now implements intelligent device capture reuse to prevent conflicts when multiple verification operations attempt to capture from the same camera device simultaneously.

## Problem Statement

Without capture reuse, the following issues could occur:
- **Device conflicts**: Attempting to capture from the same DeckLink device multiple times simultaneously causes errors
- **Redundancy**: Starting a new capture when one is already active wastes resources
- **Race conditions**: Multiple threads trying to initialize the same device can cause instability

## Solution: Smart Reuse Strategy

### Architecture

The SphereVerifier tracks active camera captures using an internal unordered set for O(1) lookup performance:
```cpp
std::unordered_set<int> m_activeCaptures;  // Set of camera IDs currently being captured
```

### Key Methods

#### 1. `IsCameraBeingCaptured(int cameraID)`
Checks if a camera is currently being captured.
```cpp
bool wasAlreadyCapturing = verifier.IsCameraBeingCaptured(cameraID);
```

#### 2. `StartCameraCapture(int cameraID)`
Starts capture for a camera. If already capturing, returns success and reuses the existing stream.
```cpp
if (!verifier.StartCameraCapture(cameraID)) {
    // Handle error
}
```

#### 3. `StopCameraCapture(int cameraID)`
Stops capture for a camera. Should only be called if the caller started the capture.
```cpp
verifier.StopCameraCapture(cameraID);
```

#### 4. `GetActiveCaptures()`
Returns list of camera IDs currently being captured (for debugging/diagnostics).
```cpp
auto activeCaptures = verifier.GetActiveCaptures();
for (int cameraID : activeCaptures) {
    Logger::Debug("Camera " + std::to_string(cameraID) + " is active");
}
```

### Usage Pattern

All verification methods follow this pattern:

```cpp
VerificationResult SphereVerifier::ExecutePresenceCheck(const VerificationConfig& config) {
    // 1. Check if already capturing
    bool wasAlreadyCapturing = IsCameraBeingCaptured(config.cameraID);
    
    // 2. Start capture (reuses if already active)
    if (!StartCameraCapture(config.cameraID)) {
        return error;
    }
    
    // 3. Perform verification operations
    // ... route camera, capture frames, detect spheres ...
    
    // 4. Stop capture ONLY if we started it
    if (!wasAlreadyCapturing) {
        StopCameraCapture(config.cameraID);
    }
    
    return result;
}
```

### Benefits

✅ **No device conflicts**: Prevents "device already in use" errors
✅ **Resource efficiency**: Reuses existing capture streams
✅ **Thread-safe**: All capture management methods are mutex-protected
✅ **Clean lifecycle**: Proper cleanup ensures captures are released when no longer needed
✅ **Transparent**: Existing API remains unchanged, reuse is automatic

## Example Scenarios

### Scenario 1: Sequential Verifications on Same Camera

```cpp
// First verification
auto result1 = verifier.CheckPresence(1, 10);  // Starts capture for camera 1
// Capture for camera 1 is released after result1 completes

// Second verification
auto result2 = verifier.CapturePositions(1);   // Starts new capture for camera 1
// Capture is released after result2 completes
```

### Scenario 2: Concurrent Verifications on Same Camera

```cpp
// Thread 1: Long-running verification
std::thread t1([&]() {
    auto result = verifier.WaitForArrivals(1, 10, 60000);  // 60 second timeout
});

// Thread 2: Quick snapshot during thread 1's operation
std::thread t2([&]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto snapshot = verifier.CapturePositions(1);  // Reuses capture from thread 1
});

t1.join();
t2.join();
// Both operations succeed without conflicts
```

### Scenario 3: Multiple Cameras Simultaneously

```cpp
// Concurrent verifications on different cameras
std::thread t1([&]() { verifier.CheckPresence(1, 10); });
std::thread t2([&]() { verifier.CheckPresence(2, 10); });
std::thread t3([&]() { verifier.CheckPresence(3, 10); });

t1.join();
t2.join();
t3.join();
// Each camera has independent capture, no conflicts
```

## Implementation Details

### Thread Safety

All capture management methods are protected by `m_mutex`:
```cpp
std::lock_guard<std::mutex> lock(m_mutex);
```

This ensures:
- Atomic read/write to `m_activeCaptures`
- Safe concurrent access from multiple threads
- No race conditions during capture start/stop

### Logging

The implementation provides diagnostic logging:
```
SphereVerifier: Reusing active capture for camera 1
SphereVerifier: Started capture for camera 2
SphereVerifier: Stopped capture for camera 1
```

Use `Logger::SetLevel(LogLevel::Debug)` to see capture management details.

## Integration with DeckLinkCapture

While the SphereVerifier tracks *intent* to capture, the actual device management happens in `DeckLinkCapture` class. This smart reuse layer prevents:
- Multiple simultaneous `Initialize()` calls on the same device
- Premature `Stop()` calls while another operation is using the device
- Resource leaks from improperly managed captures

## Future Enhancements

Potential improvements:
1. **Reference counting**: Track how many operations are using each camera
2. **Timeout-based cleanup**: Automatically release stale captures after inactivity
3. **Capture pooling**: Maintain a pool of ready-to-use capture sessions
4. **Performance metrics**: Track capture reuse efficiency

## Testing

To test the capture reuse functionality:

```cpp
// Test 1: Verify reuse detection
bool isCapturing1 = verifier.IsCameraBeingCaptured(1);  // Should be false
verifier.StartCameraCapture(1);
bool isCapturing2 = verifier.IsCameraBeingCaptured(1);  // Should be true
verifier.StopCameraCapture(1);
bool isCapturing3 = verifier.IsCameraBeingCaptured(1);  // Should be false

// Test 2: Verify reuse in verification
auto result1 = verifier.CheckPresence(1, 10);
auto activeCaptures = verifier.GetActiveCaptures();  // Should be empty after completion

// Test 3: Verify concurrent reuse
// (See Scenario 2 above)
```

## Conclusion

The smart device capture reuse feature ensures robust, efficient, and conflict-free camera operations in the SphereVerifier, enabling reliable automated sphere verification across all race phases.
