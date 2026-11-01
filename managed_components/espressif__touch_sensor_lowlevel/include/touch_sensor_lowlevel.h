/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"
#include "soc/soc_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

// add definitions for linux simulation
#ifndef SOC_TOUCH_SAMPLE_CFG_NUM
#if CONFIG_TOUCH_SENSOR_LOWLEVEL_SIMULATOR_TARGET_ESP32S2S3
#define SOC_TOUCH_SAMPLE_CFG_NUM 1
#elif CONFIG_TOUCH_SENSOR_LOWLEVEL_SIMULATOR_TARGET_ESP32P4
#define SOC_TOUCH_SAMPLE_CFG_NUM 3
#else
#define SOC_TOUCH_SAMPLE_CFG_NUM 1
#endif
#endif

#ifndef SOC_TOUCH_SENSOR_NUM
#if CONFIG_TOUCH_SENSOR_LOWLEVEL_SIMULATOR_TARGET_ESP32S2S3
#define SOC_TOUCH_SENSOR_NUM 14
#elif CONFIG_TOUCH_SENSOR_LOWLEVEL_SIMULATOR_TARGET_ESP32P4
#define SOC_TOUCH_SENSOR_NUM 14
#endif
#endif

#ifndef CONFIG_ESP_LOG
#define CONFIG_ESP_LOG 1
#endif

#ifdef CONFIG_ESP_LOG
#include "esp_log.h"
#define LOWLEVEL_LOGD(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
#define LOWLEVEL_LOGI(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#define LOWLEVEL_LOGW(tag, format, ...) ESP_LOGW(tag, format, ##__VA_ARGS__)
#define LOWLEVEL_LOGE(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
#else
#include <stdio.h>
#define LOWLEVEL_LOGD(tag, format, ...) printf("%s: " format "\n", tag, ##__VA_ARGS__)
#define LOWLEVEL_LOGI(tag, format, ...) printf("%s: " format "\n", tag, ##__VA_ARGS__)
#define LOWLEVEL_LOGW(tag, format, ...) printf("%s: " format "\n", tag, ##__VA_ARGS__)
#define LOWLEVEL_LOGE(tag, format, ...) printf("%s: " format "\n", tag, ##__VA_ARGS__)
#endif

typedef enum {
    TOUCH_LOWLEVEL_STATE_START,
    TOUCH_LOWLEVEL_STATE_STOP,
    TOUCH_LOWLEVEL_STATE_NEW_DATA,
} touch_lowlevel_state_t;

typedef enum {
    TOUCH_LOWLEVEL_TYPE_UNDEFINED = 0,
    TOUCH_LOWLEVEL_TYPE_TOUCH,
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
    TOUCH_LOWLEVEL_TYPE_PROXIMITY,
#endif
} touch_lowlevel_type_t;

typedef struct {
    uint32_t channel_num;
    uint32_t *channel_list;
    touch_lowlevel_type_t *channel_type;
    uint32_t sample_period_ms;
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
    uint32_t proximity_count;
#endif
} touch_lowlevel_config_t;

/**
 * @brief Touch sensor callback function.
 * The state values is a buffer pointer to the touch sensor data. the size of the buffer is SOC_TOUCH_SAMPLE_CFG_NUM * uint32_t.
 *
 */
typedef void(*touch_lowlevel_state_cb_t)(uint32_t channel, touch_lowlevel_state_t state, void *state_values, void *arg);

typedef void *touch_lowlevel_handle_t;

#if CONFIG_IDF_TARGET_ESP32P4

#define TOUCH_SAMPLE_CFG(times, res, cap, coarse_freq_tune, fine_freq_tune) { \
    .div_num = 8, \
    .charge_times = times, \
    .rc_filter_res = res, \
    .rc_filter_cap = cap, \
    .low_drv = fine_freq_tune, \
    .high_drv = coarse_freq_tune, \
    .bias_volt = 5, \
    .bypass_shield_output = false, \
}

/*
// #define TOUCH_SAMPLE_CFG_DEFAULT()      {TOUCH_SAMPLE_CFG(277, 3, 127, 1, 0),\
//                                          TOUCH_SAMPLE_CFG(411, 3, 127, 2, 0),\
//                                          TOUCH_SAMPLE_CFG(623, 3, 127, 5, 0)}
*/

#define TOUCH_SAMPLE_CFG_DEFAULT()      {TOUCH_SAMPLE_CFG(632, 3, 35, 2, 0),\
                                         TOUCH_SAMPLE_CFG(534, 3, 64, 2, 0),\
                                         TOUCH_SAMPLE_CFG(408, 3, 127, 2, 0)}

