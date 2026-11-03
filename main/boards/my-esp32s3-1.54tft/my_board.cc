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
#include <lwip/dns.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <lwip/ip_addr.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "MyBoard"
#define NVS_SNIFFER_NAMESPACE "sniffer"
#define MAX_SNIFFER_LOG 100

// Defined next to the DNS hook at the bottom of this file: they resolve host
// names over DNS-over-HTTPS and are used by the board constructor and by the
// self.net.doh tool.
void StartDohRefreshTask();
std::string ResolveViaDohTool(const std::string& host);

static const wifi_promiscuous_filter_t filter = {
    .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA
};

// Raw TCP handshake probe, exposed as the self.net.probe tool.
//
// esp-tls reports a refused or filtered connection as a single generic
// "Failed to open new connection in specified timeout" after 10 s, which
// cannot tell "the host is down" from "the port is filtered" from "DNS gave a
// black-holed address". Doing the handshake by hand records what DNS returned,
// how long each SYN waited and the exact errno, so the answer comes from the
// device itself instead of from a machine on a different network.
static std::string ProbeHost(const std::string& host, int port, int timeout_ms) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;  // IPv6 first if the stack prefers it
    hints.ai_socktype = SOCK_STREAM;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "host", host.c_str());
    cJSON_AddNumberToObject(root, "port", port);
    cJSON_AddNumberToObject(root, "timeout_ms", timeout_ms);

    cJSON* resolved = cJSON_AddArrayToObject(root, "resolved");

    int64_t started_us = esp_timer_get_time();
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), port_str, &hints, &res);
    int64_t dns_ms = (esp_timer_get_time() - started_us) / 1000;

    if (rc != 0 || res == nullptr) {
        cJSON_AddNumberToObject(root, "dns_ms", dns_ms);
        cJSON_AddNumberToObject(root, "dns_rc", rc);
        cJSON_AddBoolToObject(root, "ok", false);
        char* printed = cJSON_PrintUnformatted(root);
        std::string out = printed ? printed : "{\"error\":\"dns failed\"}";
        cJSON_free(printed);
        cJSON_Delete(root);
        return out;
    }

    int64_t deadline_us = started_us + static_cast<int64_t>(timeout_ms) * 1000;
    bool connected = false;
    cJSON* attempts = cJSON_AddArrayToObject(root, "attempts");

    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char ip[INET6_ADDRSTRLEN] = {0};
        if (p->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip));
        } else if (p->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr, ip, sizeof(ip));
        }
        cJSON_AddItemToArray(resolved, cJSON_CreateString(ip));

        int64_t remain_us = deadline_us - esp_timer_get_time();
        if (remain_us <= 0) {
            break;  // the budget is spent, later addresses have no chance
        }

        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            cJSON* a = cJSON_CreateObject();
            cJSON_AddItemToArray(attempts, a);
            cJSON_AddStringToObject(a, "ip", ip);
            cJSON_AddStringToObject(a, "stage", "socket");
            cJSON_AddNumberToObject(a, "errno", errno);
            cJSON_AddStringToObject(a, "error", strerror(errno));
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int64_t t0 = esp_timer_get_time();
        int ret = connect(fd, p->ai_addr, p->ai_addrlen);
        int err = 0;

        if (ret < 0 && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv;
            int64_t remain_ms = remain_us / 1000;
            tv.tv_sec = static_cast<time_t>(remain_ms / 1000);
            tv.tv_usec = static_cast<suseconds_t>((remain_ms % 1000) * 1000);

            ret = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (ret > 0) {
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            } else if (ret == 0) {
                err = ETIMEDOUT;  // SYN never answered within the budget
            } else {
                err = errno;
            }
        } else if (ret != 0) {
            err = errno;  // immediate refusal / unreachable / no route
        }

        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        close(fd);

        cJSON* a = cJSON_CreateObject();
        cJSON_AddItemToArray(attempts, a);
        cJSON_AddStringToObject(a, "ip", ip);
        cJSON_AddNumberToObject(a, "ms", elapsed_ms);
        cJSON_AddNumberToObject(a, "errno", err);
        cJSON_AddStringToObject(a, "error", err == 0 ? "connected" : strerror(err));

        if (err == 0) {
            connected = true;
            break;  // one working address is enough
        }
    }

    freeaddrinfo(res);

    cJSON_AddNumberToObject(root, "dns_ms", dns_ms);
    cJSON_AddBoolToObject(root, "ok", connected);
    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed ? printed : "{\"error\":\"out of memory\"}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return out;
}

class MyBoard : public WifiBoard {
private:
    struct WifiScanTaskParam {
        MyBoard* self;
        int reply_id;
    };

    struct WifiConnectTaskParam {
        int reply_id;
        std::string ssid;
        std::string password;
    };

    struct SnifferPacket {
        std::string description;
        std::string short_info;
    };

    static bool sniffer_active_;
    static bool sniffer_disconnected_;
    static TaskHandle_t sniffer_task_handle_;
    static QueueHandle_t sniffer_queue_;
    static std::vector<SnifferPacket> sniffer_log_;
    static int sniffer_duration_seconds_;

    // Owns the trading bot state and the background monitor task, so that the
    // self.market.analyze and self.market.monitor tools share one instance.
    MarketNotifier market_notifier_;

public:
    struct DnsQuery {
        uint32_t timestamp_sec;
        std::string hostname;
    };
    static std::vector<DnsQuery> dns_log_;
    static constexpr size_t MAX_DNS_LOG = 50;

