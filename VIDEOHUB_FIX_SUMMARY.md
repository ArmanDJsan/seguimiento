# VideoHub Input 17 Output 01 Mapping Fix

## Problem Summary
When the executable opens, the VideoHub gets incorrectly mapped with "input 17 output 01" instead of the expected routing configuration.

## Root Causes Identified

### 1. Unconsumed Initial State Data
**Issue**: The Blackmagic VideoHub protocol automatically sends the device's current configuration state (routing table, labels, locks, etc.) immediately after TCP connection establishment. The original `VideoHubClient::Connect()` method did not read or consume this data.

**Impact**: This unconsumed data remained in the socket receive buffer, potentially causing protocol confusion or interference when the first routing command was sent. This could lead to incorrect routing states.

### 2. Indexing Mismatch
**Issue**: The code was using inconsistent indexing conventions:
- VideoHub protocol uses **0-based indexing** (input 0, input 1, ..., input 15, output 0, output 1, ...)
- The original code used **1-based indexing** (CAM_01 = input 1, CAM_02 = input 2, ..., RADAR_04 = input 16)
- Output was correctly set to 0 (`kVideoHubPrimaryOutput = 0`)

**Impact**: When routing commands were sent with 1-based input indices to a device expecting 0-based indices, it could cause off-by-one errors or default to unexpected routes like "input 17 output 01".

## Changes Made

### 1. VideoHubClient.h
- Added private method: `bool ConsumeInitialState();`

### 2. VideoHubClient.cpp
- Added includes for `<thread>` and `<chrono>` to support sleep operations
- Implemented `ConsumeInitialState()` method:
  - Sets socket to non-blocking mode
  - Waits 100ms for initial data to arrive from VideoHub
  - Reads and discards all initial state data (protocol preamble, routing table, etc.)
  - Logs a preview of the first chunk for debugging
  - Restores socket to blocking mode for normal operation
  - Reports total bytes consumed
- Updated `Connect()` method to call `ConsumeInitialState()` after successful connection

### 3. main.cpp
- Updated `BuildInputLookup()` to use **0-based indexing**:
  - `CAM_01` now maps to input `0` (was `1`)
  - `CAM_02` now maps to input `1` (was `2`)
  - ...
  - `CAM_12` now maps to input `11` (was `12`)
  - `RADAR_01` now maps to input `12` (was `13`)
  - `RADAR_02` now maps to input `13` (was `14`)
  - `RADAR_03` now maps to input `14` (was `15`)
  - `RADAR_04` now maps to input `15` (was `16`)
- Updated `ValidateSignalGroup()` to convert 1-based port numbers to 0-based VideoHub inputs:
  - Added conversion: `int videoHubInput = port - 1;`
  - This maintains compatibility with DeckLink which may use 1-based port numbering

### 4. main_test.cpp
- Updated `BuildInputLookup()` to use 0-based indexing (same as main.cpp)
- Updated port routing loop to convert 1-based port numbers to 0-based VideoHub inputs

## Technical Details

### VideoHub Protocol
The Blackmagic VideoHub uses a text-based protocol over TCP (default port 9990). Upon connection:

1. **Server sends (automatically):**
   - `PROTOCOL PREAMBLE:` - Protocol version info
   - `DEVICE PRESENT:` - Device information
   - `VIDEO OUTPUT LOCKS:` - Output lock states
   - `VIDEO OUTPUT ROUTING:` - Current routing configuration
   - `INPUT LABELS:` - Names of inputs
   - `OUTPUT LABELS:` - Names of outputs
   - Additional blocks depending on device capabilities

2. **Client can then send:**
   - Routing commands: `VIDEO OUTPUT ROUTING:\n<output_index> <input_index>\n\n`
   - Label updates: `INPUT LABELS:\n<index> <label>\n\n`
   - Other control commands

### Indexing Convention
- **Inputs and Outputs**: 0-based (0, 1, 2, ..., N-1)
- Example for 16-input device: inputs are 0-15, not 1-16
- Example routing command to route input 0 to output 0:
  ```
  VIDEO OUTPUT ROUTING:
  0 0

  ```

## Expected Behavior After Fix

1. **On Startup**:
   - VideoHub connects successfully
   - Initial state data is consumed and logged (preview shown in logs)
   - Socket is ready for clean command transmission

2. **During Operation**:
   - `RouteInputToOutput(0, "CAM_01")` routes VideoHub input 0 to output 0
   - `RouteInputToOutput(0, "CAM_12")` routes VideoHub input 11 to output 0
   - `RouteInputToOutput(0, "RADAR_04")` routes VideoHub input 15 to output 0
   - No spurious "input 17 output 01" mappings

3. **Logging**:
   - Connection log: "VideoHub connected at <IP>:<PORT>"
   - Initial state log: "VideoHub initial state preview: PROTOCOL PREAMBLE:..."
   - Initial state log: "VideoHub initial state consumed (XXXX bytes)"

## Testing Recommendations

1. **Verify Connection**:
   - Check logs for "VideoHub initial state consumed" message
   - Verify bytes consumed is > 0 (typically 500-2000 bytes)

2. **Verify Routing**:
   - Test routing to CAM_01 through CAM_12
   - Test routing to RADAR_01 through RADAR_04
   - Verify physical routing matches software commands

3. **Verify No Spurious Routes**:
   - Check VideoHub display/web interface
   - Confirm no "input 17" or other out-of-range indices appear

## Files Modified
- `src/control/VideoHubClient.h`
- `src/control/VideoHubClient.cpp`
- `src/core/main.cpp`
- `src/core/main_test.cpp`

## Backward Compatibility
This change modifies the indexing convention but should not break existing functionality because:
1. All routing operations go through the same VideoHubClient methods
2. Name-based routing (e.g., "CAM_01") automatically uses correct indices
3. Port-based routing in ValidateSignalGroup now correctly converts to 0-based

## References
- Blackmagic VideoHub Protocol: Text-based TCP protocol
- Standard port: 9990
- Indexing: 0-based for all inputs and outputs
