# SphereVerifier - Ball Verification System

## Overview

The `SphereVerifier` component provides automated verification of ball/sphere positions for race setup and validation. It integrates with the `InferenceEngine` for detection and `VideoHubClient` for camera routing.

## Features

- **Presence Check**: Verify that all expected spheres are in position
- **Position Snapshot**: Capture current positions of all detected spheres
- **Arrival Order**: Track the order spheres cross the finish line *(stub)*
- **Checkpoint Pass**: Monitor spheres passing through defined zones *(stub)*

## Architecture

```
┌──────────────────────────────────────────────────┐
│              SphereVerifier                       │
├──────────────────────────────────────────────────┤
│  VerificationMode:                               │
│  • PRESENCE_CHECK    - Count verification        │
│  • POSITION_SNAPSHOT - Position capture          │
│  • ARRIVAL_ORDER     - Sequential arrival track  │
│  • CHECKPOINT_PASS   - Zone monitoring           │
└──────────────────────────────────────────────────┘
           │                    │
           ▼                    ▼
    ┌─────────────┐      ┌─────────────┐
    │ VideoHub    │      │ Inference   │
    │ (routing)   │      │ Engine      │
    └─────────────┘      └─────────────┘
```

## Usage

### C++ API

#### Basic Usage

```cpp
#include "verification/SphereVerifier.h"

// Create verifier with dependencies
SphereVerifier verifier(&videoHub, &inferenceEngine);

// Check if all spheres are present
auto result = verifier.CheckPresence(1, 10);  // Camera 1, expect 10 spheres
if (result.success) {
    Logger::Info("All " + std::to_string(result.spheresDetected) + " spheres detected!");
    for (int id : result.sphereIDs) {
        Logger::Info("  Sphere " + std::to_string(id));
    }
}
```

#### Presence Check (RunPhase2 Example)

```cpp
bool RunPhase2(VideoHubClient& videoHub, InferenceEngine& inferenceEngine, int targetSpheres) {
    Verification::SphereVerifier verifier(&videoHub, &inferenceEngine);
    auto result = verifier.CheckPresence(1, targetSpheres);  // CAM_01
    
    if (!result.success) {
        Logger::Error("[SCENE ERROR]: " + result.errorMessage);
        return false;
    }
    
    Logger::Info("Fase 2 completada: " + std::to_string(result.spheresDetected) + " esferas OK");
    return true;
}
```

#### Position Capture

```cpp
// Capture starting positions
auto startResult = verifier.CapturePositions(1);  // Camera 1
if (startResult.success) {
    Logger::Info("Captured " + std::to_string(startResult.spheresDetected) + " starting positions");
    for (const auto& pos : startResult.positions) {
        Logger::Info("  Sphere " + std::to_string(pos.sphereID) + 
                    " at (" + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ")");
    }
}
```

#### Advanced Configuration

```cpp
Verification::VerificationConfig config;
config.cameraID = 1;
config.expectedSpheres = 10;
config.timeoutMs = 10000;  // 10 seconds
config.confidenceThreshold = 0.7f;
config.consecutiveFramesRequired = 5;  // Require 5 consecutive stable frames

auto result = verifier.Execute(Verification::VerificationMode::PRESENCE_CHECK, config);
```

### Choreography Integration

The `SphereVerifier` integrates with the choreography system through three new event types:

#### Event Types

| Event Type | Description | Parameters |
|------------|-------------|------------|
| `SpherePresenceCheck` | Verify presence of spheres | `cameraID`, `expectedSpheres`, `timeoutMs` |
| `SpherePositionCapture` | Capture sphere positions | `cameraID`, `timeoutMs` |
| `SphereArrivalWait` | Wait for sphere arrivals | `cameraID`, `expectedSpheres`, `timeoutMs` |

#### JSON Script Example

```json
{
  "name": "Race Setup Verification",
  "events": [
    {
      "type": "Comment",
      "text": "=== Verify starting positions ==="
    },
    {
      "type": "SpherePresenceCheck",
      "cameraID": 1,
      "expectedSpheres": 10,
      "timeoutMs": 5000,
      "mode": "presence"
    },
    {
      "type": "SpherePositionCapture",
      "cameraID": 1,
      "timeoutMs": 3000,
      "mode": "positions"
    },
    {
      "type": "Comment",
      "text": "=== Race complete, capture finish ==="
    },
    {
      "type": "SpherePositionCapture",
      "cameraID": 12,
      "timeoutMs": 3000,
      "mode": "positions"
    }
  ]
}
```

#### C++ Choreography Usage

```cpp
ChoreographyEngine choreography(&vmixController, &sceneManager);
choreography.SetSphereVerifier(&verifier);

if (choreography.Load("config/choreography/race_setup_verification.json")) {
    choreography.Start();
}
```