    static void SaveSnifferLogToNvs() {
        nvs_handle_t handle;
        if (nvs_open(NVS_SNIFFER_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open NVS for sniffer log");
            return;
        }

        // Clear old entries first
        nvs_erase_all(handle);

        // Save count
        uint32_t count = sniffer_log_.size();
        nvs_set_u32(handle, "count", count);

        // Save each entry
        for (int i = 0; i < (int)count; i++) {
            std::string key = "s" + std::to_string(i);
            std::string value = sniffer_log_[i].short_info + "|" + sniffer_log_[i].description;
            nvs_set_str(handle, key.c_str(), value.c_str());
        }

        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Saved %d sniffer entries to NVS", count);
    }

    static void LoadSnifferLogFromNvs() {
        nvs_handle_t handle;
        if (nvs_open(NVS_SNIFFER_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            ESP_LOGI(TAG, "No saved sniffer log found");
            return;
        }

        uint32_t count = 0;
        if (nvs_get_u32(handle, "count", &count) != ESP_OK || count == 0) {
            nvs_close(handle);
            return;
        }

        sniffer_log_.clear();
        for (int i = 0; i < (int)count && i < MAX_SNIFFER_LOG; i++) {
            std::string key = "s" + std::to_string(i);
            char buf[512];
            size_t len = sizeof(buf);
            if (nvs_get_str(handle, key.c_str(), buf, &len) == ESP_OK) {
                std::string val(buf);
                size_t sep = val.find('|');
                if (sep != std::string::npos) {
                    sniffer_log_.push_back({val.substr(sep + 1), val.substr(0, sep)});
                }
            }
        }

        nvs_close(handle);
        ESP_LOGI(TAG, "Loaded %d sniffer entries from NVS", (int)sniffer_log_.size());
    }

    static void ClearSnifferLogFromNvs() {
        nvs_handle_t handle;
        if (nvs_open(NVS_SNIFFER_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_all(handle);
            nvs_commit(handle);
            nvs_close(handle);
        }
        sniffer_log_.clear();
        ESP_LOGI(TAG, "Sniffer log cleared");
    }

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

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
        // Diagnostic: esp-tls hides the connect/handshake stages behind
        // ESP_LOGD, which is compiled out at CONFIG_LOG_MAXIMUM_LEVEL=3. With
        // the maximum raised to 4 (the default level stays INFO, so nothing
        // else gets noisier) this shows the exact address esp-tls picks and
        // whether it ever reaches the TLS handshake.
        esp_log_level_set("esp-tls", ESP_LOG_DEBUG);
        esp_log_level_set("EspSsl", ESP_LOG_DEBUG);

        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        GetBacklight()->RestoreBrightness();

        InitializeTools();

        // Refresh the DNS-over-HTTPS answers in the background; without them
        // the market tools connect to a dead Yahoo edge address.
        StartDohRefreshTask();

        // Load persisted sniffer log from NVS
        LoadSnifferLogFromNvs();
    }

    static void WifiScanTask(void* param) {
        auto* p = static_cast<WifiScanTaskParam*>(param);
        auto* self = p->self;
        int captured_id = p->reply_id;
        delete p;

        auto display = self->GetDisplay();
        auto& wifi_mgr = WifiManager::GetInstance();

        display->SetChatMessage("system", "Scanning WiFi...");
        ESP_LOGI(TAG, "Background scan task started on core %d, priority %d", xPortGetCoreID(), uxTaskPriorityGet(nullptr));

        wifi_mgr.SetExternalScanMode(true);
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

        wifi_mgr.SetExternalScanMode(false);
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

    static void SnifferCallback(void* recv_buf, wifi_promiscuous_pkt_type_t type) {
        if (!sniffer_active_) return;
        if (type == WIFI_PKT_MISC) return;

        auto* pkt = (wifi_promiscuous_pkt_t*)recv_buf;
        const uint8_t* frame = pkt->payload;
        uint16_t len = pkt->rx_ctrl.sig_len;
        int8_t rssi = pkt->rx_ctrl.rssi;
        uint8_t channel = pkt->rx_ctrl.channel;

        if (len < 2) return;

        uint16_t frame_ctrl = frame[0] | (frame[1] << 8);
        uint8_t frame_subtype = (frame_ctrl >> 4) & 0xf;

        char sa[18] = "??:??:??:??:??:??";
        char da[18] = "??:??:??:??:??:??";
        char bssid[18] = "??:??:??:??:??:??";
        if (len >= 24) {
            snprintf(sa, sizeof(sa), "%02x:%02x:%02x:%02x:%02x:%02x", frame[10], frame[11], frame[12], frame[13], frame[14], frame[15]);
            snprintf(da, sizeof(da), "%02x:%02x:%02x:%02x:%02x:%02x", frame[4], frame[5], frame[6], frame[7], frame[8], frame[9]);
            snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", frame[16], frame[17], frame[18], frame[19], frame[20], frame[21]);
        }

        char desc[1024] = {0};
        char info[128] = {0};

        if (type == WIFI_PKT_MGMT) {
            if (frame_subtype == 0x08 && len > 24) {
                // Beacon
                char ssid[33] = {0};
                int offset = 24;
                while (offset + 1 < len) {
                    uint8_t eid = frame[offset];
                    uint8_t elen = frame[offset + 1];
                    if (eid == 0 && elen > 0 && elen < 33) {
                        memcpy(ssid, frame + offset + 2, elen);
                        break;
                    }
                    offset += 2 + elen;
                }
                snprintf(info, sizeof(info), "BEACON ch%d", channel);
                snprintf(desc, sizeof(desc), "BEACON %s \"%s\" %ddBm", sa, ssid, rssi);
            } else if (frame_subtype == 0x04 && len > 24) {
                // Probe Request
                char ssid[33] = {0};
                int offset = 24;
                while (offset + 1 < len) {
                    uint8_t eid = frame[offset];
                    uint8_t elen = frame[offset + 1];
                    if (eid == 0 && elen > 0 && elen < 33) {
                        memcpy(ssid, frame + offset + 2, elen);
                        break;
                    }
                    offset += 2 + elen;
                }
                snprintf(info, sizeof(info), "PROBE_REQ");
                snprintf(desc, sizeof(desc), "PROBE_REQ %s \"%s\" %ddBm", sa, ssid, rssi);
            } else if (frame_subtype == 0x0C) {
                snprintf(info, sizeof(info), "DEAUTH");
                snprintf(desc, sizeof(desc), "DEAUTH %s -> %s %ddBm", sa, da, rssi);
            } else if (frame_subtype == 0x00) {
                snprintf(info, sizeof(info), "ASSOC_REQ");
                snprintf(desc, sizeof(desc), "ASSOC_REQ %s -> %s %ddBm", sa, bssid, rssi);
            } else if (frame_subtype == 0x01) {
                snprintf(info, sizeof(info), "ASSOC_RESP");
                snprintf(desc, sizeof(desc), "ASSOC_RESP %s -> %s %ddBm", bssid, sa, rssi);
            } else if (frame_subtype == 0x0B) {
                snprintf(info, sizeof(info), "AUTH");
                snprintf(desc, sizeof(desc), "AUTH %s -> %s %ddBm", sa, bssid, rssi);
            } else if (frame_subtype == 0x0D) {
                snprintf(info, sizeof(info), "DISASSOC");
                snprintf(desc, sizeof(desc), "DISASSOC %s -> %s %ddBm", sa, da, rssi);
            } else {
                snprintf(info, sizeof(info), "MGMT_%02x", frame_subtype);
                snprintf(desc, sizeof(desc), "MGMT sub=%d from=%s %ddBm", frame_subtype, sa, rssi);
            }
        }
        else if (type == WIFI_PKT_DATA) {
            if (len < 24) return;

            // Try to parse LLC/SNAP + IPv4 for extra detail
            int snap_offset = -1;
            if (len > 24) {
                // Find LLC/SNAP header
                for (int i = 24; i < (int)len - 8; i++) {
                    if (frame[i] == 0xaa && frame[i+1] == 0xaa && frame[i+2] == 0x03) {
                        snap_offset = i;
                        break;
                    }
                }
            }

            if (snap_offset >= 0) {
                uint16_t ethertype = (frame[snap_offset + 6] << 8) | frame[snap_offset + 7];
                int ip_offset = snap_offset + 8;

                if (ethertype == 0x0800 && ip_offset + 20 <= (int)len) {
                    // IPv4
                    uint8_t protocol = frame[ip_offset + 9];
                    char src_ip[16], dst_ip[16];
                    snprintf(src_ip, sizeof(src_ip), "%d.%d.%d.%d", frame[ip_offset+12], frame[ip_offset+13], frame[ip_offset+14], frame[ip_offset+15]);
                    snprintf(dst_ip, sizeof(dst_ip), "%d.%d.%d.%d", frame[ip_offset+16], frame[ip_offset+17], frame[ip_offset+18], frame[ip_offset+19]);

                    char extra[256] = {0};
                    std::string extra_str;
                    int transport_offset = ip_offset + (frame[ip_offset] & 0x0f) * 4;

                    if (protocol == 6 && transport_offset + 20 <= (int)len) {
                        uint16_t sport = (frame[transport_offset] << 8) | frame[transport_offset+1];
                        uint16_t dport = (frame[transport_offset+2] << 8) | frame[transport_offset+3];
                        uint8_t flags = frame[transport_offset + 13];
                        char flag_str[8] = {0};
                        int f = 0;
                        if (flags & 0x02) flag_str[f++] = 'S';
                        if (flags & 0x10) flag_str[f++] = 'A';
                        if (flags & 0x01) flag_str[f++] = 'F';
                        if (flags & 0x04) flag_str[f++] = 'R';
                        if (flags & 0x08) flag_str[f++] = 'P';
                        snprintf(extra, sizeof(extra), " %s:%d->%s:%d [%s]", src_ip, sport, dst_ip, dport, flag_str);
                        snprintf(info, sizeof(info), "TCP [%s]", flag_str);
                    } else if (protocol == 17 && transport_offset + 8 <= (int)len) {
                        uint16_t sport = (frame[transport_offset] << 8) | frame[transport_offset+1];
                        uint16_t dport = (frame[transport_offset+2] << 8) | frame[transport_offset+3];
                        snprintf(extra, sizeof(extra), " %s:%d->%s:%d", src_ip, sport, dst_ip, dport);

                        int udp_payload = transport_offset + 8;

                        if ((dport == 53 || sport == 53) && udp_payload + 12 <= (int)len) {
                            // DNS packet
                            uint16_t dns_flags = (frame[udp_payload+2] << 8) | frame[udp_payload+3];
                            uint16_t ancount = (frame[udp_payload+6] << 8) | frame[udp_payload+7];
                            bool is_response = (dns_flags >> 15) & 1;

                            char dns_name[256] = {0};
                            int pos = udp_payload + 12;

                            // Parse query name
                            for (int label = 0; label < 128 && pos < (int)len; label++) {
                                uint8_t label_len = frame[pos];
                                if (label_len == 0) { pos++; break; }
                                if (label_len > 63 || pos + 1 + label_len > (int)len) break;
                                if (label > 0 && strlen(dns_name) < sizeof(dns_name) - 1) {
                                    strcat(dns_name, ".");
                                }
                                int copy_len = label_len;
                                if (strlen(dns_name) + label_len >= sizeof(dns_name) - 1) {
                                    copy_len = sizeof(dns_name) - 2 - strlen(dns_name);
                                }
                                if (copy_len > 0) {
                                    strncat(dns_name, (const char*)(frame + pos + 1), copy_len);
                                }
                                pos += 1 + label_len;
                            }

                            // Skip query type and class
                            if (is_response) {
                                // Parse answers to get resolved IPs
                                pos += 4; // skip type + class in question
                                char resolved_ips[256] = {0};
                                for (int a = 0; a < ancount && a < 10 && pos + 10 <= (int)len; a++) {
                                    // Skip name (could be pointer)
                                    if ((frame[pos] & 0xC0) == 0xC0) {
                                        pos += 2;
                                    } else {
                                        while (pos < (int)len && frame[pos] != 0) pos++;
                                        pos++;
                                    }
                                    if (pos + 10 > (int)len) break;
                                    uint16_t rr_type = (frame[pos] << 8) | frame[pos+1];
                                    // uint16_t rr_len = (frame[pos+8] << 8) | frame[pos+9];
                                    pos += 10; // type(2) + class(2) + ttl(4) + rdlen(2)

                                    if (rr_type == 1 && pos + 4 <= (int)len) {
                                        // A record
                                        char a_ip[16];
                                        snprintf(a_ip, sizeof(a_ip), "%d.%d.%d.%d", frame[pos], frame[pos+1], frame[pos+2], frame[pos+3]);
                                        if (resolved_ips[0]) strcat(resolved_ips, ", ");
                                        strcat(resolved_ips, a_ip);
                                        pos += 4;
                                    } else if (rr_type == 28 && pos + 16 <= (int)len) {
                                        // AAAA record (skip)
                                        pos += 16;
                                    } else {
                                        uint16_t rdlen = (frame[pos-2] << 8) | frame[pos-1];
                                        pos += rdlen;
                                    }
                                }

                                snprintf(info, sizeof(info), "DNS_RESP");
                                if (dns_name[0] && resolved_ips[0]) {
                                    extra_str = std::string(" ") + src_ip + " -> " + dns_name + " [" + resolved_ips + "]";
                                } else if (dns_name[0]) {
                                    extra_str = std::string(" ") + src_ip + " -> " + dns_name;
                                }
                            } else {
                                // DNS query
                                snprintf(info, sizeof(info), "DNS_QUERY");
                                if (dns_name[0]) {
                                    uint16_t qtype = (frame[pos] << 8) | frame[pos+1];
                                    const char* type_str = "A";
                                    if (qtype == 28) type_str = "AAAA";
                                    else if (qtype == 12) type_str = "PTR";
                                    else if (qtype == 5) type_str = "CNAME";
                                    else if (qtype == 15) type_str = "MX";
                                    else if (qtype == 16) type_str = "TXT";
                                    extra_str = std::string(" ") + src_ip + " -> " + dns_name + " (" + type_str + ")";
                                }
                            }
                        } else if (dport == 5353 || sport == 5353) {
                            snprintf(info, sizeof(info), "mDNS");
                        } else {
                            snprintf(info, sizeof(info), "UDP");
                        }
                    } else {
                        snprintf(extra, sizeof(extra), " %s->%s proto=%d", src_ip, dst_ip, protocol);
                        snprintf(info, sizeof(info), "IP_%d", protocol);
                    }

                    // Build desc with string to avoid format-truncation
                    {
                        std::string extra_part = extra_str.empty() ? extra : extra_str;
                        std::string d = std::string("DATA ") + sa + "->" + da + " [" + info + "]" + extra_part + " " + std::to_string(rssi) + "dBm ch" + std::to_string(channel);
                        snprintf(desc, sizeof(desc), "%s", d.c_str());
                    }
                } else if (ethertype == 0x0806) {
                    snprintf(info, sizeof(info), "ARP");
                    snprintf(desc, sizeof(desc), "ARP %s %ddBm ch%d", sa, rssi, channel);
                } else {
                    snprintf(info, sizeof(info), "DATA_0x%04x", ethertype);
                    snprintf(desc, sizeof(desc), "DATA %s->%s eth=0x%04x %ddBm ch%d", sa, da, ethertype, rssi, channel);
                }
            } else {
                snprintf(info, sizeof(info), "DATA");
                snprintf(desc, sizeof(desc), "DATA %s->%s len=%d %ddBm ch%d", sa, da, len, rssi, channel);
            }
        }
        else {
            return;
        }

        if (desc[0] == 0) return;

        auto* entry = new SnifferPacket();
        entry->description = desc;
        entry->short_info = info;

        if (xQueueSend(sniffer_queue_, &entry, 0) != pdTRUE) {
            delete entry;
        }
    }

    static void SnifferTask(void* arg) {
        int duration_sec = (int)(uintptr_t)arg;
        ESP_LOGI(TAG, "Sniffer task started, will run for %d seconds", duration_sec);

        auto& app = Application::GetInstance();
        char msg[64];
        snprintf(msg, sizeof(msg), "Sniffer: %ds remaining...", duration_sec);
        app.Alert("Sniffer", msg, "radar", Lang::Sounds::OGG_POPUP);

        int elapsed = 0;
        while (sniffer_active_ && elapsed < duration_sec) {
            // Wait for packets with 1 second timeout
            SnifferPacket* pkt = nullptr;
            if (sniffer_queue_ && xQueueReceive(sniffer_queue_, &pkt, pdMS_TO_TICKS(1000)) == pdTRUE && pkt) {
                app.Alert("Network", pkt->short_info.c_str(), "wifi");

                if (sniffer_log_.size() >= MAX_SNIFFER_LOG) {
                    sniffer_log_.erase(sniffer_log_.begin());
                }
                sniffer_log_.push_back(*pkt);

                ESP_LOGI(TAG, "[SNIFFER] %s", pkt->description.c_str());
                delete pkt;
            }
            elapsed++;

            // Update countdown on screen every 5 seconds
            if (elapsed % 5 == 0 && elapsed < duration_sec) {
                snprintf(msg, sizeof(msg), "Sniffer: %ds remaining...", duration_sec - elapsed);
                app.Alert("Sniffer", msg, "radar");
            }
        }

        // Auto-stop: disable promiscuous mode
        sniffer_active_ = false;
        esp_wifi_set_promiscuous(false);

        // Re-enable WifiStation auto behavior
        auto& wifi_mgr = WifiManager::GetInstance();
        wifi_mgr.SetExternalScanMode(false);

        // Reconnect if we were in disconnected mode
        if (sniffer_disconnected_) {
            sniffer_disconnected_ = false;
            wifi_mgr.Reconnect();
        }

        if (sniffer_queue_) {
            SnifferPacket* pkt;
            while (xQueueReceive(sniffer_queue_, &pkt, 0) == pdTRUE) {
                delete pkt;
            }
            vQueueDelete(sniffer_queue_);
            sniffer_queue_ = nullptr;
        }

        ESP_LOGI(TAG, "Sniffer auto-stopped after %ds, %d packets captured",
                 duration_sec, (int)sniffer_log_.size());

        // Persist log to NVS
        SaveSnifferLogToNvs();

        char result_msg[128];
        snprintf(result_msg, sizeof(result_msg), "Sniffer done: %d packets in %ds.",
                 (int)sniffer_log_.size(), duration_sec);
        app.Alert("Sniffer", result_msg, "radar");

        sniffer_task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }

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

        mcp_server.AddTool("self.wifi.start_sniffer",
            "Start WiFi packet sniffer for a fixed duration.\n"
            "Default: captures WiFi management frames while staying connected.\n"
            "With disconnected=true: disconnects WiFi to capture ALL traffic including DNS queries from other devices.\n"
            "After the duration, the sniffer auto-stops and WiFi reconnects.\n"
            "After it finishes, use self.wifi.get_sniffer_log to see captured packets.",
            PropertyList({
                Property("duration_seconds", kPropertyTypeInteger, 10),
                Property("disconnected", kPropertyTypeBoolean, false)
            }), [](const PropertyList& properties) -> ReturnValue {
                if (sniffer_active_) {
                    return std::string("{\"error\":\"Sniffer is already running\"}");
                }

                auto& wifi_mgr = WifiManager::GetInstance();
                if (!wifi_mgr.IsInitialized() || wifi_mgr.IsConfigMode()) {
                    return std::string("{\"error\":\"WiFi not connected\"}");
                }

                int duration = properties["duration_seconds"].value<int>();
                if (duration < 3) duration = 3;
                if (duration > 60) duration = 60;
                bool disconnected = properties["disconnected"].value<bool>();

                // Get current channel
                uint8_t primary;
                wifi_second_chan_t second;
                esp_wifi_get_channel(&primary, &second);

                sniffer_active_ = true;
                sniffer_log_.clear();
                sniffer_queue_ = xQueueCreate(32, sizeof(SnifferPacket*));

                if (disconnected) {
                    // Disconnect mode: captures ALL traffic on channel
                    sniffer_disconnected_ = true;
                    wifi_mgr.SetExternalScanMode(true);
                    esp_wifi_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(1500));

                    esp_wifi_set_channel(primary, second);
                    esp_wifi_set_promiscuous_rx_cb(SnifferCallback);
                    esp_wifi_set_promiscuous(true);

                    ESP_LOGI(TAG, "WiFi sniffer (disconnected) enabled on channel %d for %d seconds", primary, duration);
                    xTaskCreatePinnedToCore(SnifferTask, "sniffer", 4096, (void*)(uintptr_t)duration, 18, &sniffer_task_handle_, 1);

                    return std::string("{\"success\":true,\"message\":\"Sniffer started (disconnected mode) for " + std::to_string(duration) + " seconds on channel " + std::to_string(primary) + ". Capturing ALL traffic including DNS.\",\"duration\":" + std::to_string(duration) + ",\"mode\":\"disconnected\"}");
                } else {
                    // Connected mode: captures management frames + some data
                    sniffer_disconnected_ = false;
                    wifi_mgr.SetExternalScanMode(true);
                    esp_wifi_set_promiscuous_rx_cb(SnifferCallback);
                    esp_wifi_set_promiscuous(true);
                    esp_wifi_set_ps(WIFI_PS_NONE);

                    ESP_LOGI(TAG, "WiFi sniffer (connected) enabled on channel %d for %d seconds", primary, duration);
                    xTaskCreatePinnedToCore(SnifferTask, "sniffer", 4096, (void*)(uintptr_t)duration, 18, &sniffer_task_handle_, 1);

                    return std::string("{\"success\":true,\"message\":\"Sniffer started for " + std::to_string(duration) + " seconds on channel " + std::to_string(primary) + ". Capturing WiFi management frames.\",\"duration\":" + std::to_string(duration) + ",\"mode\":\"connected\"}");
                }
            });

        mcp_server.AddTool("self.wifi.get_sniffer_log",
            "Get the log of captured WiFi packets from the sniffer.\n"
            "Returns: A list of captured network packets with source/destination IPs, protocol, and details.\n"
            "Use this to analyze what traffic was captured.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                if (sniffer_log_.empty()) {
                    return std::string("{\"packets\":[]}");
                }

                cJSON* root = cJSON_CreateObject();
                cJSON* packets = cJSON_CreateArray();

                // Return last 50 packets
                size_t start = sniffer_log_.size() > 50 ? sniffer_log_.size() - 50 : 0;
                for (size_t i = start; i < sniffer_log_.size(); i++) {
                    cJSON* pkt_json = cJSON_CreateObject();
                    cJSON_AddStringToObject(pkt_json, "info", sniffer_log_[i].short_info.c_str());
                    cJSON_AddStringToObject(pkt_json, "details", sniffer_log_[i].description.c_str());
                    cJSON_AddItemToArray(packets, pkt_json);
                }
                cJSON_AddItemToObject(root, "packets", packets);
                cJSON_AddNumberToObject(root, "total", sniffer_log_.size());

                char* json_str = cJSON_PrintUnformatted(root);
                std::string result(json_str);
                cJSON_free(json_str);
                cJSON_Delete(root);
                return result;
            });

