# VIB Configuration Guide

## Configuration File Structure

VIB uses a JSON configuration file (`config.json`) to define system parameters, VideoHub routing, and camera/radar assignments.

### Basic Configuration

The configuration file includes the following main sections:

```json
{
  "videohub": {
    "ip": "192.168.1.50",
    "port": 9990
  },
  "esp32": {
    "ip": "192.168.88.114",
    "port": 80,
    "endpoints": {
      "test": "/test",
      "iniciar": "/iniciar",
      "cerrar": "/cerrar",
      "recargar": "/recargar"
    }
  },
  "ports": [...],
  "target_spheres": 10,
  "configurations": {...}
}
```

### VideoHub Configurations

**NEW in v2.0**: The `configurations` section allows you to define multiple routing setups for different scenarios. Each configuration includes camera groups and radar assignments.

#### Configuration Structure

Each configuration (e.g., `config_a`, `config_b`, `config_c`) defines:

- **g1_g4**: Array of camera port indexes for group 1-4
- **g5_g8**: Array of camera port indexes for group 5-8
- **radars**: Array of radar port indexes (ensures radars are properly routed when switching configurations)

#### Example

```json
"configurations": {
  "config_a": {
    "g1_g4": [1, 2, 3, 12],
    "g5_g8": [4, 5, 6, 7],
    "radars": [13, 14, 15, 16]
  },
  "config_b": {
    "g1_g4": [1, 2, 3, 4],
    "g5_g8": [5, 6, 7, 8],
    "radars": [13, 14, 15, 16]
  },
  "config_c": {
    "g1_g4": [9, 10, 11, 12],
    "g5_g8": [1, 2, 3, 4],
    "radars": [13, 14, 15, 16]
  }
}
```

### Radar Routing

**Why Include Radars in Configurations?**

When the VideoHub configuration changes (e.g., switching from `config_a` to `config_b`), the system needs to ensure that radar assignments remain properly routed to their designated groups. By explicitly including radar port indexes in each configuration, the system can:

1. **Verify** that radars are correctly assigned when applying a configuration
2. **Prevent** radar routing issues when the VideoHub state changes
3. **Ensure** consistent radar-to-camera group associations

### Port Definitions

The `ports` array defines all available inputs to the VideoHub:

```json
"ports": [
  { "index": 1, "name": "CAM_01", "role": "stream", "is_output": true },
  { "index": 2, "name": "CAM_02", "role": "stream" },
  ...
  { "index": 13, "name": "RADAR_01", "role": "radar" },
  { "index": 14, "name": "RADAR_02", "role": "radar" },
  { "index": 15, "name": "RADAR_03", "role": "radar" },
  { "index": 16, "name": "RADAR_04", "role": "radar" }
]
```

- **index**: Physical VideoHub port number (1-16)
- **name**: Logical name for the port (e.g., CAM_01, RADAR_01)
- **role**: Port type (`stream` for cameras, `radar` for tracking cameras)
- **is_output**: Optional flag to mark primary output port

### ESP32 Configuration

Controls the physical tracking system:

```json
"esp32": {
  "ip": "192.168.88.114",
  "port": 80,
  "endpoints": {
    "test": "/test",        // Endpoint for mechanical test
    "iniciar": "/iniciar",  // Start tracking
    "cerrar": "/cerrar",    // Stop tracking
    "recargar": "/recargar" // Reload configuration
  }
}
```

### Target Spheres

```json
"target_spheres": 10
```

Defines the expected number of spheres to detect during Phase 2 (scene validation).

## Configuration Loading

The system loads configurations during initialization:

1. **Parse** the JSON file from `config.json`
2. **Validate** all port assignments and network settings
3. **Apply** the default configuration (`config_a`) during Phase 1
4. **Log** configuration details for debugging

### Default Configuration

If no configuration file is found or parsing fails, the system uses these defaults:

- VideoHub IP: `192.168.1.50:9990`
- ESP32 IP: `192.168.88.114:80`
- Target Spheres: `10`
- Configurations: Empty (must be defined in config.json)

## Usage in Code

### Applying a Configuration

To apply a specific VideoHub configuration:

```cpp
Config config = LoadConfig("config.json");
VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);

// Apply config_a
auto it = config.configurations.find("config_a");
if (it != config.configurations.end()) {
    ApplyVideoHubConfiguration(videoHub, it->second, it->first);
}
```

### Accessing Configuration Data

```cpp
// Access camera groups
const auto& g1_g4_cameras = vhConfig.g1_g4;  // std::vector<int>
const auto& g5_g8_cameras = vhConfig.g5_g8;  // std::vector<int>

// Access radar assignments
const auto& radars = vhConfig.radars;  // std::vector<int>
```

## Best Practices

1. **Always include radars**: Every configuration should specify radar port indexes to ensure proper routing
2. **Validate port numbers**: Ensure all camera and radar indexes match your physical VideoHub connections
3. **Test configurations**: Use the system's test mode to verify each configuration before production use
4. **Document changes**: Add comments in your config.json file to explain non-obvious routing decisions
5. **Backup configurations**: Keep backups of working configurations before making changes

## Troubleshooting

### Configuration Not Loading

Check the log output for:
- JSON parsing errors
- File access issues
- Invalid port indexes

### Radars Not Routing

Ensure that:
- Radar indexes in `configurations` match the `ports` array
- Radar cables are properly connected to the specified VideoHub ports
- VideoHub connection is established before applying configurations

### Unknown Source Errors

If you see "Unknown VideoHub source name" errors:
- Verify port indexes match the `ports` array
- Check that all referenced ports are defined in `config.json`
- Ensure port numbers are within the VideoHub's physical range (1-16)

## Migration from Previous Versions

If upgrading from a version without the `configurations` section:

1. Keep your existing `videohub`, `esp32`, `ports`, and `target_spheres` sections
2. Add the new `configurations` section with at least one configuration
3. Include all radar ports (typically 13-16) in each configuration's `radars` array
4. Test the configuration with `[1] TEST DE FUNCIONAMIENTO` before running in production
