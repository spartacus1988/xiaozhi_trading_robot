/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>
#include <time.h>
#include "unity.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "touch_sensor_lowlevel.h"

static size_t before_free_8bit;
static size_t before_free_32bit;

#define TEST_MEMORY_LEAK_THRESHOLD (-200)
static uint64_t start_time = 0;

static uint64_t get_time_in_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t current_time = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (start_time == 0) {
        start_time = current_time;
    }
    return current_time - start_time;
}

#if SOC_TOUCH_SAMPLE_CFG_NUM == 1
#if CONFIG_IDF_TARGET_ESP32
static uint32_t touch_channel_list[] = {8, 6, 4};
#else
static uint32_t touch_channel_list[] = {8, 12, 10};
#endif
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
static touch_lowlevel_type_t touch_type_list[] = {TOUCH_LOWLEVEL_TYPE_TOUCH, TOUCH_LOWLEVEL_TYPE_TOUCH, TOUCH_LOWLEVEL_TYPE_PROXIMITY};
#else
static touch_lowlevel_type_t touch_type_list[] = {TOUCH_LOWLEVEL_TYPE_TOUCH, TOUCH_LOWLEVEL_TYPE_TOUCH, TOUCH_LOWLEVEL_TYPE_TOUCH};
#endif
#elif SOC_TOUCH_SAMPLE_CFG_NUM == 3
static uint32_t touch_channel_list[] = {9};
static touch_lowlevel_type_t touch_type_list[] = {TOUCH_LOWLEVEL_TYPE_TOUCH};
#endif

#if CONFIG_IDF_TARGET_LINUX
#define my_printf printf
#else
#define my_printf esp_rom_printf
#endif

#define TOUCH_CHANNEL_NUM (sizeof(touch_channel_list) / sizeof(touch_channel_list[0]))

// print touch values(raw, smooth, benchmark) of each enabled channel, every 50ms
#define PRINT_VALUE(pad_num, raw, smooth, benchmark) \
    my_printf("vl,%"PRId64",%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"\n", \
              (int64_t)(get_time_in_ms()), \
              (pad_num), (raw), (smooth), (benchmark))

#if CONFIG_IDF_TARGET_ESP32P4
// Strong implementation of weak_get_touch_sensor_config
touch_sensor_config_t weak_get_touch_sensor_config(void)
{
    static touch_sensor_sample_config_t sample_cfg[SOC_TOUCH_SAMPLE_CFG_NUM] = TOUCH_SAMPLE_CFG_DEFAULT();
    static touch_sensor_config_t basic_cfg = TOUCH_SENSOR_BASIC_CFG_DEFAULT(SOC_TOUCH_SAMPLE_CFG_NUM, sample_cfg);
    return basic_cfg;
}

// Strong implementation of weak_get_filter_config
touch_sensor_filter_config_t weak_get_filter_config(void)
{
    static touch_sensor_filter_config_t filter_cfg = TOUCH_DEFAULT_FILTER_CONFIG();
    return filter_cfg;
}
#endif

static void touch_sensor_callback(uint32_t channel, touch_lowlevel_state_t state, void *state_data, void *arg)
{
    switch (state) {
    case TOUCH_LOWLEVEL_STATE_START:
        my_printf("Touch sensor %"PRIu32" start\n", (uint32_t)(uintptr_t)arg);
        break;
    case TOUCH_LOWLEVEL_STATE_STOP:
        my_printf("Touch sensor %"PRIu32" stop\n", (uint32_t)(uintptr_t)arg);
        break;
    case TOUCH_LOWLEVEL_STATE_NEW_DATA:
        for (int i = 0; i < SOC_TOUCH_SAMPLE_CFG_NUM; i++) {
            PRINT_VALUE((uint32_t)(uintptr_t)arg + i, *((uint32_t *)state_data + i), *((uint32_t *)state_data + i), *((uint32_t *)state_data + i));
        }
        break;
    default:
        break;
    }
}

void setUp(void)
{
    before_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    before_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
}

void tearDown(void)
{
    size_t after_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t after_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    ssize_t delta_8bit = after_free_8bit - before_free_8bit;
    ssize_t delta_32bit = after_free_32bit - before_free_32bit;
    my_printf("MALLOC_CAP_8BIT: Before %zu bytes free, After %zu bytes free (delta %zd)\n", before_free_8bit, after_free_8bit, delta_8bit);
    my_printf("MALLOC_CAP_32BIT: Before %zu bytes free, After %zu bytes free (delta %zd)\n", before_free_32bit, after_free_32bit, delta_32bit);
    TEST_ASSERT_MESSAGE(delta_8bit >= TEST_MEMORY_LEAK_THRESHOLD, "memory leak");
    TEST_ASSERT_MESSAGE(delta_32bit >= TEST_MEMORY_LEAK_THRESHOLD, "memory leak");
}

TEST_CASE("Touch sensor lowlevel config and start", "[touch_sensor_lowlevel]")
{
    touch_lowlevel_config_t config = {
        .channel_num = TOUCH_CHANNEL_NUM,
        .channel_list = touch_channel_list,
        .channel_type = touch_type_list,
        .sample_period_ms = 0,
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
        .proximity_count = 10,
#endif
    };

    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_create(&config));
    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_start());
    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_stop());
    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_delete());
}

