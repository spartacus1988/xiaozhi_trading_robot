/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
#include "soc/clk_tree_defs.h"
#include "esp_clk_tree.h"
#else
#include "soc/rtc.h"
#endif
#if CONFIG_TOUCH_SENSOR_DEBUG_GPIO_TRIGGER
#include "driver/gpio.h"
#define DEBUG_NEW_DATA_IO CONFIG_TOUCH_SENSOR_DEBUG_GPIO_TRIGGER_PIN
#endif

#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32
#include "driver/touch_pad.h"
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
#include "esp_private/rtc_ctrl.h"
#else
#include "driver/rtc_cntl.h"
#endif
#elif CONFIG_IDF_TARGET_ESP32P4
#include "driver/touch_sens.h"
#include "hal/touch_sensor_ll.h"
#endif

#include "touch_sensor_lowlevel.h"

static const char *TAG = "touch_lowlevel";

#define TOUCH_CHECK(a, str, ret_val) \
    if (!(a)) { \
        LOWLEVEL_LOGE(TAG, "%s(%d): %s", __FUNCTION__, __LINE__, str); \
        return (ret_val); \
    }

#define TOUCH_CHECK_GOTO(a, str, label) \
    if (!(a)) { \
        LOWLEVEL_LOGE(TAG, "%s(%d): %s", __FUNCTION__, __LINE__, str); \
        goto label; \
    }

typedef struct callback_node {
    touch_lowlevel_state_cb_t cb;
    void *arg;
    struct callback_node *next;
} callback_node_t;

typedef struct {
    callback_node_t *callbacks;
#if CONFIG_IDF_TARGET_ESP32P4
    touch_channel_handle_t chan_handle;
#endif
} channel_lowlevel_t;

typedef struct {
    touch_lowlevel_config_t config;
    channel_lowlevel_t channels[SOC_TOUCH_SENSOR_NUM];
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    touch_pad_intr_mask_t touch_intr_mask;
#endif
    bool touch_pad_started;
#if CONFIG_IDF_TARGET_ESP32P4
    touch_sensor_handle_t sens_handle;
#endif
} touch_lowlevel_t;

static touch_lowlevel_t s_touch_lowlevel = {0};

static bool _is_channel_configured(uint32_t channel)
{
    for (uint32_t i = 0; i < s_touch_lowlevel.config.channel_num; i++) {
        if (s_touch_lowlevel.config.channel_list[i] == channel) {
            return true;
        }
    }
    return false;
}

static uint32_t _get_channel_index(uint32_t channel)
{
    for (uint32_t i = 0; i < s_touch_lowlevel.config.channel_num; i++) {
        if (s_touch_lowlevel.config.channel_list[i] == channel) {
            return i;
        }
    }
    return SOC_TOUCH_SENSOR_NUM;
}

