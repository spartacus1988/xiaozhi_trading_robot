/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#ifdef CONFIG_TOUCH_SENSOR_LOWLEVEL_SIMULATOR
#if CONFIG_IDF_TARGET_LINUX
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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
} channel_lowlevel_t;

typedef struct {
    touch_lowlevel_config_t config;
    channel_lowlevel_t channels[SOC_TOUCH_SENSOR_NUM];
    uint32_t data_index[SOC_TOUCH_SENSOR_NUM];
    uint32_t data_index_get[SOC_TOUCH_SENSOR_NUM];
    uint32_t (**date_buffer)[4];
    uint32_t data_buffer_size[SOC_TOUCH_SENSOR_NUM];
    TaskHandle_t task;
    bool touch_pad_started;
    EventGroupHandle_t event_group;
} touch_lowlevel_t;

static touch_lowlevel_t s_touch_lowlevel = {0};
extern const uint32_t channel_1[][4];
extern const uint32_t channel_2[][4];
extern const uint32_t channel_3[][4];
extern const uint32_t channel_1_size;
extern const uint32_t channel_2_size;
extern const uint32_t channel_3_size;

#define TOUCH_EVENT_BIT_STARTED (1 << 1)
#define TOUCH_EVENT_BIT_STOPPED (1 << 2)
#define TOUCH_EVENT_BIT_DELETED (1 << 3)

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