        mcp_server.AddTool("self.wifi.clear_sniffer_log",
            "Clear all saved sniffer log entries from device memory.\n"
            "Use this when the user wants to delete the captured traffic log.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                int count = sniffer_log_.size();
                ClearSnifferLogFromNvs();
                return std::string("{\"success\":true,\"message\":\"Cleared " + std::to_string(count) + " sniffer log entries.\"}");
            });

        mcp_server.AddTool("self.wifi.get_dns_log",
            "Get DNS query log captured by LwIP hook.\n"
            "Returns list of DNS queries made by this device (hostname + timestamp).\n"
            "Use this to see what hostnames the device is resolving.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                cJSON* root = cJSON_CreateObject();
                cJSON* queries = cJSON_CreateArray();
                uint32_t now = esp_timer_get_time() / 1000000;
                for (const auto& q : dns_log_) {
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "hostname", q.hostname.c_str());
                    cJSON_AddNumberToObject(item, "seconds_ago", now - q.timestamp_sec);
                    cJSON_AddItemToArray(queries, item);
                }
                cJSON_AddItemToObject(root, "dns_queries", queries);
                cJSON_AddNumberToObject(root, "total", dns_log_.size());
                char* json = cJSON_PrintUnformatted(root);
                std::string result(json);
                cJSON_free(json);
                cJSON_Delete(root);
                return result;
            });

        mcp_server.AddTool("self.get_memory_info",
            "Get detailed memory usage information by memory type.\n"
            "Returns: Internal RAM, PSRAM, DMA, and task stack usage.\n"
            "Use this to check how much memory is available on the device.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                cJSON* root = cJSON_CreateObject();

                // Internal DRAM (heap_caps_get_free_size with MALLOC_CAP_8BIT)
                size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
                size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                size_t dram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
                cJSON* dram = cJSON_CreateObject();
                cJSON_AddNumberToObject(dram, "total", dram_total);
                cJSON_AddNumberToObject(dram, "free", dram_free);
                cJSON_AddNumberToObject(dram, "min_free", dram_min);
                cJSON_AddNumberToObject(dram, "used", dram_total - dram_free);
                cJSON_AddNumberToObject(dram, "usage_pct", dram_total > 0 ? (int)((dram_total - dram_free) * 100 / dram_total) : 0);
                cJSON_AddItemToObject(root, "internal_dram", dram);

                // Internal IRAM
                size_t iram_total = heap_caps_get_total_size(MALLOC_CAP_32BIT);
                size_t iram_free = heap_caps_get_free_size(MALLOC_CAP_32BIT);
                size_t iram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_32BIT);
                cJSON* iram = cJSON_CreateObject();
                cJSON_AddNumberToObject(iram, "total", iram_total);
                cJSON_AddNumberToObject(iram, "free", iram_free);
                cJSON_AddNumberToObject(iram, "min_free", iram_min);
                cJSON_AddNumberToObject(iram, "used", iram_total - iram_free);
                cJSON_AddNumberToObject(iram, "usage_pct", iram_total > 0 ? (int)((iram_total - iram_free) * 100 / iram_total) : 0);
                cJSON_AddItemToObject(root, "internal_iram", iram);

                // PSRAM
                size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
                cJSON* psram = cJSON_CreateObject();
                cJSON_AddNumberToObject(psram, "total", psram_total);
                cJSON_AddNumberToObject(psram, "free", psram_free);
                cJSON_AddNumberToObject(psram, "min_free", psram_min);
                cJSON_AddNumberToObject(psram, "used", psram_total - psram_free);
                cJSON_AddNumberToObject(psram, "usage_pct", psram_total > 0 ? (int)((psram_total - psram_free) * 100 / psram_total) : 0);
                cJSON_AddItemToObject(root, "psram", psram);

                // DMA-capable memory
                size_t dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);
                size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
                size_t dma_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
                cJSON* dma = cJSON_CreateObject();
                cJSON_AddNumberToObject(dma, "total", dma_total);
                cJSON_AddNumberToObject(dma, "free", dma_free);
                cJSON_AddNumberToObject(dma, "min_free", dma_min);
                cJSON_AddNumberToObject(dma, "used", dma_total - dma_free);
                cJSON_AddNumberToObject(dma, "usage_pct", dma_total > 0 ? (int)((dma_total - dma_free) * 100 / dma_total) : 0);
                cJSON_AddItemToObject(root, "dma", dma);

                // Overall heap summary
                cJSON* summary = cJSON_CreateObject();
                cJSON_AddNumberToObject(summary, "total_heap", heap_caps_get_total_size(MALLOC_CAP_DEFAULT));
                cJSON_AddNumberToObject(summary, "free_heap", esp_get_free_heap_size());
                cJSON_AddNumberToObject(summary, "min_free_heap", esp_get_minimum_free_heap_size());
                cJSON_AddItemToObject(root, "heap_summary", summary);

                // NVS (Non-Volatile Storage)
                cJSON* nvs_json = cJSON_CreateObject();
                nvs_stats_t nvs_stats;
                if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
                    cJSON_AddNumberToObject(nvs_json, "total_entries", nvs_stats.total_entries);
                    cJSON_AddNumberToObject(nvs_json, "used_entries", nvs_stats.used_entries);
                    cJSON_AddNumberToObject(nvs_json, "free_entries", nvs_stats.free_entries);
                    cJSON_AddNumberToObject(nvs_json, "usage_pct", nvs_stats.total_entries > 0 ?
                        (int)(nvs_stats.used_entries * 100 / nvs_stats.total_entries) : 0);
                }
                const esp_partition_t* nvs_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
                if (nvs_part) {
                    cJSON_AddNumberToObject(nvs_json, "partition_size", nvs_part->size);
                    cJSON_AddStringToObject(nvs_json, "partition_label", nvs_part->label);
                }
                cJSON_AddItemToObject(root, "nvs", nvs_json);

                // Top task stacks
                cJSON* tasks = cJSON_CreateArray();
                UBaseType_t task_count = uxTaskGetNumberOfTasks();
                TaskStatus_t* task_status = (TaskStatus_t*)pvPortMalloc(task_count * sizeof(TaskStatus_t));
                if (task_status) {
                    uint32_t total_runtime;
                    task_count = uxTaskGetSystemState(task_status, task_count, &total_runtime);
                    for (UBaseType_t i = 0; i < task_count; i++) {
                        cJSON* t = cJSON_CreateObject();
                        cJSON_AddStringToObject(t, "name", task_status[i].pcTaskName);
                        cJSON_AddNumberToObject(t, "stack_high", task_status[i].usStackHighWaterMark * 4);
                        cJSON_AddNumberToObject(t, "stack_min_pct", task_status[i].usStackHighWaterMark > 0 ?
                            (int)((1 - (float)task_status[i].usStackHighWaterMark * 4 / (float)configMINIMAL_STACK_SIZE / 4) * 100) : 0);
                        cJSON_AddNumberToObject(t, "priority", task_status[i].uxCurrentPriority);
                        cJSON_AddNumberToObject(t, "runtime_pct", task_status[i].ulRunTimeCounter * 100 / total_runtime);
                        cJSON_AddItemToArray(tasks, t);
                    }
                    vPortFree(task_status);
                }
                cJSON_AddItemToObject(root, "tasks", tasks);

                char* json_str = cJSON_PrintUnformatted(root);
                std::string result(json_str);
                cJSON_free(json_str);
                cJSON_Delete(root);
                return result;
            });

        // Not part of the upstream board: the market notifier is a C++ port of
        // https://github.com/spartacus1988/Gold_and_crypto_notifyer (main.py).
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
            [this](const PropertyList& properties) -> ReturnValue {
                return market_notifier_.Analyze(properties["asset"].value<std::string>());
            });

        // The Python script polled Yahoo every INTERVAL seconds from main().
        // The device cannot call MCP tools by itself, so the same loop lives in
        // a FreeRTOS task started and stopped through this tool.
        mcp_server.AddTool("self.market.monitor",
            "Start, stop or inspect the background market monitor: a task that keeps "
            "analyzing the market on its own every `interval_sec` seconds, without "
            "waiting for anyone to ask.\n"
            "While it runs the device plays a sound and shows a message on its display "
            "only when the trading bot actually opens or closes a position (BUY, "
            "STRONG_BUY, STOP_LOSS, TAKE_PROFIT, SELL, TIME_EXIT). HOLD and WAIT are "
            "silent, so a running monitor makes no noise in a quiet market.\n"
            "The monitor is held in RAM and is lost on reboot; the bot's money and "
            "position state are not, they survive in flash.\n"
            "Args:\n"
            "  `action`: `start` (default), `stop` or `status`.\n"
            "  `asset`: `gold` or `btc`, used by `start`. Defaults to `btc`.\n"
            "  `interval_sec`: polling period for `start`, 10-3600 seconds, "
            "defaults to 60.\n"
            "Returns: for `start` the first full analysis report plus a `monitor` "
            "object; otherwise a `monitor` object with `running`, `asset` and "
            "`interval_sec`.",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("start")),
                Property("asset", kPropertyTypeString, std::string("btc")),
                Property("interval_sec", kPropertyTypeInteger, 60, 10, 3600)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();
                if (action == "stop") {
                    return market_notifier_.StopMonitor();
                }
                if (action == "status") {
                    return market_notifier_.MonitorStatus();
                }
                if (action != "start") {
                    return std::string("{\"error\":\"action must be start, stop or status\"}");
                }
                return market_notifier_.StartMonitor(
                    properties["asset"].value<std::string>(),
                    properties["interval_sec"].value<int>());
            });

        // Connectivity diagnosis from the device itself. The market tools fail
        // with a generic "connection failed" that says nothing about which hop
        // is broken; this runs a plain TCP handshake and reports the resolved
        // addresses, the wait per address and the errno.
        mcp_server.AddTool("self.net.probe",
            "Test raw TCP connectivity from the device to a host and port. Use it "
            "when another network tool fails with a generic connection error: it "
            "reports every address DNS returned, how long the handshake waited "
            "and the exact errno, which tells 'host is down' and 'port is "
            "filtered' apart.\n"
            "Args:\n"
            "  `host`: hostname or IP address.\n"
            "  `port`: TCP port, 1-65535, defaults to 443.\n"
            "  `timeout_ms`: total budget for the whole attempt, 1000-15000, "
            "defaults to 5000.\n"
            "Returns: JSON with `ok`, `dns_ms`, `resolved` (all IPs) and "
            "`attempts` (ip, ms, errno, error per address).",
            PropertyList({
                Property("host", kPropertyTypeString, std::string("")),
                Property("port", kPropertyTypeInteger, 443, 1, 65535),
                Property("timeout_ms", kPropertyTypeInteger, 5000, 1000, 15000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string host = properties["host"].value<std::string>();
                if (host.empty()) {
                    return std::string("{\"error\":\"host is required\"}");
                }
                return ProbeHost(host,
                                 properties["port"].value<int>(),
                                 properties["timeout_ms"].value<int>());
            });

        // Diagnoses the class of failure where the local resolver hands out an
        // address that accepts TCP but never completes a TLS handshake. The
        // lookup goes over HTTPS to cloudflare-dns.com, so it bypasses the
        // local resolver entirely.
        mcp_server.AddTool("self.net.doh",
            "Resolve a hostname through DNS-over-HTTPS (cloudflare-dns.com) instead "
            "of the local resolver, and show what address the device would actually "
            "connect to. Use it when a host fails to connect while a plain TCP test "
            "to the same address succeeds: that means the local resolver returned a "
            "stale or hijacked address.\n"
            "Args:\n"
            "  `host`: hostname to resolve.\n"
            "Returns: JSON with `ok`, `ip` from the live DoH answer and `cached`, "
            "the address currently served by the device.",
            PropertyList({
                Property("host", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                return ResolveViaDohTool(properties["host"].value<std::string>());
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
        // Block LOW_POWER during sniffer to keep radio active for promiscuous mode
        if (sniffer_active_ && level == PowerSaveLevel::LOW_POWER) {
            return;
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

bool MyBoard::sniffer_active_ = false;
bool MyBoard::sniffer_disconnected_ = false;
TaskHandle_t MyBoard::sniffer_task_handle_ = nullptr;
QueueHandle_t MyBoard::sniffer_queue_ = nullptr;
std::vector<MyBoard::SnifferPacket> MyBoard::sniffer_log_;
int MyBoard::sniffer_duration_seconds_ = 10;
std::vector<MyBoard::DnsQuery> MyBoard::dns_log_;

// ---------- DNS-over-HTTPS fallback for hosts with stale resolver answers ----------
//
// Yahoo rotates its edge pool, but the resolvers on this network keep handing
// out addresses from an old pool: they accept a TCP connection and then never
// answer the TLS handshake. From the firmware that is indistinguishable from
// "Yahoo is blocked", and it made the market tools fail for a reason that had
// nothing to do with the port, the TLS stack or the Wi-Fi. Resolving those
// hosts over HTTPS gets a live address instead, and the hook below hands it to
// lwip so every consumer (esp-tls, the HTTP client, our own getaddrinfo) uses
// it while the SNI still carries the real hostname.
namespace {

struct DohAddr {
    ip_addr_t addr;
    uint32_t key;  // the IPv4 address as a u32, its identity in the bad list
};

struct DohPool {
    std::vector<DohAddr> addrs;
    size_t next = 0;  // round-robin cursor
};

std::map<std::string, DohPool> g_doh_cache;
// Addresses that refused to complete a TLS handshake -> epoch seconds until
// which they must not be handed out again.
std::map<uint32_t, uint32_t> g_bad_until;
// The address most recently served for a name, so that a failed connection
// can be attributed to the exact address that caused it.
std::map<std::string, uint32_t> g_last_served;
std::mutex g_doh_mutex;

// Only these names are answered from DoH; everything else keeps using the
// router's resolver, which is fine for every other host.
const char* kDohHosts[] = {
    "query1.finance.yahoo.com",
    "query2.finance.yahoo.com",
};

// A dead address is skipped for half an hour, which is long enough for the
// pool to move on and short enough to recover if it was a transient failure.
constexpr uint32_t kBadAddressSeconds = 30 * 60;

uint32_t NowSec() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}

uint32_t KeyOf(const ip_addr_t& addr) {
    return ip4_addr_get_u32(ip_2_ip4(&addr));
}

std::string KeyToString(uint32_t key) {
    ip4_addr_t ip4;
    ip4_addr_set_u32(&ip4, key);
    ip_addr_t addr;
    ip_addr_copy_from_ip4(addr, ip4);
    char buf[INET6_ADDRSTRLEN] = {0};
    ipaddr_ntoa_r(&addr, buf, sizeof(buf));
    return buf;
}

// One DoH query: GET https://cloudflare-dns.com/dns-query?name=<host>&type=A
// with Accept: application/dns-json, parsed with cJSON. The body is small
// (~300 B) but is still streamed, because the HTTP client deadlocks on any
// response larger than its 8 kB chunk buffer.
//
// Every A record in the answer is kept: esp-tls connects to the first address
// it is given and never tries the rest, so the caller has to be able to offer
// the next one when the first refuses to speak TLS.
bool ResolveViaDoh(const std::string& host, std::vector<std::string>& ips_out,
                   std::string& error_out) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error_out = "network is not available";
        return false;
    }

    auto http = network->CreateHttp(0);
    http->SetTimeout(15000);
    http->SetHeader("Accept", "application/dns-json");

    const std::string url =
        std::string("https://cloudflare-dns.com/dns-query?name=") + host + "&type=A";

    if (!http->Open("GET", url)) {
        error_out = "the DoH endpoint is unreachable";
        return false;
    }

    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        http->Close();
        error_out = "DoH endpoint replied HTTP " + std::to_string(status_code);
        return false;
    }

    std::string body;
    char buffer[1024];
    while (true) {
        int bytes_read = http->Read(buffer, sizeof(buffer));
        if (bytes_read < 0) {
            http->Close();
            error_out = "the DoH response was reset";
            return false;
        }
        if (bytes_read == 0) {
            break;
        }
        body.append(buffer, static_cast<size_t>(bytes_read));
        if (body.size() > 16 * 1024) {
            break;
        }
    }
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error_out = "failed to parse the DoH response";
        return false;
    }

    ips_out.clear();
    cJSON* answers = cJSON_GetObjectItem(root, "Answer");
    if (cJSON_IsArray(answers)) {
        for (cJSON* item = answers->child; item != nullptr; item = item->next) {
            cJSON* type = cJSON_GetObjectItem(item, "type");
            cJSON* data = cJSON_GetObjectItem(item, "data");
            // type 1 = A record; CNAMEs (type 5) are walked past.
            if (cJSON_IsNumber(type) && (int)type->valuedouble == 1 && cJSON_IsString(data)) {
                ip_addr_t parsed;
                if (ipaddr_aton(data->valuestring, &parsed) && IP_IS_V4(&parsed)) {
                    std::string ip = data->valuestring;
                    if (std::find(ips_out.begin(), ips_out.end(), ip) == ips_out.end()) {
                        ips_out.push_back(ip);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);

    if (ips_out.empty()) {
        error_out = "the DoH answer contains no A record";
        return false;
    }
    return true;
}

void StoreDohPool(const std::string& host, const std::vector<std::string>& ips) {
    DohPool pool;
    for (const std::string& ip : ips) {
        ip_addr_t parsed;
        if (!ipaddr_aton(ip.c_str(), &parsed) || !IP_IS_V4(&parsed)) {
            continue;
        }
        DohAddr entry{parsed, KeyOf(parsed)};
        bool duplicate = false;
        for (const DohAddr& existing : pool.addrs) {
            if (existing.key == entry.key) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            pool.addrs.push_back(entry);
        }
    }
    if (pool.addrs.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_doh_mutex);
    // A refresh must not reset the cursor of a pool another task is walking
    // through, so only the address list is replaced.
    auto it = g_doh_cache.find(host);
    if (it != g_doh_cache.end() && !it->second.addrs.empty()) {
        pool.next = it->second.next % pool.addrs.size();
    }
    g_doh_cache[host] = std::move(pool);
}

void DohRefreshTask(void*) {
    const TickType_t kSteadyInterval = pdMS_TO_TICKS(10 * 60 * 1000);
    // Wi-Fi is usually up ~8 s after boot, so the first pass often runs before
    // there is a network. Retrying quickly until something resolves keeps the
    // cache from staying empty for a whole ten minute interval.
    const TickType_t kRetryInterval = pdMS_TO_TICKS(15000);

    vTaskDelay(pdMS_TO_TICKS(3000));
    for (;;) {
        int resolved = 0;
        for (const char* host : kDohHosts) {
            std::vector<std::string> ips;
            std::string error;
            if (ResolveViaDoh(host, ips, error)) {
                StoreDohPool(host, ips);
                std::string joined;
                for (const std::string& ip : ips) {
                    joined += (joined.empty() ? "" : ", ") + ip;
                }
                ESP_LOGI("DNS_HOOK", "DoH: %s -> %s", host, joined.c_str());
                resolved++;
            } else {
                // Not fatal: an empty cache simply means the local resolver is
                // used for that name, exactly as before.
                ESP_LOGW("DNS_HOOK", "DoH lookup for %s failed: %s", host, error.c_str());
            }
        }
        vTaskDelay(resolved > 0 ? kSteadyInterval : kRetryInterval);
    }
}

// Serves the next address of the pool that is not currently marked bad. The
// cursor only moves forward, so two lookups in a row get two different
// addresses instead of the same dead one twice.
bool PickDohAddress(const std::string& host, ip_addr_t& out) {
    auto it = g_doh_cache.find(host);
    if (it == g_doh_cache.end() || it->second.addrs.empty()) {
        return false;
    }
    DohPool& pool = it->second;
    uint32_t now = NowSec();

    for (size_t i = 0; i < pool.addrs.size(); ++i) {
        size_t idx = (pool.next + i) % pool.addrs.size();
        auto bad = g_bad_until.find(pool.addrs[idx].key);
        if (bad == g_bad_until.end() || now >= bad->second) {
            pool.next = (idx + 1) % pool.addrs.size();
            out = pool.addrs[idx].addr;
            g_last_served[host] = pool.addrs[idx].key;
            return true;
        }
    }

    // Everything in the pool is currently marked bad. Serving the next address
    // anyway is better than failing the lookup outright: the marks may be
    // stale, and the caller will simply mark it again if it is not.
    size_t idx = pool.next % pool.addrs.size();
    pool.next = (idx + 1) % pool.addrs.size();
    out = pool.addrs[idx].addr;
    g_last_served[host] = pool.addrs[idx].key;
    return true;
}

}  // namespace

// Called by the market fetch when a connection to `host` could not be opened.
// The address the DNS hook served for that host is the one esp-tls tried, and
// esp-tls only ever tries the first address it gets, so the two always refer
// to the same connection.
void DohNoteConnectFailure(const char* host) {
    if (host == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_doh_mutex);
    auto served = g_last_served.find(host);
    if (served == g_last_served.end()) {
        return;
    }
    auto& until = g_bad_until[served->second];
    uint32_t now = NowSec();
    if (until > now) {
        return;  // already marked by an earlier failure
    }
    until = now + kBadAddressSeconds;
    ESP_LOGW("DNS_HOOK", "%s: %s refused a TLS handshake, skipping it for %u min",
             host, KeyToString(served->second).c_str(),
             static_cast<unsigned>(kBadAddressSeconds / 60));
}

void StartDohRefreshTask() {
    // 12 kB of stack: this task runs an HTTPS request and a cJSON parse.
    xTaskCreate(DohRefreshTask, "doh_refresh", 12288, nullptr, 3, nullptr);
}

// Runs the same lookup on demand for the self.net.doh diagnostic tool.
std::string ResolveViaDohTool(const std::string& host) {
    if (host.empty()) {
        return "{\"error\":\"host is required\"}";
    }

    // Report what the device would actually connect to, so the tool shows the
    // live DoH answer and the address the next lookup would be served.
    std::string next_up;
    {
        std::lock_guard<std::mutex> lock(g_doh_mutex);
        ip_addr_t picked;
        if (PickDohAddress(host, picked)) {
            char buf[INET6_ADDRSTRLEN] = {0};
            ipaddr_ntoa_r(&picked, buf, sizeof(buf));
            next_up = buf;
        }
    }

    std::vector<std::string> ips;
    std::string error;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "host", host.c_str());
    cJSON_AddStringToObject(root, "resolver", "cloudflare-dns.com");

    if (ResolveViaDoh(host, ips, error)) {
        StoreDohPool(host, ips);
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON* list = cJSON_AddArrayToObject(root, "ips");
        for (const std::string& ip : ips) {
            cJSON_AddItemToArray(list, cJSON_CreateString(ip.c_str()));
        }
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", error.c_str());
    }
    if (!next_up.empty()) {
        cJSON_AddStringToObject(root, "next_address", next_up.c_str());
    }

    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed ? printed : "{\"error\":\"out of memory\"}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return out;
}

extern "C" int lwip_hook_dns_external_resolve(const char* name, ip_addr_t* addr,
                                               dns_found_callback found, void* callback_arg,
                                               u8_t addrtype, err_t* err) {
    if (name && strlen(name) > 0) {
        uint32_t now = esp_timer_get_time() / 1000000;
        MyBoard::dns_log_.push_back({now, std::string(name)});
        if (MyBoard::dns_log_.size() > MyBoard::MAX_DNS_LOG) {
            MyBoard::dns_log_.erase(MyBoard::dns_log_.begin());
        }
        ESP_LOGI("DNS_HOOK", "Query: %s", name);

        // Hand out a DoH answer when we have one for this name. Returning 1
        // with ERR_OK means "already resolved, *addr is valid", which is the
        // same contract a cache hit in dns.c uses, so getaddrinfo() and the
        // HTTP client pick it up without ever asking the local resolver.
        if (addr != nullptr && addrtype != LWIP_DNS_ADDRTYPE_IPV6) {
            std::lock_guard<std::mutex> lock(g_doh_mutex);
            ip_addr_t picked;
            if (PickDohAddress(name, picked)) {
                ip_addr_copy(*addr, picked);
                char buf[INET6_ADDRSTRLEN] = {0};
                ipaddr_ntoa_r(&picked, buf, sizeof(buf));
                ESP_LOGI("DNS_HOOK", "%s -> %s (from DoH)", name, buf);
                *err = ERR_OK;
                return 1;
            }
        }
    }
    return 0;  // 0 = don't consume, let normal DNS resolution continue
}

DECLARE_BOARD(MyBoard);
