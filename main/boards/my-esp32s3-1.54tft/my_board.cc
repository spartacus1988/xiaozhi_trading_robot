#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "market_notifier.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include <esp_lcd_panel_vendor.h>
#include <nvs_flash.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <esp_partition.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <map>

#define TAG "MyBoard"

class MyBoard : public WifiBoard {
public:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    // Parameters for the WiFi tool background tasks. The scan and the connect
    // run in their own FreeRTOS task and send the MCP reply later through
    // McpServer::SendReply, so they carry the reply id with them.
    struct WifiScanTaskParam {
        MyBoard* self;
        int reply_id;
    };

    struct WifiConnectTaskParam {
        int reply_id;
        std::string ssid;
        std::string password;
    };

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_38);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_21);
        rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_21, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_set_level(GPIO_NUM_21, 0);
            // 启用保持功能，确保睡眠期间电平不变
            rtc_gpio_hold_en(GPIO_NUM_21);
            esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        display_ = new SpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

public:
    MyBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        GetBacklight()->RestoreBrightness();

        InitializeTools();
    }

    // Background task for self.wifi.scan_networks: runs a blocking WiFi scan
    // off the main task and posts the JSON result back to the MCP client.
    static void WifiScanTask(void* param) {
        auto* p = static_cast<WifiScanTaskParam*>(param);
        auto* self = p->self;
        int captured_id = p->reply_id;
        delete p;

        auto display = self->GetDisplay();

        display->SetChatMessage("system", "Scanning WiFi...");
        ESP_LOGI(TAG, "Background scan task started on core %d, priority %d", xPortGetCoreID(), uxTaskPriorityGet(nullptr));

        // NOTE: the stock 78/esp-wifi-connect component (3.1.5) has no
        // SetExternalScanMode(); if the station's reconnect timer fires a
        // scan of its own, esp_wifi_scan_start() returns ESP_ERR_WIFI_STATE
        // and the retry loop below simply tries again.
        esp_wifi_set_ps(WIFI_PS_NONE);
        vTaskDelay(pdMS_TO_TICKS(500));

        uint16_t ap_count = 0;
        wifi_ap_record_t* ap_records = nullptr;

        for (int attempt = 1; attempt <= 3; attempt++) {
            wifi_scan_config_t scan_config = {};
            scan_config.show_hidden = false;
            scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_config.scan_time.active.min = 100;
            scan_config.scan_time.active.max = 300;

            esp_err_t err = esp_wifi_scan_start(&scan_config, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            esp_wifi_scan_get_ap_num(&ap_count);
            ESP_LOGI(TAG, "Attempt %d: found %d APs", attempt, ap_count);

            if (ap_count > 0) {
                ap_records = new wifi_ap_record_t[ap_count];
                esp_wifi_scan_get_ap_records(&ap_count, ap_records);
                break;
            }

            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(1500));
        }

        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

        cJSON* root = cJSON_CreateArray();
        std::string summary;
        int displayed = 0;

        if (ap_records) {
            for (int i = 0; i < ap_count; i++) {
                std::string ssid = reinterpret_cast<char*>(ap_records[i].ssid);
                if (ssid.empty()) continue;

                ESP_LOGI(TAG, "  [%d] SSID=%s RSSI=%d AUTH=%d",
                         i, ssid.c_str(), ap_records[i].rssi, ap_records[i].authmode);

                cJSON* net = cJSON_CreateObject();
                cJSON_AddStringToObject(net, "ssid", ssid.c_str());
                cJSON_AddNumberToObject(net, "rssi", ap_records[i].rssi);

                const char* auth = "open";
                if (ap_records[i].authmode == WIFI_AUTH_WEP) auth = "wep";
                else if (ap_records[i].authmode == WIFI_AUTH_WPA_PSK) auth = "wpa";
                else if (ap_records[i].authmode == WIFI_AUTH_WPA2_PSK) auth = "wpa2";
                else if (ap_records[i].authmode == WIFI_AUTH_WPA3_PSK) auth = "wpa3";
                else if (ap_records[i].authmode == WIFI_AUTH_WPA_WPA2_PSK) auth = "wpa/wpa2";
                cJSON_AddStringToObject(net, "auth", auth);
                cJSON_AddItemToArray(root, net);

                if (displayed < 8) {
                    if (!summary.empty()) summary += ", ";
                    summary += ssid;
                    summary += " (";
                    summary += std::to_string(ap_records[i].rssi);
                    summary += "dBm)";
                }
                displayed++;
            }
            delete[] ap_records;

            if (displayed > 8) {
                summary += " + " + std::to_string(displayed - 8) + " more";
            }
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "Found %d networks", ap_count);
        display->SetChatMessage("system", msg);
        ESP_LOGI(TAG, "Scan complete: %s", msg);

        if (summary.empty()) summary = "No WiFi networks found";

        char* json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        cJSON* wrapper = cJSON_CreateObject();
        cJSON* content = cJSON_CreateArray();
        cJSON* text = cJSON_CreateObject();
        cJSON_AddStringToObject(text, "type", "text");
        cJSON_AddStringToObject(text, "text", json_str);
        cJSON_free(json_str);
        cJSON_AddItemToArray(content, text);
        cJSON_AddItemToObject(wrapper, "content", content);
        cJSON_AddBoolToObject(wrapper, "isError", false);

        char* wrapper_str = cJSON_PrintUnformatted(wrapper);
        std::string result_str(wrapper_str);
        cJSON_free(wrapper_str);
        cJSON_Delete(wrapper);

        ESP_LOGI(TAG, "MCP reply payload: %s", result_str.c_str());
        McpServer::GetInstance().SendReply(captured_id, result_str);
        ESP_LOGI(TAG, "MCP reply sent for id=%d", captured_id);

        vTaskDelete(nullptr);
    }

    // Background task for self.wifi.connect_network: saves the credentials,
    // reconnects and reports success or a 15 second timeout.
    static void WifiConnectTask(void* arg) {
        auto* param = static_cast<WifiConnectTaskParam*>(arg);
        int captured_id = param->reply_id;
        std::string ssid = std::move(param->ssid);
        std::string password = std::move(param->password);
        delete param;

        ESP_LOGI(TAG, "WiFi connect task started for SSID: %s", ssid.c_str());

        auto& wifi_mgr = WifiManager::GetInstance();

        // Save credentials to NVS
        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.AddSsid(ssid, password);
        ESP_LOGI(TAG, "Saved SSID to NVS: %s", ssid.c_str());

        // Stop current connection and reconnect with new credentials
        wifi_mgr.StopStation();
        vTaskDelay(pdMS_TO_TICKS(500));
        wifi_mgr.StartStation();

        // Wait for connection with 15s timeout
        auto start_time = xTaskGetTickCount();
        const TickType_t timeout_ticks = pdMS_TO_TICKS(15000);
        bool connected = false;

        while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
            if (wifi_mgr.IsConnected()) {
                connected = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Build MCP reply
        cJSON* wrapper = cJSON_CreateObject();
        cJSON* content = cJSON_CreateArray();
        cJSON* text = cJSON_CreateObject();
        cJSON_AddStringToObject(text, "type", "text");

        if (connected) {
            std::string msg = "Successfully connected to " + ssid + ".";
            cJSON_AddStringToObject(text, "text", msg.c_str());
            ESP_LOGI(TAG, "Connected to %s", ssid.c_str());
        } else {
            std::string msg = "Failed to connect to " + ssid + " within 15 seconds. "
                              "Please check the password and try again, or use captive portal at http://192.168.4.1.";
            cJSON_AddStringToObject(text, "text", msg.c_str());
            ESP_LOGW(TAG, "Connection to %s timed out", ssid.c_str());
        }

        cJSON_AddItemToArray(content, text);
        cJSON_AddItemToObject(wrapper, "content", content);
        cJSON_AddBoolToObject(wrapper, "isError", !connected);

        char* wrapper_str = cJSON_PrintUnformatted(wrapper);
        std::string result_str(wrapper_str);
        cJSON_free(wrapper_str);
        cJSON_Delete(wrapper);

        ESP_LOGI(TAG, "MCP reply payload: %s", result_str.c_str());
        McpServer::GetInstance().SendReply(captured_id, result_str);
        ESP_LOGI(TAG, "MCP reply sent for id=%d", captured_id);

        vTaskDelete(nullptr);
    }

    // The board's custom MCP tools: the four WiFi network tools
    // (scan/connect/save/forget) are restored unchanged from the original
    // board, the packet sniffer, DNS log and memory info tools stay removed.
    // The market notifier is a C++ port of
    // https://github.com/spartacus1988/Gold_and_crypto_notifyer (main.py).
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.wifi.scan_networks",
            "Scan all available WiFi networks nearby and return the list.\n"
            "Returns: A list of WiFi networks with SSID, signal strength (RSSI), and encryption type.\n"
            "Use this tool when the user wants to see available WiFi networks or check WiFi signal.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                auto& wifi_mgr = WifiManager::GetInstance();
                int reply_id = McpServer::GetInstance().GetPendingToolCallId();

                ESP_LOGI(TAG, "WiFi scan requested, reply_id=%d", reply_id);

                if (!wifi_mgr.IsInitialized()) {
                    return std::string("{\"error\":\"WiFi not initialized\"}");
                }
                if (wifi_mgr.IsConfigMode()) {
                    return std::string("{\"error\":\"WiFi in config AP mode\"}");
                }

                auto* self = this;
                int captured_id = reply_id;
                McpServer::GetInstance().SetDeferredReply(true);
                auto* param = new WifiScanTaskParam{self, captured_id};
                xTaskCreatePinnedToCore(WifiScanTask, "wifi_scan", 8192, param, 24, nullptr, 0);

                return std::string("WiFi scan in progress. Results will be announced shortly.");
            });

        mcp_server.AddTool("self.wifi.connect_network",
            "Connect to a WiFi network.\n"
            "Args:\n"
            "  `ssid`: The name of the WiFi network to connect to.\n"
            "  `password`: The password for the WiFi network (optional if already saved).\n"
            "If the network was previously connected, the device already has the password saved and will connect automatically.\n"
            "Only ask the user for the password if the network has never been connected before.",
            PropertyList({
                Property("ssid", kPropertyTypeString),
                Property("password", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto& wifi_mgr = WifiManager::GetInstance();
                int reply_id = McpServer::GetInstance().GetPendingToolCallId();
                std::string ssid = properties["ssid"].value<std::string>();
                std::string password = properties["password"].value<std::string>();

                ESP_LOGI(TAG, "WiFi connect requested: ssid=%s, reply_id=%d", ssid.c_str(), reply_id);

                if (!wifi_mgr.IsInitialized()) {
                    return std::string("{\"error\":\"WiFi not initialized\"}");
                }
                if (ssid.empty()) {
                    return std::string("{\"error\":\"SSID is required\"}");
                }

                bool used_saved = false;
                // If no password provided, check if we have saved credentials for this SSID
                if (password.empty()) {
                    auto& ssid_manager = SsidManager::GetInstance();
                    auto& ssid_list = ssid_manager.GetSsidList();
                    for (int i = 0; i < (int)ssid_list.size(); i++) {
                        if (ssid_list[i].ssid == ssid) {
                            password = ssid_list[i].password;
                            used_saved = true;
                            ESP_LOGI(TAG, "Using saved credentials for SSID: %s", ssid.c_str());
                            break;
                        }
                    }
                    if (password.empty()) {
                        return std::string("{\"error\":\"No saved password for " + ssid + ". Please provide the password.\"}");
                    }
                }

                // Save and connect
                {
                    auto& ssid_manager = SsidManager::GetInstance();
                    ssid_manager.AddSsid(ssid, password);
                }
                McpServer::GetInstance().SetDeferredReply(true);
                auto* param = new WifiConnectTaskParam{reply_id, ssid, password};
                xTaskCreatePinnedToCore(WifiConnectTask, "wifi_connect", 8192, param, 24, nullptr, 0);
                if (used_saved) {
                    return std::string("Using saved password to connect to " + ssid + "...");
                }
                return std::string("Connecting to " + ssid + "...");
            });

        mcp_server.AddTool("self.wifi.get_saved_networks",
            "Get the list of WiFi networks saved on the device.\n"
            "Returns: A list of saved WiFi network names (SSIDs).\n"
            "Use this tool to check which networks the device remembers.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                auto& ssid_manager = SsidManager::GetInstance();
                auto& ssid_list = ssid_manager.GetSsidList();

                if (ssid_list.empty()) {
                    return std::string("{\"networks\":[]}");
                }

                cJSON* root = cJSON_CreateObject();
                cJSON* networks = cJSON_CreateArray();
                for (const auto& item : ssid_list) {
                    cJSON* net = cJSON_CreateObject();
                    cJSON_AddStringToObject(net, "ssid", item.ssid.c_str());
                    cJSON_AddBoolToObject(net, "has_password", !item.password.empty());
                    cJSON_AddItemToArray(networks, net);
                }
                cJSON_AddItemToObject(root, "networks", networks);

                char* json_str = cJSON_PrintUnformatted(root);
                std::string result(json_str);
                cJSON_free(json_str);
                cJSON_Delete(root);
                return result;
            });

        mcp_server.AddTool("self.wifi.forget_network",
            "Remove a saved WiFi network from the device memory.\n"
            "Args:\n"
            "  `ssid`: The name of the WiFi network to forget.\n"
            "Use this tool when a saved network has the wrong password or the user no longer needs it.",
            PropertyList({
                Property("ssid", kPropertyTypeString)
            }), [](const PropertyList& properties) -> ReturnValue {
                std::string ssid = properties["ssid"].value<std::string>();
                if (ssid.empty()) {
                    return std::string("{\"error\":\"SSID is required\"}");
                }

                auto& ssid_manager = SsidManager::GetInstance();
                auto& ssid_list = ssid_manager.GetSsidList();
                bool found = false;
                for (int i = 0; i < (int)ssid_list.size(); i++) {
                    if (ssid_list[i].ssid == ssid) {
                        ssid_manager.RemoveSsid(i);
                        found = true;
                        ESP_LOGI(TAG, "Forgot network: %s", ssid.c_str());
                        break;
                    }
                }
                if (!found) {
                    return std::string("{\"error\":\"Network " + ssid + " not found in saved networks.\"}");
                }
                return std::string("{\"success\":true,\"message\":\"Network " + ssid + " has been removed from saved networks.\"}");
            });

        mcp_server.AddTool("self.market.analyze",
            "Analyze the gold or Bitcoin market: download fresh candles from Yahoo Finance, "
            "compute technical indicators (RSI, MACD, EMA, ATR, ADX, Bollinger, volume), "
            "reduce them to an overall score and run one cycle of the trading bot.\n"
            "Args:\n"
            "  `asset`: `gold` (Gold futures, GC=F) or `btc` (Bitcoin). Defaults to `btc`.\n"
            "Returns: a JSON object with:\n"
            "  - price and per-indicator values with a short reading;\n"
            "  - `score` from -6 (strong bearish) to +6 (strong bullish) and a `verdict`;\n"
            "  - `bot`: action (BUY, STRONG_BUY, SELL, HOLD, STOP_LOSS, TAKE_PROFIT, "
            "TIME_EXIT or WAIT), message, cash, position and total equity.\n"
            "The bot state is persisted on the device, so calling this tool can open or "
            "close a position and change the balance. Use it when the user asks for the "
            "gold/Bitcoin price, the current market signal or the bot's portfolio.",
            PropertyList({
                Property("asset", kPropertyTypeString, std::string("btc"))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return MarketNotifier::Analyze(properties["asset"].value<std::string>());
            });
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(MyBoard);
