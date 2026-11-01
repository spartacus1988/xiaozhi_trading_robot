# Touch Sensor Low Level Driver

## Overview

The Touch Sensor Low Level Driver provides a unified interface for touch sensor hardware across different ESP32 platforms (ESP32, ESP32-S2, ESP32-S3, ESP32-P4). This driver abstracts platform-specific differences and offers configurable sampling periods for different use cases.

## Key Features

- **Multi-platform support**: ESP32, ESP32-S2, ESP32-S3, ESP32-P4
- **Configurable sampling period**: Adjust touch sensor sampling frequency based on application needs
- **Interrupt and polling modes**: Support for both callback-driven and polling-based data collection
- **Channel management**: Support for multiple touch channels with individual configuration
- **Memory management**: Automatic allocation and cleanup of resources

## Configuration

### Basic Configuration

```c
touch_lowlevel_config_t config = {
    .channel_num = 3,
    .channel_list = (uint32_t[]){8, 12, 10},
    .channel_type = NULL,  // Use default TOUCH type
    .sample_period_ms = 0, // Use platform defaults
};
```

### Configurable Sampling Period

The `sample_period_ms` parameter allows you to control the overall sampling period for all channels:

- **`sample_period_ms = 0`**: Use automatic defaults based on channel count
  - ≤ 8 channels: 11ms (90.9 Hz)
  - > 8 channels: 17ms (58.8 Hz)

- **`sample_period_ms > 0`**: Custom sampling period in milliseconds
  - Fast response: `sample_period_ms = 10` (100 Hz)
  - Balanced: `sample_period_ms = 50` (20 Hz)
  - Power efficient: `sample_period_ms = 100` (10 Hz)
  - Very low power: `sample_period_ms = 200` (5 Hz)

### Example Configurations

#### Default Configuration (Automatic Based on Channel Count)
```c
touch_lowlevel_config_t config = {
    .channel_num = 2,
    .channel_list = (uint32_t[]){8, 12},
    .sample_period_ms = 0,  // Auto: 11ms for ≤8 channels, 17ms for >8 channels
};
```

#### Fast Response Configuration
```c
touch_lowlevel_config_t config = {
    .channel_num = 2,
    .channel_list = (uint32_t[]){8, 12},
    .sample_period_ms = 10,  // 10ms = 100Hz sampling
};
```

#### Power Efficient Configuration
```c
touch_lowlevel_config_t config = {
    .channel_num = 2,
    .channel_list = (uint32_t[]){8, 12},
    .sample_period_ms = 200,  // 200ms = 5Hz sampling
};
```

## Usage Example

```c
#include "touch_sensor_lowlevel.h"

void app_main(void) {
    // Configure touch sensor with custom sampling period
    touch_lowlevel_config_t config = {
        .channel_num = 2,
        .channel_list = (uint32_t[]){8, 12},
        .sample_period_ms = 50,  // 50ms sampling period
    };
    
    // Initialize and start
    ESP_ERROR_CHECK(touch_sensor_lowlevel_create(&config));
    ESP_ERROR_CHECK(touch_sensor_lowlevel_start());
    
    // Read data (polling mode)
    uint32_t data[SOC_TOUCH_SAMPLE_CFG_NUM];
    for (int i = 0; i < 10; i++) {
        ESP_ERROR_CHECK(touch_sensor_lowlevel_get_data(8, data));
        printf("Touch data: %lu\n", data[0]);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // Cleanup
    ESP_ERROR_CHECK(touch_sensor_lowlevel_stop());
    ESP_ERROR_CHECK(touch_sensor_lowlevel_delete());
}
```

## Platform-Specific Behavior

### ESP32
- Supports polling mode only (no callback registration)
- Default sampling period: 16ms
- Single sample per channel

### ESP32-S2/S3
- Supports both polling and interrupt modes
- Default sampling period: 20ms
- Single sample per channel
- Proximity sensing support (if available)

### ESP32-P4
- Supports both polling and interrupt modes
- Default sampling period: 20ms
- Three samples per channel (different frequencies)
- Advanced filtering capabilities

## API Reference

### Configuration Structure

```c
typedef struct {
    uint32_t channel_num;                    /*!< Number of channels */
    uint32_t *channel_list;                  /*!< List of channel numbers */
    touch_lowlevel_type_t *channel_type;     /*!< Channel types (optional) */
    uint32_t sample_period_ms;               /*!< Sampling period in milliseconds. If 0, use default values */
    uint32_t proximity_count;                /*!< Proximity count (platform dependent) */
} touch_lowlevel_config_t;
```

### Main Functions

- `touch_sensor_lowlevel_create(config)`: Initialize with configuration
- `touch_sensor_lowlevel_start()`: Start touch sensor operation
- `touch_sensor_lowlevel_stop()`: Stop touch sensor operation
- `touch_sensor_lowlevel_delete()`: Clean up resources
- `touch_sensor_lowlevel_get_data(channel, data)`: Read touch data
- `touch_sensor_lowlevel_register(channel, callback, arg, handle)`: Register callback
- `touch_sensor_lowlevel_unregister(handle)`: Unregister callback

## Notes

- The actual measurement interval per channel is calculated as: `sample_period_ms / channel_count`
- For ESP32-P4, the interval is further divided by `SOC_TOUCH_SAMPLE_CFG_NUM` (3)
- Minimum effective measurement intervals are enforced to ensure hardware limitations are respected
- The driver automatically logs the calculated periods for debugging purposes