#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
static void default_internal_callback(void *arg)
{
#if CONFIG_TOUCH_SENSOR_DEBUG_GPIO_TRIGGER
    gpio_set_level(DEBUG_NEW_DATA_IO, true);
#endif
    uint32_t data = 0;
    uint32_t intr_mask = touch_pad_read_intr_status_mask();
    if (intr_mask & TOUCH_PAD_INTR_MASK_SCAN_DONE) {
        for (uint32_t index = 0; index < s_touch_lowlevel.config.channel_num; index++) {
            if (s_touch_lowlevel.config.channel_type[index] == TOUCH_LOWLEVEL_TYPE_TOUCH) {
                touch_pad_read_raw_data(s_touch_lowlevel.config.channel_list[index], &data);
            } else {
                continue;
            }
            channel_lowlevel_t *ch = &s_touch_lowlevel.channels[index];
            callback_node_t *node = ch->callbacks;
            while (node) {
                node->cb(s_touch_lowlevel.config.channel_list[index], TOUCH_LOWLEVEL_STATE_NEW_DATA, (void *)&data, node->arg);
                node = node->next;
            }
        }
    }

#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
    if (intr_mask & TOUCH_PAD_INTR_MASK_PROXI_MEAS_DONE) {
        for (uint32_t index = 0; index < s_touch_lowlevel.config.channel_num; index++) {
            if (s_touch_lowlevel.config.channel_type[index] == TOUCH_LOWLEVEL_TYPE_PROXIMITY) {
                touch_pad_proximity_get_data(s_touch_lowlevel.config.channel_list[index], &data);
            } else {
                continue;
            }
            channel_lowlevel_t *ch = &s_touch_lowlevel.channels[index];
            callback_node_t *node = ch->callbacks;
            while (node) {
                node->cb(s_touch_lowlevel.config.channel_list[index], TOUCH_LOWLEVEL_STATE_NEW_DATA, (void *)&data, node->arg);
                node = node->next;
            }
        }
    }
#endif
#if CONFIG_TOUCH_SENSOR_DEBUG_GPIO_TRIGGER
    gpio_set_level(DEBUG_NEW_DATA_IO, false);
#endif
}
#elif CONFIG_IDF_TARGET_ESP32P4
static bool _on_scan_done(touch_sensor_handle_t sens_handle, const touch_scan_done_event_data_t *event, void *user_ctx)
{
#if CONFIG_TOUCH_SENSOR_DEBUG_GPIO_TRIGGER
    gpio_set_level(DEBUG_NEW_DATA_IO, true);
#endif

    // read the data from all channels and update the FSMs
    for (uint32_t index = 0; index < s_touch_lowlevel.config.channel_num; index++) {
        uint32_t data[SOC_TOUCH_SAMPLE_CFG_NUM] = {};
        if (s_touch_lowlevel.config.channel_type[index] == TOUCH_LOWLEVEL_TYPE_TOUCH) {
            touch_channel_read_data(s_touch_lowlevel.channels[index].chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, data);
        } else {
            continue;
        }
        channel_lowlevel_t *ch = &s_touch_lowlevel.channels[index];
        callback_node_t *node = ch->callbacks;
        while (node) {
            node->cb(s_touch_lowlevel.config.channel_list[index], TOUCH_LOWLEVEL_STATE_NEW_DATA, (void *)data, node->arg);
            node = node->next;
        }
    }

#if CONFIG_TOUCH_SENSOR_DEBUG_GPIO_TRIGGER
    gpio_set_level(DEBUG_NEW_DATA_IO, false);
#endif
    return false;
}
#endif