static void touch_simulate_task(void *arg)
{
    // loop until get the stop event
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
                               s_touch_lowlevel.event_group,
                               TOUCH_EVENT_BIT_STARTED | TOUCH_EVENT_BIT_STOPPED | TOUCH_EVENT_BIT_DELETED,
                               pdFALSE,
                               pdFALSE,
                               portMAX_DELAY
                           );
        if (bits & TOUCH_EVENT_BIT_DELETED) {
            break;
        }
        if (bits & TOUCH_EVENT_BIT_STOPPED) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint32_t next_timeout = UINT32_MAX;
        int add_delay = 1;
        for (uint32_t i = 0; i < s_touch_lowlevel.config.channel_num; i++) {
            uint32_t channel = s_touch_lowlevel.config.channel_list[i];
            uint32_t index = _get_channel_index(channel);
            s_touch_lowlevel.data_index[index]++;
#if SOC_TOUCH_SAMPLE_CFG_NUM == 1
            uint32_t data[1];
            data[0] = s_touch_lowlevel.date_buffer[index][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
#elif SOC_TOUCH_SAMPLE_CFG_NUM == 3
            uint32_t data[3];
            data[0] = s_touch_lowlevel.date_buffer[index][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
            data[1] = s_touch_lowlevel.date_buffer[index + 1][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
            data[2] = s_touch_lowlevel.date_buffer[index + 2][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
#endif
            uint32_t timeout = s_touch_lowlevel.date_buffer[index][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][0]
                               - s_touch_lowlevel.date_buffer[index][(s_touch_lowlevel.data_index[index] - 1) % s_touch_lowlevel.data_buffer_size[index]][0];
            if (timeout < next_timeout) {
                next_timeout = timeout;
            }
            channel_lowlevel_t *ch = &s_touch_lowlevel.channels[index];
            callback_node_t *node = ch->callbacks;
            while (node) {
                node->cb(channel, TOUCH_LOWLEVEL_STATE_NEW_DATA, (void *)data, node->arg);
                node = node->next;
                add_delay = 0;
            }
        }
        if (add_delay) {
            vTaskDelay(add_delay);
        }
#if CONFIG_TOUCH_SENSOR_LOWLEVEL_SIMULATOR_REALTIME
        vTaskDelay(pdMS_TO_TICKS(next_timeout));
#endif
    }
    xEventGroupClearBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_DELETED);
    vTaskDelete(NULL);
}

esp_err_t touch_sensor_lowlevel_create(touch_lowlevel_config_t *config)
{
    TOUCH_CHECK(config != NULL, "Config pointer is NULL", ESP_ERR_INVALID_ARG);
    TOUCH_CHECK(!s_touch_lowlevel.touch_pad_started, "Cannot change config while touch pad is running", ESP_ERR_INVALID_STATE);
    // todo: Check if the channel list is valid, check is proximity is supported and if the count is valid
    // Free previously allocated memory if any
    if (s_touch_lowlevel.config.channel_list != NULL) {
        free(s_touch_lowlevel.config.channel_list);
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
    // memory
#if SOC_TOUCH_SAMPLE_CFG_NUM == 1
    s_touch_lowlevel.date_buffer = (uint32_t(**)[4])malloc(config->channel_num * sizeof(uint32_t(*)[4]));
#elif SOC_TOUCH_SAMPLE_CFG_NUM == 3
    // three buffer each channel for three frequencies
    s_touch_lowlevel.date_buffer = (uint32_t(**)[4])malloc(config->channel_num * 3 * sizeof(uint32_t(*)[4]));
#endif
    TOUCH_CHECK_GOTO(s_touch_lowlevel.date_buffer != NULL, "Failed to allocate memory for data buffer", _free_buffer);

#if SOC_TOUCH_SAMPLE_CFG_NUM == 1
    for (uint32_t i = 0; i < config->channel_num; i++) {
        const uint32_t (*buf)[4] = weak_get_data_buffer ? weak_get_data_buffer(config->channel_list[i]) : NULL;
        if (buf) {
            s_touch_lowlevel.date_buffer[i] = buf;
            s_touch_lowlevel.data_buffer_size[i] = weak_get_data_buffer_size ? weak_get_data_buffer_size(config->channel_list[i]) : 0;
        } else {
            switch (i % 3) {
            case 0:
                s_touch_lowlevel.date_buffer[i] = channel_1;
                s_touch_lowlevel.data_buffer_size[i] = channel_1_size;
                break;
            case 1:
                s_touch_lowlevel.date_buffer[i] = channel_2;
                s_touch_lowlevel.data_buffer_size[i] = channel_2_size;
                break;
            case 2:
                s_touch_lowlevel.date_buffer[i] = channel_3;
                s_touch_lowlevel.data_buffer_size[i] = channel_3_size;
                break;
            default:
                break;
            }
        }
        LOWLEVEL_LOGD(TAG, "Channel %" PRIu32 ": type %d", config->channel_list[i], s_touch_lowlevel.config.channel_type[i]);
    }
#elif SOC_TOUCH_SAMPLE_CFG_NUM == 3
    for (uint32_t i = 0; i < config->channel_num; i++) {
        if (weak_get_data_buffer && weak_get_data_buffer_size) {
            s_touch_lowlevel.date_buffer[3 * i + 0] = weak_get_data_buffer(config->channel_list[i]);
            s_touch_lowlevel.date_buffer[3 * i + 1] = weak_get_data_buffer(config->channel_list[i]);
            s_touch_lowlevel.date_buffer[3 * i + 2] = weak_get_data_buffer(config->channel_list[i]);
            s_touch_lowlevel.data_buffer_size[3 * i + 0] = weak_get_data_buffer_size(config->channel_list[i]);
            s_touch_lowlevel.data_buffer_size[3 * i + 1] = weak_get_data_buffer_size(config->channel_list[i]);
            s_touch_lowlevel.data_buffer_size[3 * i + 2] = weak_get_data_buffer_size(config->channel_list[i]);
        } else {
            s_touch_lowlevel.date_buffer[i] = channel_1;
            s_touch_lowlevel.date_buffer[i + 1] = channel_2;
            s_touch_lowlevel.date_buffer[i + 2] = channel_3;
            s_touch_lowlevel.data_buffer_size[i] = channel_1_size;
            s_touch_lowlevel.data_buffer_size[i + 1] = channel_2_size;
            s_touch_lowlevel.data_buffer_size[i + 2] = channel_3_size;
        }
        LOWLEVEL_LOGD(TAG, "Channel %" PRIu32 ": type %d", config->channel_list[i], s_touch_lowlevel.config.channel_type[i]);
    }
#endif
    s_touch_lowlevel.event_group = xEventGroupCreate();
    TOUCH_CHECK_GOTO(s_touch_lowlevel.event_group != NULL, "Failed to create event group", _free_buffer);
    UBaseType_t priority = uxTaskPriorityGet(NULL);
    xTaskCreate(touch_simulate_task, "touch_simulate", 2048, NULL, priority, &s_touch_lowlevel.task);
    TOUCH_CHECK_GOTO(s_touch_lowlevel.task != NULL, "Failed to create task", _free_buffer);
    return ESP_OK;

_free_buffer:
    if (s_touch_lowlevel.date_buffer != NULL) {
        free(s_touch_lowlevel.date_buffer);
    }
    if (s_touch_lowlevel.event_group != NULL) {
        vEventGroupDelete(s_touch_lowlevel.event_group);
    }
    free(buffer);
    s_touch_lowlevel.config.channel_list = NULL;
    s_touch_lowlevel.config.channel_type = NULL;
    s_touch_lowlevel.config.channel_num = 0;
    return ESP_ERR_NO_MEM;
}

esp_err_t touch_sensor_lowlevel_delete(void)
{
    TOUCH_CHECK(!s_touch_lowlevel.touch_pad_started, "Cannot delete config while touch pad is running", ESP_ERR_INVALID_STATE);
    // delete the task
    xEventGroupSetBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_DELETED);
    // wait until the TOUCH_EVENT_BIT_DELETED be clear
    while (xEventGroupGetBits(s_touch_lowlevel.event_group) & TOUCH_EVENT_BIT_DELETED) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
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
    free(s_touch_lowlevel.date_buffer);
    vEventGroupDelete(s_touch_lowlevel.event_group);
    memset(&s_touch_lowlevel, 0, sizeof(touch_lowlevel_t));
    LOWLEVEL_LOGI(TAG, "Touch sensor lowlevel config deleted");
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_start(void)
{
    // check if the config is valid
    TOUCH_CHECK(s_touch_lowlevel.config.channel_num > 0, "No channels configured", ESP_ERR_INVALID_ARG);
    TOUCH_CHECK(!s_touch_lowlevel.touch_pad_started, "Touch lowlevel already started", ESP_OK);
    // start the task
    xEventGroupClearBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STOPPED);
    xEventGroupSetBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STARTED);
    s_touch_lowlevel.touch_pad_started = true;
    LOWLEVEL_LOGI(TAG, "Touch sensor lowlevel started");

    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_stop(void)
{
    TOUCH_CHECK(s_touch_lowlevel.touch_pad_started, "Touch lowlevel not started", ESP_OK);
    // suspend the task
    xEventGroupSetBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STOPPED);
    s_touch_lowlevel.touch_pad_started = false;
    LOWLEVEL_LOGI(TAG, "Touch sensor lowlevel stopped");
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_get_data(uint32_t channel, uint32_t *data)
{
    TOUCH_CHECK(_is_channel_configured(channel), "Channel not configured", ESP_ERR_INVALID_ARG);
    TOUCH_CHECK(data != NULL, "Data pointer is NULL", ESP_ERR_INVALID_ARG);
#if CONFIG_TOUCH_SENSOR_LOWLEVEL_SIMULATOR_REALTIME
#if SOC_TOUCH_SAMPLE_CFG_NUM == 1
    uint32_t index = _get_channel_index(channel);
    // return data from simulation data
    *data = s_touch_lowlevel.date_buffer[index][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
#elif SOC_TOUCH_SAMPLE_CFG_NUM == 3
    uint32_t index = _get_channel_index(channel);
    data[0] = s_touch_lowlevel.date_buffer[3 * index + 0][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
    data[1] = s_touch_lowlevel.date_buffer[3 * index + 1][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
    data[2] = s_touch_lowlevel.date_buffer[3 * index + 2][s_touch_lowlevel.data_index[index] % s_touch_lowlevel.data_buffer_size[index]][1];
#endif
#else
    // return data from the index 0 to the last index one by one using data_index_get
#if SOC_TOUCH_SAMPLE_CFG_NUM == 1
    uint32_t index = _get_channel_index(channel);
    *data = s_touch_lowlevel.date_buffer[index][s_touch_lowlevel.data_index_get[index] % s_touch_lowlevel.data_buffer_size[index]][1];
#elif SOC_TOUCH_SAMPLE_CFG_NUM == 3
    uint32_t index = _get_channel_index(channel);
    data[0] = s_touch_lowlevel.date_buffer[3 * index + 0][s_touch_lowlevel.data_index_get[index] % s_touch_lowlevel.data_buffer_size[index]][1];
    data[1] = s_touch_lowlevel.date_buffer[3 * index + 1][s_touch_lowlevel.data_index_get[index] % s_touch_lowlevel.data_buffer_size[index]][1];
    data[2] = s_touch_lowlevel.date_buffer[3 * index + 2][s_touch_lowlevel.data_index_get[index] % s_touch_lowlevel.data_buffer_size[index]][1];
#endif
    s_touch_lowlevel.data_index_get[index]++;
#endif
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_register(uint32_t channel, touch_lowlevel_state_cb_t cb, void *arg, touch_lowlevel_handle_t *handle)
{
    TOUCH_CHECK(_is_channel_configured(channel), "Channel not configured", ESP_ERR_INVALID_ARG);
    TOUCH_CHECK(cb != NULL, "Callback pointer is NULL", ESP_ERR_INVALID_ARG);
    // Temporary stop the task
    xEventGroupSetBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STOPPED);
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
    // Re-enable the task
    xEventGroupClearBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STOPPED);
    LOWLEVEL_LOGD(TAG, "Callback registered for channel %" PRIu32, channel);
    return ESP_OK;
}

esp_err_t touch_sensor_lowlevel_unregister(touch_lowlevel_handle_t handle)
{
    TOUCH_CHECK(handle != NULL, "Handle is NULL", ESP_ERR_INVALID_ARG);
    // Temporary disable the timer
    xEventGroupSetBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STOPPED);
    callback_node_t *node = (callback_node_t *)handle;
    for (uint32_t i = 0; i < SOC_TOUCH_SENSOR_NUM; i++) {
        channel_lowlevel_t *ch = &s_touch_lowlevel.channels[i];
        callback_node_t **current = &ch->callbacks;
        while (*current) {
            if (*current == node) {
                *current = node->next;
                free(node);
                xEventGroupClearBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STOPPED);
                LOWLEVEL_LOGD(TAG, "Callback unregistered for channel %" PRIu32, i);
                return ESP_OK;
            }
            current = &(*current)->next;
        }
    }

    // Re-enable the timer
    xEventGroupClearBits(s_touch_lowlevel.event_group, TOUCH_EVENT_BIT_STOPPED);
    return ESP_ERR_NOT_FOUND;
}
#else
#warning "Touch sensor lowlevel simulator is only supported on Linux"
#endif
#endif