## API Reference

### VerificationMode Enum

- `PRESENCE_CHECK` - Verify expected number of spheres present
- `POSITION_SNAPSHOT` - Capture current positions
- `ARRIVAL_ORDER` - Track arrival sequence *(not yet implemented)*
- `CHECKPOINT_PASS` - Monitor zone passage *(not yet implemented)*

### VerificationConfig Structure

```cpp
struct VerificationConfig {
    int expectedSpheres = 10;           // Number of spheres expected
    int cameraID = 1;                   // Camera to use (1-12)
    int timeoutMs = 5000;               // Maximum wait time
    float confidenceThreshold = 0.6f;   // Min confidence for detection
    Rect checkpointZone;                // Zone for CHECKPOINT_PASS
    int sampleFrames = 30;              // Frames to sample
    int consecutiveFramesRequired = 3;  // Consecutive stable frames
};
```

### VerificationResult Structure

```cpp
struct VerificationResult {
    bool success;                       // True if successful
    int spheresDetected;                // Number detected
    std::vector<int> sphereIDs;         // Detected sphere IDs (sorted)
    std::vector<SpherePosition> positions; // Sphere positions
    std::vector<int> arrivalOrder;      // Arrival sequence (ARRIVAL_ORDER)
    std::string errorMessage;           // Error description
    int64_t timestamp;                  // Result timestamp
    int cameraUsed;                     // Camera used
    int framesProcessed;                // Frames processed
};
```

### SpherePosition Structure

```cpp
struct SpherePosition {
    int sphereID;        // Ball identifier (1-10)
    float x;             // Normalized X position [0.0, 1.0]
    float y;             // Normalized Y position [0.0, 1.0]
    float confidence;    // Detection confidence [0.0, 1.0]
    int64_t timestamp;   // Detection timestamp
};
```

## Stub Mode

When the `InferenceEngine` is in stub mode (real TensorRT not loaded), the `SphereVerifier` operates in stub mode and generates simulated detections for testing. This allows the system to be tested without requiring actual ball detection.

Stub mode generates 10 balls in a 5x2 grid pattern with confidence values of 0.85-0.95.

## Error Handling

The `SphereVerifier` returns detailed error messages in the `VerificationResult`:

```cpp
auto result = verifier.CheckPresence(1, 10);
if (!result.success) {
    Logger::Error("Verification failed: " + result.errorMessage);
    Logger::Info("Detected: " + std::to_string(result.spheresDetected) + " spheres");
}
```

Common errors:
- "Failed to route camera X" - VideoHub routing failed
- "Sphere count mismatch. Expected X, detected Y" - Wrong number of spheres
- "No spheres detected in snapshot" - No detections found
- "Failed to detect stable spheres" - Timeout waiting for stable detection
- "SphereVerifier not ready" - Missing dependencies

## Future Enhancements

### Arrival Order Mode (Planned)

Track spheres as they cross the finish line:

```cpp
auto result = verifier.WaitForArrivals(12, 10, 60000);  // Camera 12, 10 spheres, 60s timeout
if (result.success) {
    Logger::Info("Arrival order:");
    for (size_t i = 0; i < result.arrivalOrder.size(); i++) {
        Logger::Info("  " + std::to_string(i+1) + ". Sphere " + 
                    std::to_string(result.arrivalOrder[i]));
    }
}
```

### Checkpoint Pass Mode (Planned)

Monitor spheres passing through a defined zone:

```cpp
VerificationConfig config;
config.cameraID = 6;
config.checkpointZone = Rect(0.3f, 0.2f, 0.4f, 0.6f);  // x, y, width, height

auto result = verifier.Execute(VerificationMode::CHECKPOINT_PASS, config);
```

## Integration with Main System

The `SphereVerifier` is designed to integrate into the main VIB system:

1. **Startup**: Initialize alongside other components
2. **Race Setup**: Use `CheckPresence` to verify all balls before race
3. **Position Capture**: Use `CapturePositions` at start/finish
4. **Choreography**: Embed verification in automated race scripts

See `config/choreography/race_setup_verification.json` for a complete example.

## Performance Notes

- Presence check typically completes in 1-3 seconds with stable detections
- Position snapshot captures ~30 frames in under 2 seconds
- Stub mode has negligible performance impact
- Real mode performance depends on `InferenceEngine` batch processing

## Dependencies

- `VideoHubClient` - For camera routing
- `InferenceEngine` - For ball detection
- `Logger` - For diagnostic logging

## Files

- `SphereVerifier.h` - Header with API definitions
- `SphereVerifier.cpp` - Implementation
- `README.md` - This documentation