esp_err_t touch_sensor_lowlevel_create(touch_lowlevel_config_t *config)
{
    TOUCH_CHECK(config != NULL, "Config pointer is NULL", ESP_ERR_INVALID_ARG);
    TOUCH_CHECK(!s_touch_lowlevel.touch_pad_started, "Cannot change config while touch pad is running", ESP_ERR_INVALID_STATE);
    // todo: Check if the channel list is valid, check is proximity is supported and if the count is valid
    // Free previously allocated memory if any
    if (s_touch_lowlevel.config.channel_list != NULL) {
        free(s_touch_lowlevel.config.channel_list);
        s_touch_lowlevel.config.channel_list = NULL;
    }
    // Deep copy the config
    s_touch_lowlevel.config.channel_num = config->channel_num;
    size_t total_size = config->channel_num * (sizeof(uint32_t) + sizeof(touch_lowlevel_type_t));
    uint8_t *buffer = (uint8_t *)malloc(total_size);
    TOUCH_CHECK(buffer != NULL, "Failed to allocate memory for channel list and type", ESP_ERR_NO_MEM);

    s_touch_lowlevel.config.channel_list = (uint32_t *)buffer;
    s_touch_lowlevel.config.channel_type = (touch_lowlevel_type_t *)(buffer + config->channel_num * sizeof(uint32_t));

    // Set default values for sample_period_ms based on channel count
    if (config->sample_period_ms == 0) {
        if (config->channel_num <= 8) {
            s_touch_lowlevel.config.sample_period_ms = 11;
        } else {
            s_touch_lowlevel.config.sample_period_ms = 17;
        }
    } else {
        s_touch_lowlevel.config.sample_period_ms = config->sample_period_ms;
    }

#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
    s_touch_lowlevel.config.proximity_count = config->proximity_count;
#endif

    memcpy(s_touch_lowlevel.config.channel_list, config->channel_list, config->channel_num * sizeof(uint32_t));
    if (config->channel_type) {
        memcpy(s_touch_lowlevel.config.channel_type, config->channel_type, config->channel_num * sizeof(touch_lowlevel_type_t));
    } else {
        for (uint32_t i = 0; i < config->channel_num; i++) {
            s_touch_lowlevel.config.channel_type[i] = TOUCH_LOWLEVEL_TYPE_TOUCH;
        }
    }

    LOWLEVEL_LOGI(TAG, "Touch sensor lowlevel (v%d.%d.%d) configured with %" PRIu32 " channels", TOUCH_SENSOR_LOWLEVEL_VER_MAJOR, TOUCH_SENSOR_LOWLEVEL_VER_MINOR, TOUCH_SENSOR_LOWLEVEL_VER_PATCH, config->channel_num);
    if (config->sample_period_ms > 0) {
        LOWLEVEL_LOGI(TAG, "User configured sample period: %" PRIu32 " ms", config->sample_period_ms);
    } else {
        LOWLEVEL_LOGI(TAG, "Using default sample period: %" PRIu32 " ms (channels <= 8: 11ms, channels > 8: 17ms)", s_touch_lowlevel.config.sample_period_ms);
    }
    for (uint32_t i = 0; i < config->channel_num; i++) {
        LOWLEVEL_LOGD(TAG, "Channel %" PRIu32 ": type %d", config->channel_list[i], config->channel_type[i]);
    }
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_delete(void)
{
    TOUCH_CHECK(!s_touch_lowlevel.touch_pad_started, "Cannot delete config while touch pad is running", ESP_ERR_INVALID_STATE);
    // Ensure all callbacks are unregistered
    for (uint32_t i = 0; i < SOC_TOUCH_SENSOR_NUM; i++) {
        channel_lowlevel_t *ch = &s_touch_lowlevel.channels[i];
        callback_node_t *node = ch->callbacks;
        while (node) {
            callback_node_t *temp = node;
            node = node->next;
            free(temp);
        }
        ch->callbacks = NULL;
    }
    // Free the memory allocated for channel list and type
    if (s_touch_lowlevel.config.channel_list != NULL) {
        free(s_touch_lowlevel.config.channel_list);
        s_touch_lowlevel.config.channel_list = NULL;
    }
    s_touch_lowlevel.config.channel_type = NULL;
    s_touch_lowlevel.config.channel_num = 0;
    LOWLEVEL_LOGI(TAG, "Touch sensor lowlevel config deleted");
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_start(void)
{
    // check if the config is valid
    TOUCH_CHECK(s_touch_lowlevel.config.channel_num > 0, "No channels configured", ESP_ERR_INVALID_ARG);
    TOUCH_CHECK(!s_touch_lowlevel.touch_pad_started, "Touch lowlevel already started", ESP_OK);

#if CONFIG_TOUCH_SENSOR_DEBUG_GPIO_TRIGGER
    // Initialize debug GPIO pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DEBUG_NEW_DATA_IO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(DEBUG_NEW_DATA_IO, 0); // Start with low level
    LOWLEVEL_LOGI(TAG, "Debug GPIO - New data: %d", DEBUG_NEW_DATA_IO);
#endif

    uint32_t src_freq_hz = 0;
    // Get the RTC slow clock frequency
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_RTC_SLOW, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &src_freq_hz);
#else
    src_freq_hz = rtc_clk_slow_freq_get_hz();
#endif
    // Calculate the measurement interval based on the number of channels
    // Use configured sample period (which includes defaults based on channel count)
    uint32_t total_period_us = s_touch_lowlevel.config.sample_period_ms * 1000;
    uint16_t measurement_interval = total_period_us / s_touch_lowlevel.config.channel_num;
    measurement_interval = measurement_interval < 1000 ? 0xff : measurement_interval - 1000;
    LOWLEVEL_LOGD(TAG, "RTC slow clock frequency: %" PRIu32 " Hz, total period: %" PRIu32 " us, set measurement interval: %" PRIu16 " us",
                  src_freq_hz, total_period_us, measurement_interval);

#if CONFIG_IDF_TARGET_ESP32
    touch_pad_init();
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_0V);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // For ESP-IDF v5.0 and later, use the new API
    touch_pad_set_measurement_interval(measurement_interval * src_freq_hz / 1000000);
    touch_pad_set_measurement_clock_cycles(0x3fff);
#else
    touch_pad_set_meas_time(measurement_interval * src_freq_hz / 1000000, 0x3fff);
#endif

    for (uint32_t i = 0; i < s_touch_lowlevel.config.channel_num; i++) {
        uint32_t channel = s_touch_lowlevel.config.channel_list[i];
        touch_pad_config(channel, 0);
    }
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    touch_pad_init();
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_0V);
    // Set the measurement time and charge/discharge times
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    touch_pad_set_measurement_interval(measurement_interval * src_freq_hz / 1000000);
    touch_pad_set_charge_discharge_times(234);