TEST_CASE("Touch sensor lowlevel register and unregister callback", "[touch_sensor_lowlevel]")
{
    touch_lowlevel_config_t config = {
        .channel_num = TOUCH_CHANNEL_NUM,
        .channel_list = touch_channel_list,
        .channel_type = touch_type_list,
        .sample_period_ms = 0,
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
        .proximity_count = 10,
#endif
    };

    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_create(&config));
    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_start());

    touch_lowlevel_handle_t handle[TOUCH_CHANNEL_NUM] = {0};
    for (uint32_t i = 0; i < TOUCH_CHANNEL_NUM; i++) {
#if CONFIG_IDF_TARGET_ESP32
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, touch_sensor_lowlevel_register(config.channel_list[i], touch_sensor_callback, (void *)(uintptr_t)config.channel_list[i], &handle[i]));
#else
        TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_register(config.channel_list[i], touch_sensor_callback, (void *)(uintptr_t)config.channel_list[i], &handle[i]));
#endif
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    for (uint32_t i = 0; i < TOUCH_CHANNEL_NUM; i++) {
#if CONFIG_IDF_TARGET_ESP32
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, touch_sensor_lowlevel_unregister(handle[i]));
#else
        TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_unregister(handle[i]));
#endif
    }

    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_stop());
    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_delete());
}

TEST_CASE("Touch sensor lowlevel get data", "[touch_sensor_lowlevel]")
{
    touch_lowlevel_config_t config = {
        .channel_num = TOUCH_CHANNEL_NUM,
        .channel_list = touch_channel_list,
        .channel_type = touch_type_list,
        .sample_period_ms = 0,
#if SOC_TOUCH_PROXIMITY_MEAS_DONE_SUPPORTED
        .proximity_count = 50,
#endif
    };

    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_create(&config));
    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_start());
    vTaskDelay(100 / portTICK_PERIOD_MS);

    uint32_t loop_times = 100;
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_LINUX
    uint32_t data[3] = {0};
#else
    uint32_t data[1] = {0};
#endif

    for (uint32_t i = 0; i < loop_times; i++) {
        for (uint32_t j = 0; j < config.channel_num; j++) {
            TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_get_data(config.channel_list[j], data));
#if SOC_TOUCH_SAMPLE_CFG_NUM == 1
            PRINT_VALUE(config.channel_list[j], data[0], data[0], data[0]);
#elif SOC_TOUCH_SAMPLE_CFG_NUM == 3
            PRINT_VALUE(config.channel_list[j], data[0], data[0], data[0]);
            PRINT_VALUE(config.channel_list[j] + 1, data[1], data[1], data[1]);
            PRINT_VALUE(config.channel_list[j] + 2, data[2], data[2], data[2]);
#endif
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_stop());
    TEST_ASSERT_EQUAL(ESP_OK, touch_sensor_lowlevel_delete());
}

void app_main(void)
{
    unity_run_menu();
}
