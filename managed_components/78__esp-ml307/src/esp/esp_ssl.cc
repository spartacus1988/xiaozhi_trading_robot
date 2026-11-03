#include "esp_ssl.h"
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static const char *TAG = "EspSsl";

EspSsl::EspSsl() {
    event_group_ = xEventGroupCreate();
}

EspSsl::~EspSsl() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspSsl::Connect(const std::string& host, int port) {
    if (tls_client_ != nullptr) {
        ESP_LOGE(TAG, "tls client has been initialized");
        return false;
    }

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    // Diagnostic: a connect timeout is indistinguishable from a bad address
    // unless we know what DNS returned. esp-tls resolves the host internally
    // through getaddrinfo(), so doing the same lookup here (it hits the cache)
    // shows exactly the addresses it is about to try, including IPv6, which
    // would hang the same way on a network without IPv6 connectivity.
    {
        struct addrinfo hints = {};
        struct addrinfo* res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", port);

        int rc = getaddrinfo(host.c_str(), port_str, &hints, &res);
        if (rc == 0 && res != nullptr) {
            int n = 0;
            for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
                char ip[INET6_ADDRSTRLEN] = {0};
                if (p->ai_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in*)p->ai_addr)->sin_addr,
                              ip, sizeof(ip));
                    ESP_LOGI(TAG, "Resolved %s -> %s (IPv4)", host.c_str(), ip);
                } else if (p->ai_family == AF_INET6) {
                    inet_ntop(AF_INET6, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr,
                              ip, sizeof(ip));
                    ESP_LOGI(TAG, "Resolved %s -> %s (IPv6)", host.c_str(), ip);
                } else {
                    ESP_LOGI(TAG, "Resolved %s -> family %d", host.c_str(), p->ai_family);
                }
                n++;
            }
            freeaddrinfo(res);
            if (n == 0) {
                ESP_LOGW(TAG, "getaddrinfo(%s) returned no addresses", host.c_str());
            }
        } else {
            ESP_LOGW(TAG, "getaddrinfo(%s:%s) failed, rc=%d", host.c_str(), port_str, rc);
        }
    }

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        esp_tls_error_handle_t last_error;
        if (esp_tls_get_error_handle(tls_client_, &last_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t err = esp_tls_get_and_clear_last_error(last_error, &error_code, &error_flags);
            last_error_ = err;
            // error_code carries the real reason: a socket errno such as
            // 110 ETIMEDOUT / 113 EHOSTUNREACH / 101 ENETUNREACH, or a
            // negative mbedtls code. err alone is always 0x8006 here and says
            // nothing about which of the three happened.
            ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x, error_code=%d, flags=0x%x",
                     host.c_str(), port, err, error_code, error_flags);
        } else {
            last_error_ = -1;
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
    xTaskCreate([](void* arg) {
        EspSsl* ssl = (EspSsl*)arg;
        ssl->ReceiveTask();
        xEventGroupSetBits(ssl->event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "ssl_receive", 4096, this, 1, &receive_task_handle_);
    return true;
}

void EspSsl::Disconnect() {
    connected_ = false;
    
    // Close socket if it is open
    if (tls_client_ != nullptr) {
        int sockfd;
        ESP_ERROR_CHECK(esp_tls_get_conn_sockfd(tls_client_, &sockfd));
        if (sockfd >= 0) {
            close(sockfd);
        }
    
        auto bits = xEventGroupWaitBits(event_group_, ESP_SSL_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (!(bits & ESP_SSL_EVENT_RECEIVE_TASK_EXIT)) {
            ESP_LOGE(TAG, "Failed to wait for receive task exit");
        }

        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
}

/* CONFIG_MBEDTLS_SSL_RENEGOTIATION should be disabled in sdkconfig.
 * Otherwise, invalid memory access may be triggered.
 */
int EspSsl::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    
    while (total_sent < data_size) {
        int ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);

        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }

        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }
        
        total_sent += ret;
    }
    
    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());

        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;
        }

        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
            }
            connected_ = false;
            // 接收失败或连接断开时调用断连回调
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }
        
        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspSsl::GetLastError() {
    return last_error_;
}