#else
    touch_pad_set_meas_time(measurement_interval * src_freq_hz / 1000000, 234);
#endif

    // todo: the mask macro may change in different idf versions
    s_touch_lowlevel.touch_intr_mask = TOUCH_PAD_INTR_MASK_SCAN_DONE;
    for (uint32_t i = 0; i < s_touch_lowlevel.config.channel_num; i++) {
        uint32_t channel = s_touch_lowlevel.config.channel_list[i];
        touch_pad_config(channel);
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
        if (s_touch_lowlevel.config.channel_type[i] == TOUCH_LOWLEVEL_TYPE_PROXIMITY) {
            touch_pad_proximity_enable(channel, true);
            touch_pad_proximity_set_count(TOUCH_PAD_MAX, s_touch_lowlevel.config.proximity_count);
            s_touch_lowlevel.touch_intr_mask |= TOUCH_PAD_INTR_MASK_PROXI_MEAS_DONE;
        }
#endif
    }
    touch_pad_isr_register(default_internal_callback, NULL, s_touch_lowlevel.touch_intr_mask);
    touch_pad_intr_enable(s_touch_lowlevel.touch_intr_mask);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_fsm_start();
#elif CONFIG_IDF_TARGET_ESP32P4
    /* Allocate new touch controller handle, using user configs if implements weak_get_touch_sensor_config()
    * otherwise default configs will be used.
    */
    touch_sensor_sample_config_t sample_cfg[SOC_TOUCH_SAMPLE_CFG_NUM] = TOUCH_SAMPLE_CFG_DEFAULT();
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_BASIC_CFG_DEFAULT(SOC_TOUCH_SAMPLE_CFG_NUM, sample_cfg);

    if (weak_get_touch_sensor_config) {
        LOWLEVEL_LOGI(TAG, "Using config from weak_get_touch_sensor_config");
        sens_cfg = weak_get_touch_sensor_config();
    }
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &s_touch_lowlevel.sens_handle));
    // workaround for the esp-idf interval config issue
    touch_ll_set_measure_interval_ticks(measurement_interval * src_freq_hz / 1000000);
    touch_sensor_filter_config_t filter_cfg = TOUCH_DEFAULT_FILTER_CONFIG();
    if (weak_get_filter_config) {
        LOWLEVEL_LOGI(TAG, "Using filter config from weak_get_filter_config");
        filter_cfg = weak_get_filter_config();
    }
    ESP_ERROR_CHECK(touch_sensor_config_filter(s_touch_lowlevel.sens_handle, &filter_cfg));

    // Print sample_cfg
    for (int i = 0; i < SOC_TOUCH_SAMPLE_CFG_NUM; i++) {
        LOWLEVEL_LOGI(TAG, "sample_cfg[%d]:", i);
        LOWLEVEL_LOGI(TAG, "  div_num=%"PRIu32, sens_cfg.sample_cfg[i].div_num);
        LOWLEVEL_LOGI(TAG, "  charge_times=%"PRIu32, sens_cfg.sample_cfg[i].charge_times);
        LOWLEVEL_LOGI(TAG, "  rc_filter_res=%u", sens_cfg.sample_cfg[i].rc_filter_res);
        LOWLEVEL_LOGI(TAG, "  rc_filter_cap=%u", sens_cfg.sample_cfg[i].rc_filter_cap);
        LOWLEVEL_LOGI(TAG, "  low_drv=%u", sens_cfg.sample_cfg[i].low_drv);
        LOWLEVEL_LOGI(TAG, "  high_drv=%u", sens_cfg.sample_cfg[i].high_drv);
        LOWLEVEL_LOGI(TAG, "  bias_volt=%u", sens_cfg.sample_cfg[i].bias_volt);
        LOWLEVEL_LOGI(TAG, "  bypass_shield_output=%d", sens_cfg.sample_cfg[i].bypass_shield_output);
    }

    // Print sens_cfg
    LOWLEVEL_LOGI(TAG, "sens_cfg:");
    LOWLEVEL_LOGI(TAG, "  power_on_wait_us=%" PRIu32, sens_cfg.power_on_wait_us);
    LOWLEVEL_LOGI(TAG, "  meas_interval_us=%f", sens_cfg.meas_interval_us);
    LOWLEVEL_LOGI(TAG, "  max_meas_time_us=%" PRIu32, sens_cfg.max_meas_time_us);
    LOWLEVEL_LOGI(TAG, "  output_mode=%d", sens_cfg.output_mode);
    LOWLEVEL_LOGI(TAG, "  sample_cfg_num=%" PRIu32, sens_cfg.sample_cfg_num);

    // Print filter_cfg
    LOWLEVEL_LOGI(TAG, "filter_cfg:");
    LOWLEVEL_LOGI(TAG, "  benchmark.filter_mode=%d", filter_cfg.benchmark.filter_mode);
    LOWLEVEL_LOGI(TAG, "  benchmark.jitter_step=%" PRIu32, filter_cfg.benchmark.jitter_step);
    LOWLEVEL_LOGI(TAG, "  benchmark.denoise_lvl=%d", filter_cfg.benchmark.denoise_lvl);
    LOWLEVEL_LOGI(TAG, "  data.smooth_filter=%d", filter_cfg.data.smooth_filter);
    LOWLEVEL_LOGI(TAG, "  data.active_hysteresis=%" PRIu32, filter_cfg.data.active_hysteresis);
    LOWLEVEL_LOGI(TAG, "  data.debounce_cnt=%" PRIu32, filter_cfg.data.debounce_cnt);

    for (uint32_t i = 0; i < s_touch_lowlevel.config.channel_num; i++) {
        touch_channel_config_t chan_cfg = {
            .active_thresh = {0xFFFF, 0xFFFF, 0xFFFF},
        };
        ESP_ERROR_CHECK(touch_sensor_new_channel(s_touch_lowlevel.sens_handle, s_touch_lowlevel.config.channel_list[i], &chan_cfg, &s_touch_lowlevel.channels[i].chan_handle));
    }

    touch_event_callbacks_t callbacks = {
        .on_scan_done = _on_scan_done,
        .on_proximity_meas_done = NULL, //TODO
    };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(s_touch_lowlevel.sens_handle, &callbacks, NULL));
    /* Enable the touch sensor */
    ESP_ERROR_CHECK(touch_sensor_enable(s_touch_lowlevel.sens_handle));
    /* Start continuous scanning, you can also trigger oneshot scanning manually */
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(s_touch_lowlevel.sens_handle));
#endif
    s_touch_lowlevel.touch_pad_started = true;
    LOWLEVEL_LOGI(TAG, "Touch sensor lowlevel started");
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_stop(void)
{
    TOUCH_CHECK(s_touch_lowlevel.touch_pad_started, "Touch lowlevel not started", ESP_OK);
#if CONFIG_IDF_TARGET_ESP32
    touch_pad_deinit();
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    while (!touch_pad_meas_is_done());
    touch_pad_intr_disable(TOUCH_PAD_INTR_MASK_ALL);
    touch_pad_intr_clear(TOUCH_PAD_INTR_MASK_ALL);
    touch_pad_fsm_stop();
    // known memory leak in touch_sensor.c, TODO: fix esp-idf
    // there is no touch_pad_isr_deregister api to free the memory allocated by rtc_isr_register
    rtc_isr_deregister(default_internal_callback, NULL);
    touch_pad_deinit();
#elif CONFIG_IDF_TARGET_ESP32P4
    touch_sensor_stop_continuous_scanning(s_touch_lowlevel.sens_handle);
    touch_sensor_disable(s_touch_lowlevel.sens_handle);
    for (uint32_t i = 0; i < s_touch_lowlevel.config.channel_num; i++) {
        touch_sensor_del_channel(s_touch_lowlevel.channels[i].chan_handle);
    }
    touch_sensor_del_controller(s_touch_lowlevel.sens_handle);
#endif
    s_touch_lowlevel.touch_pad_started = false;
    LOWLEVEL_LOGI(TAG, "Touch sensor lowlevel stopped");
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_get_data(uint32_t channel, uint32_t *data)
{
    TOUCH_CHECK(_is_channel_configured(channel), "Channel not configured", ESP_ERR_INVALID_ARG);
    TOUCH_CHECK(data != NULL, "Data pointer is NULL", ESP_ERR_INVALID_ARG);
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    uint32_t index = _get_channel_index(channel);
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
    if (s_touch_lowlevel.config.channel_type[index] == TOUCH_LOWLEVEL_TYPE_PROXIMITY) {
        touch_pad_proximity_get_data(channel, data);
    } else
#endif
        if (s_touch_lowlevel.config.channel_type[index] == TOUCH_LOWLEVEL_TYPE_TOUCH) {
            touch_pad_read_raw_data(channel, data);
        }
#elif CONFIG_IDF_TARGET_ESP32
    touch_pad_read(channel, (uint16_t *)data);
#elif CONFIG_IDF_TARGET_ESP32P4
    uint32_t index = _get_channel_index(channel);
    touch_channel_read_data(s_touch_lowlevel.channels[index].chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, data);
#endif
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_register(uint32_t channel, touch_lowlevel_state_cb_t cb, void *arg, touch_lowlevel_handle_t *handle)
{
    TOUCH_CHECK(_is_channel_configured(channel), "Channel not configured", ESP_ERR_INVALID_ARG);
#if CONFIG_IDF_TARGET_ESP32
    if (handle) {
        *handle = NULL;
    }
    TOUCH_CHECK(0, "Touch sensor lowlevel register not supported on ESP32", ESP_ERR_NOT_SUPPORTED);
#endif
    TOUCH_CHECK(cb != NULL, "Callback pointer is NULL", ESP_ERR_INVALID_ARG);
    // Temporary disable the interrupt
    if (s_touch_lowlevel.touch_pad_started) {
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
        touch_pad_intr_disable(s_touch_lowlevel.touch_intr_mask);
#elif CONFIG_IDF_TARGET_ESP32P4
        touch_sensor_stop_continuous_scanning(s_touch_lowlevel.sens_handle);
#endif
    }
    uint32_t index = _get_channel_index(channel);
    channel_lowlevel_t *ch = &s_touch_lowlevel.channels[index];
    callback_node_t *node = (callback_node_t *)malloc(sizeof(callback_node_t));
    TOUCH_CHECK(node != NULL, "Failed to allocate memory for callback node", ESP_ERR_NO_MEM);
    node->cb = cb;
    node->arg = arg;
    node->next = ch->callbacks;
    ch->callbacks = node;
    if (handle) {
        *handle = (touch_lowlevel_handle_t)node;
    }
    // Re-enable the interrupt
    if (s_touch_lowlevel.touch_pad_started) {
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
        touch_pad_intr_enable(s_touch_lowlevel.touch_intr_mask);
#elif CONFIG_IDF_TARGET_ESP32P4
        touch_sensor_start_continuous_scanning(s_touch_lowlevel.sens_handle);
#endif
    }
    LOWLEVEL_LOGD(TAG, "Callback registered for channel %" PRIu32, channel);
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_unregister(touch_lowlevel_handle_t handle)
{
    TOUCH_CHECK(handle != NULL, "Handle is NULL", ESP_ERR_INVALID_ARG);
    if (s_touch_lowlevel.touch_pad_started) {
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
        touch_pad_intr_disable(s_touch_lowlevel.touch_intr_mask);
#elif CONFIG_IDF_TARGET_ESP32P4
        touch_sensor_stop_continuous_scanning(s_touch_lowlevel.sens_handle);
#endif
    }
    callback_node_t *node = (callback_node_t *)handle;
    for (uint32_t i = 0; i < SOC_TOUCH_SENSOR_NUM; i++) {
        channel_lowlevel_t *ch = &s_touch_lowlevel.channels[i];
        callback_node_t **current = &ch->callbacks;
        while (*current) {
            if (*current == node) {
                *current = node->next;
                free(node);
                LOWLEVEL_LOGD(TAG, "Callback unregistered for channel %" PRIu32, i);
                return ESP_OK;
            }
            current = &(*current)->next;
        }
    }
    if (s_touch_lowlevel.touch_pad_started) {
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
        touch_pad_intr_enable(s_touch_lowlevel.touch_intr_mask);
#elif CONFIG_IDF_TARGET_ESP32P4
        touch_sensor_start_continuous_scanning(s_touch_lowlevel.sens_handle);
#endif
    }
    return ESP_ERR_NOT_FOUND;
}
