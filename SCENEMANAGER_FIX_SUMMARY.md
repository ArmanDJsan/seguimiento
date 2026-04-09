# SceneManager Configuration Loading Fix - Implementation Summary

## Overview
This document summarizes the implementation of fixes for the SceneManager configuration loading error identified in the VIB system. The changes improve robustness, validation, error handling, and diagnostics for the SceneManager component.

## Problem Statement
The SceneManager was experiencing configuration loading issues due to:
1. Lack of validation for camera IDs and trigger thresholds
2. No error recovery for transient VideoHub routing failures
3. Insufficient diagnostic logging during configuration parsing
4. Missing error handling for malformed JSON configurations

## Implementation Summary

### Phase 1: Configuration Validation (High Priority) ✅

#### Added Validation Function (`src/core/main.cpp`)
New function `ValidateSceneManagerConfig()` performs comprehensive validation:
- **Empty groups check**: Ensures at least one group configuration exists when enabled
- **Camera ID validation**: Verifies all camera IDs are in valid range [1, 12]
- **Threshold ordering**: Warns if trigger thresholds are not in ascending order
- **Complete validation**: Checks all 8 cameras in each group (G1-G4 and G5-G8)

```cpp
bool ValidateSceneManagerConfig(const Config& config) {
    // Validates:
    // - Groups not empty when enabled
    // - All camera IDs in range [1, 12]
    // - Trigger thresholds in ascending order
    // Returns: true if valid, false otherwise
}
```

#### Enhanced Configuration Parsing
- **Try-catch protection**: Wraps group parsing to handle JSON exceptions gracefully
- **Missing field handling**: Provides warnings and defaults for missing required fields
- **Per-group logging**: Logs each group's configuration as it's parsed
- **Sorted order logging**: Logs final configuration order after sorting by threshold

Example log output:
```
[INFO] SceneManager config: Parsed group 'config_a' - G1-G4=[1,2,3,12], G5-G8=[4,5,6,7], threshold=25.0m
[INFO] SceneManager config: Sorted 3 groups by threshold: config_a(25.0m) -> config_b(50.0m) -> config_c(75.0m)
```

#### Validation Integration
- Validation called immediately after config loading in `main()`
- If validation fails with SceneManager enabled, it's automatically disabled
- Clear error messages guide users to fix configuration issues

### Phase 2: Enhanced Error Handling (High Priority) ✅

#### Improved `Initialize()` Method (`src/scene/SceneManager.cpp`)
Enhanced validation in the initialization phase:
- **Camera ID bounds checking**: Validates each camera ID against `kMaxCameras` (12)
- **Detailed error messages**: Identifies specific slot and camera causing validation failure
- **Configuration summary**: Logs complete mapping of slots to cameras for all groups
- **Early failure detection**: Prevents initialization with invalid configuration

Example initialization log:
```
[INFO] SceneManager: Configuration summary:
[INFO]   Group 0 ('config_a'): G1-G4=[CAM_01,CAM_02,CAM_03,CAM_12], G5-G8=[CAM_04,CAM_05,CAM_06,CAM_07], trigger=25.0m
[INFO]   Group 1 ('config_b'): G1-G4=[CAM_08,CAM_09,CAM_10,CAM_12], G5-G8=[CAM_04,CAM_05,CAM_06,CAM_07], trigger=50.0m
[INFO] SceneManager: Initialized with 2 group configurations, mute timeout=200ms
```

#### Enhanced `SendVideoHubRouting()` Method
Implemented robust retry logic for VideoHub communication:
- **Retry attempts**: 3 attempts with 50ms delay between retries
- **Transient failure recovery**: Handles temporary network or VideoHub issues
- **Detailed logging**: Different log levels for each attempt and final outcome
- **Connection validation**: Checks VideoHub connection before attempting routing

Retry behavior:
```cpp
const int maxRetries = 3;
const int retryDelayMs = 50;

for (int attempt = 0; attempt < maxRetries; ++attempt) {
    success = m_videoHub->RouteInputToOutput(slotIndex, cameraName);
    if (success) break;
    if (attempt < maxRetries - 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
    }
}
```

Example retry log:
```
[WARN] SceneManager: Routing attempt 1 failed for CAM_08 to slot G2, retrying...
[INFO] SceneManager: Successfully routed CAM_08 to slot G2 on retry attempt 2
```

### Phase 3: Configuration Parsing Robustness (Medium Priority) ✅

#### Robust JSON Parsing
- **Exception handling**: Try-catch block around entire group parsing section
- **Graceful degradation**: System continues with valid groups even if some fail to parse
- **Default fallbacks**: Uses SceneManagerConfig defaults if JSON section is missing
- **Array size validation**: Checks array sizes before accessing elements

#### Missing Field Handling
For each group configuration field:
- **g1_g4 array**: Warns if missing or insufficient elements, uses defaults
- **g5_g8 array**: Warns if missing or insufficient elements, uses defaults  
- **trigger_threshold**: Warns if missing, uses default value (0.0)

Example warning logs:
```
[WARN] SceneManager config: Group 'config_x' missing g1_g4 field, using defaults
[WARN] SceneManager config: Group 'config_y' g5_g8 array has fewer than 4 elements, using defaults
```

#### Configuration Defaults
If the `scene_manager` section is missing or malformed:
- Default values: enabled=true, mute_timeout_ms=200
- Falls back to SceneManagerConfig constructor defaults (2 groups)
- Warning logged indicating defaults are being used

## Key Improvements