#define TOUCH_SENSOR_BASIC_CFG_DEFAULT(sample_cfg_number, sample_cfg_ptr) { \
    .power_on_wait_us = 256, \
    .meas_interval_us = 2, \
    .max_meas_time_us = 0, \
    .sample_cfg_num = sample_cfg_number,  \
    .sample_cfg = sample_cfg_ptr,  \
}

#define TOUCH_DEFAULT_FILTER_CONFIG() { \
    .benchmark = { \
        .filter_mode = TOUCH_BM_IIR_FILTER_16, \
        .jitter_step = 4, \
        .denoise_lvl = 1, \
    }, \
    .data = { \
        .smooth_filter = TOUCH_SMOOTH_IIR_FILTER_2, \
        .active_hysteresis = 2, \
        .debounce_cnt = 2, \
    }, \
}
#endif

/**
 * @brief create the touch sensor low-level driver in singleton mode.
 *
 * This function saves the configuration values and context to the internal data structure.
 *
 * @param[in] config Pointer to the configuration structure.
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - ESP_ERR_INVALID_STATE: Touch pad is running, cannot change config
 *      - ESP_ERR_NO_MEM: Memory allocation failed
 */
esp_err_t touch_sensor_lowlevel_create(touch_lowlevel_config_t *config);

/**
 * @brief delete the touch sensor low-level driver.
 *
 * This function frees the memory allocated for the touch sensor.
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: Touch low-level not started
 */
esp_err_t touch_sensor_lowlevel_delete(void);

/**
 * @brief Start the touch sensor low-level driver.
 *
 * This function initializes the touch sensor with all the channels depending on the channel types.
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: No channels configured
 *      - ESP_ERR_INVALID_STATE: Touch low-level already started
 */
esp_err_t touch_sensor_lowlevel_start(void);

/**
 * @brief Stop the touch sensor low-level driver.
 *
 * This function stops the touch sensor.
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: Touch low-level not started
 */
esp_err_t touch_sensor_lowlevel_stop(void);

/**
 * @brief Get data from the touch sensor.
 *
 * This function reads the data from the touch sensor. the data size is SOC_TOUCH_SAMPLE_CFG_NUM * uint32_t.
 *
 * @param[in] channel Channel number.
 * @param[out] data Pointer to store the read data.
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 */
esp_err_t touch_sensor_lowlevel_get_data(uint32_t channel, uint32_t *data);

/**
 * @brief Register a callback function for the touch sensor.
 *
 * This function sets the callback function for the touch sensor.
 *
 * @param[in] channel Channel number.
 * @param[in] cb Callback function.
 * @param[in] arg Argument to be passed to the callback function.
 * @param[out] handle Handle to the registered callback.
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - ESP_ERR_NO_MEM: Memory allocation failed
 *      - ESP_ERR_NOT_SUPPORTED: currently target does not support this feature
 */
esp_err_t touch_sensor_lowlevel_register(uint32_t channel, touch_lowlevel_state_cb_t cb, void *arg, touch_lowlevel_handle_t *handle);

/**
 * @brief Unregister a callback function for the touch sensor.
 *
 * This function removes the callback function for the touch sensor.
 *
 * @param[in] handle Handle to the registered callback.
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - ESP_ERR_NOT_FOUND: Callback not found
 */
esp_err_t touch_sensor_lowlevel_unregister(touch_lowlevel_handle_t handle);

#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/touch_sens.h"
/**
 * @brief Implement the weak function if you want to change the touch sensor default configuration.
 *
 * This function returns the touch sensor configuration.
 *
 * @return touch_sensor_config_t Touch sensor configuration.
 */
touch_sensor_config_t weak_get_touch_sensor_config(void) __attribute__((weak));

/**
 * @brief Implement the weak function if you want to change the touch sensor filter configuration.
 *
 * This function returns the touch sensor filter configuration.
 *
 * @return touch_sensor_filter_config_t Touch sensor filter configuration.
 */
touch_sensor_filter_config_t weak_get_filter_config(void) __attribute__((weak));
#endif

#if CONFIG_IDF_TARGET_LINUX
/**
 * @brief Implement the weak function if you want to change the touch sensor simulation data buffer.
 *
 * This function returns the touch sensor data buffer.
 *
 * @param[in] channel Channel number.
 * @return const uint32_t (*)[4] Touch sensor data buffer.
 */
__attribute__((weak)) const uint32_t (*weak_get_data_buffer(uint32_t channel))[4];

/**
 * @brief Implement the weak function if you want to change the touch sensor data buffer size.
 *
 * This function returns the touch sensor data buffer size.
 *
 * @param[in] channel Channel number.
 * @return uint32_t Touch sensor data buffer size.
 */
__attribute__((weak)) uint32_t weak_get_data_buffer_size(uint32_t channel);
#endif

#ifdef __cplusplus
}
#endif
