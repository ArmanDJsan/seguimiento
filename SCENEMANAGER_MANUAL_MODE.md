# SceneManager Manual Mode Implementation

## Overview
This document describes the implementation of Auto/Manual mode functionality for the SceneManager system, allowing operators to manually control camera group configurations and selections.

## Features Implemented

### 1. Configuration (config.json)
- **mode**: "auto" or "manual" (default: "auto")
- **manual_keys**: Key mappings for manual control
  - `toggle_mode`: "M" - Toggle between AUTO/MANUAL
  - `config_select`: ["F1", "F6", "F7"] - Select config_a/b/c
  - `group_select`: "G" - Toggle between G1_G4 and G5_G8
  - `camera_select`: ["1", "2", "3", "4"] - Select camera within group

### 2. SceneManager.h Additions

#### Enumerations
- **SceneMode**: AUTO, MANUAL
- **ActiveGroup**: G1_G4, G5_G8

#### Structures
- **ManualState**: Tracks active config, group, and camera selection
- **ManualKeysConfig**: Stores key mappings for manual controls
- **SceneManagerConfig**: Extended with mode and manualKeys fields

#### New Methods
- `SetMode(SceneMode mode)`: Switch between AUTO/MANUAL
- `GetMode()`: Get current mode
- `SelectConfig(int configIndex)`: Manually select configuration (0-2)
- `SelectGroup(ActiveGroup group)`: Select specific group
- `ToggleGroup()`: Toggle between G1_G4 ↔ G5_G8
- `SelectCameraInGroup(int index)`: Select camera (0-3) within active group
- `GetManualState()`: Get current manual state
- `GetActiveCameraID()`: Calculate active camera ID in manual mode

### 3. SceneManager.cpp Implementation

#### Constructor
- Initializes m_mode from config
- Initializes m_manualState with defaults (config 0, G1_G4, camera 0)
- Copies manual keys configuration

#### UpdateLeaderPosition
- Now checks mode and skips evaluation in MANUAL mode
- Only performs automatic configuration switching in AUTO mode

#### Manual Mode Methods
All methods include:
- Thread safety via std::lock_guard
- Validation of parameters
- Logging of state changes
- Mode checking (warnings if called in wrong mode)

### 4. main.cpp Integration

#### Configuration Parsing
- Parses `mode` field (validates "auto"/"manual")
- Parses `manual_keys` section with all key mappings
- Validates array sizes (3 configs, 4 cameras)
- Logs parsed configuration for verification

#### Keyboard Handling
Added debounced key handlers for:
- **M**: Toggle AUTO/MANUAL mode
- **F1**: Select config_a (index 0)
- **F6**: Select config_b (index 1)
- **F7**: Select config_c (index 2)
- **G**: Toggle between G1_G4 and G5_G8
- **1-4**: Select camera within active group

Note: F2/F3 remain for emergency camera throttling as before.

#### Status Logging
Periodic 10-second status now includes:
- **AUTO mode**: "[AUTO] Current config: config_name"
- **MANUAL mode**: "[MANUAL] Config: config_b | Group: G5_G8 | Cameras: [4,5,6,7] | Active: CAM_05"

### 5. Thread Safety
- All new methods use std::lock_guard<std::mutex>
- Prevented recursive locking in GetActiveCameraID
- Manual state access is properly synchronized

## Usage

### Starting the System
The system starts in the mode specified in config.json (default: AUTO).

### AUTO Mode Operation
1. System automatically switches configurations based on leader X position
2. Configurations trigger at their defined thresholds (25m, 50m, 75m)
3. Leapfrogging logic operates normally

### MANUAL Mode Operation
1. Press **M** to switch to MANUAL mode
2. Use **F1/F6/F7** to select configurations (config_a/b/c)
   - Applies all 8 camera routings for selected config
3. Use **G** to toggle focus between G1_G4 and G5_G8 groups
   - This is conceptual - doesn't change routing, just tracking context
4. Use **1-4** to select specific camera within active group
   - Logs which camera is "active" for operator awareness

### Returning to AUTO Mode
Press **M** again to return to AUTO mode. The system resumes automatic configuration switching based on leader position.

## Technical Details

### Configuration Application
- **SelectConfig**: Applies full 8-camera routing via ApplyGroupConfig()
- **ToggleGroup**: Only updates manual state, no routing changes
- **SelectCameraInGroup**: Updates selection tracking, no routing changes

### Camera ID Calculation
In MANUAL mode, the active camera ID is calculated as:
```
activeGroup == G1_G4 ? config.slotsG1_G4[selectedIndex] 
                     : config.slotsG5_G8[selectedIndex]
```

### Validation
- Config indices validated: 0-2
- Camera indices validated: 0-3
- Mode checked before executing manual operations
- Invalid calls logged with warnings

## Testing Recommendations

1. **AUTO Mode**: Verify automatic switching at thresholds
2. **Mode Toggle**: Press M, verify transition logged
3. **Config Selection**: Test F1, F6, F7 in MANUAL mode
4. **Group Toggle**: Press G multiple times, verify alternation
5. **Camera Selection**: Press 1-4, verify camera ID logging
6. **Return to AUTO**: Press M, verify normal operation resumes

## Key Bindings Summary

| Key | Function | Mode |
|-----|----------|------|
| M | Toggle AUTO/MANUAL | Both |
| F1 | Select config_a | MANUAL |
| F6 | Select config_b | MANUAL |
| F7 | Select config_c | MANUAL |
| G | Toggle G1_G4/G5_G8 | MANUAL |
| 1-4 | Select camera in group | MANUAL |
| F2 | Emergency: Reduce to 2 cameras | Both |
| F3 | Emergency: Restore to 4 cameras | Both |
| F4 | Emergency stop | Both |
| F5 | Reset telemetry | Both |
| ESC | Exit to menu | Both |

## Notes

- F2/F3 were already in use for emergency camera throttling, so we used F6/F7 for config_b/c
- The config.json uses F1, F6, F7 for the three configurations
- All manual operations are ignored in AUTO mode (with warnings logged)
- Switching between modes preserves the current configuration state