### 1. Fail-Fast Validation
- Configuration errors detected at startup, not during runtime
- Invalid configurations prevented from initializing SceneManager
- Clear error messages guide troubleshooting

### 2. Self-Healing Behavior
- Retry logic handles transient VideoHub communication failures
- Graceful degradation when some configurations are invalid
- System continues operating with valid subset of configuration

### 3. Enhanced Diagnostics
- Detailed logging at every stage of configuration loading
- Clear indication of what was parsed and how it was interpreted
- Summary logs show complete system state after initialization

### 4. Error Context
- Every error message includes specific details (slot, camera, group name)
- Validation messages identify exact location of problem
- Log levels appropriate for severity (Error, Warning, Info, Debug)

## Testing Recommendations

### Test Case 1: Valid Configuration
```json
{
  "scene_manager": {
    "enabled": true,
    "mute_timeout_ms": 200,
    "groups": {
      "config_a": { "g1_g4": [1,2,3,12], "g5_g8": [4,5,6,7], "trigger_threshold": 25.0 },
      "config_b": { "g1_g4": [8,9,10,12], "g5_g8": [4,5,6,7], "trigger_threshold": 50.0 }
    }
  }
}
```
**Expected**: All groups loaded, validation passes, system initializes

### Test Case 2: Invalid Camera IDs
```json
{
  "scene_manager": {
    "enabled": true,
    "groups": {
      "bad_config": { "g1_g4": [1,2,15,20], "g5_g8": [4,5,6,7], "trigger_threshold": 25.0 }
    }
  }
}
```
**Expected**: Validation errors for cameras 15 and 20, SceneManager disabled

### Test Case 3: Missing Required Fields
```json
{
  "scene_manager": {
    "enabled": true,
    "groups": {
      "incomplete": { "g1_g4": [1,2,3,12], "trigger_threshold": 25.0 }
    }
  }
}
```
**Expected**: Warning logged, g5_g8 uses defaults, system continues

### Test Case 4: Missing scene_manager Section
```json
{
  "videohub": { "ip": "192.168.1.50", "port": 9990 }
}
```
**Expected**: Warning logged, SceneManager uses defaults from constructor

### Test Case 5: Out-of-Order Thresholds
```json
{
  "scene_manager": {
    "enabled": true,
    "groups": {
      "config_a": { "g1_g4": [1,2,3,4], "g5_g8": [5,6,7,8], "trigger_threshold": 75.0 },
      "config_b": { "g1_g4": [8,9,10,11], "g5_g8": [1,2,3,4], "trigger_threshold": 25.0 }
    }
  }
}
```
**Expected**: Configs sorted by threshold, warning if not ascending after sort

## Files Modified

### `src/core/main.cpp`
- Added `ValidateSceneManagerConfig()` function (66 lines)
- Enhanced scene_manager parsing section with try-catch and logging (90+ lines)
- Added validation call after config loading (8 lines)
- **Total changes**: ~160 lines added/modified

### `src/scene/SceneManager.cpp`
- Enhanced `Initialize()` method with validation and summary logging (50+ lines)
- Rewrote `SendVideoHubRouting()` with retry logic and better error handling (45+ lines)
- Added `#include <thread>` and `#include <chrono>` for retry delays
- **Total changes**: ~100 lines added/modified

## Memory Storage

Storing key facts about this implementation for future reference:

### SceneManager Configuration Validation
- **Fact**: SceneManager configuration is validated at startup via ValidateSceneManagerConfig() which checks: groups not empty when enabled, all camera IDs in range [1,12], and trigger thresholds in ascending order. Validation failures disable SceneManager with detailed error logs.
- **Context**: Critical for preventing runtime failures and providing clear diagnostic information
- **Location**: src/core/main.cpp:145-207 (validation function), src/core/main.cpp:624-632 (validation call)

### VideoHub Routing Retry Logic
- **Fact**: SceneManager::SendVideoHubRouting() implements 3-attempt retry logic with 50ms delays to handle transient VideoHub communication failures. Each retry is logged with attempt number and final outcome.
- **Context**: Improves system reliability during network hiccups or VideoHub busy states
- **Location**: src/scene/SceneManager.cpp:284-330

### Configuration Parsing Robustness
- **Fact**: Scene_manager config parsing uses try-catch exception handling, validates array sizes, provides defaults for missing fields, and logs detailed diagnostics for each parsed group. System gracefully degrades if some groups fail to parse.
- **Context**: Prevents config parsing errors from crashing the application
- **Location**: src/core/main.cpp:349-440

## Conclusion

The implementation successfully addresses all high and medium priority items from the original plan:

✅ **Phase 1**: Configuration validation with comprehensive checks and detailed logging  
✅ **Phase 2**: Enhanced error handling with retry logic and improved diagnostics  
✅ **Phase 3**: Robust configuration parsing with exception handling and defaults

The system is now more resilient to configuration errors, provides better diagnostic information, and can recover from transient failures. The extensive logging helps developers and operators quickly identify and resolve issues.

## Next Steps

1. **Testing**: Run test cases with various configuration scenarios
2. **Documentation**: Update user documentation with configuration schema
3. **Monitoring**: Observe system behavior with new logging in production
4. **Phase 4 (Future)**: Add runtime diagnostics and telemetry integration
5. **Phase 5 (Future)**: Create automated configuration validation tests

## References

- Original error file: `error_scenemanger.txt`
- Configuration file: `src/config.json`
- SceneManager header: `src/scene/SceneManager.h`
- VideoHub protocol: Memory stored about VideoHub 0-based indexing
