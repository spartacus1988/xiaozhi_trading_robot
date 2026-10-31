// Auto-generated language config
// Language: ru-RU with en-US fallback
#pragma once

#include <string_view>

#ifndef ru_ru
    #define ru_ru  // 預設語言
#endif

namespace Lang {
    // 语言元数据
    constexpr const char* CODE = "ru-RU";

    // 字符串资源 (en-US as fallback for missing keys)
    namespace Strings {
        constexpr const char* ACCESS_VIA_BROWSER = "，доступ через браузер ";
        constexpr const char* ACTIVATION = "Активация устройства";
        constexpr const char* BATTERY_CHARGING = "Зарядка";
        constexpr const char* BATTERY_FULL = "Батарея полная";
        constexpr const char* BATTERY_LOW = "Низкий заряд батареи";
        constexpr const char* BATTERY_NEED_CHARGE = "Низкий заряд, пожалуйста, зарядите";
        constexpr const char* CHECKING_NEW_VERSION = "Проверка новой версии...";
        constexpr const char* CHECK_NEW_VERSION_FAILED = "Ошибка проверки новой версии, повтор через %d секунд: %s";
        constexpr const char* CONNECTED_TO = "Подключено к ";
        constexpr const char* CONNECTING = "Подключение...";
        constexpr const char* CONNECTION_SUCCESSFUL = "Подключение успешно";
        constexpr const char* CONNECT_TO = "Подключение к ";
        constexpr const char* CONNECT_TO_HOTSPOT = "Подключите телефон к точке доступа ";
        constexpr const char* DETECTING_MODULE = "Обнаружение модуля...";
        constexpr const char* DOWNLOAD_ASSETS_FAILED = "Не удалось загрузить ресурсы";
        constexpr const char* ENTERING_WIFI_CONFIG_MODE = "Вход в режим настройки сети...";
        constexpr const char* ERROR = "Ошибка";
        constexpr const char* FLIGHT_MODE_OFF = "Режим полета выключен";
        constexpr const char* FLIGHT_MODE_ON = "Режим полета включен";
        constexpr const char* FOUND_NEW_ASSETS = "Найдены новые ресурсы: %s";
        constexpr const char* HELLO_MY_FRIEND = "Привет, мой друг!";
        constexpr const char* INFO = "Информация";
        constexpr const char* INITIALIZING = "Инициализация...";
        constexpr const char* LISTENING = "Прослушивание...";
        constexpr const char* LOADING_ASSETS = "Загрузка ресурсов...";
        constexpr const char* LOADING_PROTOCOL = "Подключение к серверу...";
        constexpr const char* MAX_VOLUME = "Максимальная громкость";
        constexpr const char* MODEM_INIT_ERROR = "Ошибка инициализации модема";
        constexpr const char* MUTED = "Звук отключен";
        constexpr const char* NEW_VERSION = "Новая версия ";
        constexpr const char* OTA_UPGRADE = "Обновление OTA";
        constexpr const char* PIN_ERROR = "Пожалуйста, вставьте SIM-карту";
        constexpr const char* PLEASE_WAIT = "Пожалуйста, подождите...";
        constexpr const char* REGISTERING_NETWORK = "Ожидание сети...";
        constexpr const char* REG_ERROR = "Невозможно подключиться к сети, проверьте состояние карты данных";
        constexpr const char* RTC_MODE_OFF = "AEC выключен";
        constexpr const char* RTC_MODE_ON = "AEC включен";
        constexpr const char* SCANNING_WIFI = "Сканирование Wi-Fi...";
        constexpr const char* SERVER_ERROR = "Ошибка отправки, проверьте сеть";
        constexpr const char* SERVER_NOT_CONNECTED = "Невозможно подключиться к сервису, попробуйте позже";
        constexpr const char* SERVER_NOT_FOUND = "Поиск доступного сервиса";
        constexpr const char* SERVER_TIMEOUT = "Тайм-аут ответа";
        constexpr const char* SPEAKING = "Говорение...";
        constexpr const char* STANDBY = "Ожидание";
        constexpr const char* SWITCH_TO_4G_NETWORK = "Переключение на 4G...";
        constexpr const char* SWITCH_TO_WIFI_NETWORK = "Переключение на Wi-Fi...";
        constexpr const char* UPGRADE_FAILED = "Обновление не удалось";
        constexpr const char* UPGRADING = "Обновление системы...";
        constexpr const char* VERSION = "Версия ";
        constexpr const char* VOLUME = "Громкость ";
        constexpr const char* WARNING = "Предупреждение";
        constexpr const char* WIFI_CONFIG_MODE = "Режим настройки сети";
    }

    // 音效资源 (en-US as fallback for missing audio files)
    namespace Sounds {

        extern const char ogg_0_start[] asm("_binary_0_ogg_start");
        extern const char ogg_0_end[] asm("_binary_0_ogg_end");
        static const std::string_view OGG_0 {
        static_cast<const char*>(ogg_0_start),
        static_cast<size_t>(ogg_0_end - ogg_0_start)
        };

        extern const char ogg_1_start[] asm("_binary_1_ogg_start");
        extern const char ogg_1_end[] asm("_binary_1_ogg_end");
        static const std::string_view OGG_1 {
        static_cast<const char*>(ogg_1_start),
        static_cast<size_t>(ogg_1_end - ogg_1_start)
        };

        extern const char ogg_2_start[] asm("_binary_2_ogg_start");
        extern const char ogg_2_end[] asm("_binary_2_ogg_end");
        static const std::string_view OGG_2 {
        static_cast<const char*>(ogg_2_start),
        static_cast<size_t>(ogg_2_end - ogg_2_start)
        };

        extern const char ogg_3_start[] asm("_binary_3_ogg_start");
        extern const char ogg_3_end[] asm("_binary_3_ogg_end");
        static const std::string_view OGG_3 {
        static_cast<const char*>(ogg_3_start),
        static_cast<size_t>(ogg_3_end - ogg_3_start)
        };

        extern const char ogg_4_start[] asm("_binary_4_ogg_start");
        extern const char ogg_4_end[] asm("_binary_4_ogg_end");
        static const std::string_view OGG_4 {
        static_cast<const char*>(ogg_4_start),
        static_cast<size_t>(ogg_4_end - ogg_4_start)
        };

        extern const char ogg_5_start[] asm("_binary_5_ogg_start");
        extern const char ogg_5_end[] asm("_binary_5_ogg_end");
        static const std::string_view OGG_5 {
        static_cast<const char*>(ogg_5_start),
        static_cast<size_t>(ogg_5_end - ogg_5_start)
        };

        extern const char ogg_6_start[] asm("_binary_6_ogg_start");
        extern const char ogg_6_end[] asm("_binary_6_ogg_end");
        static const std::string_view OGG_6 {
        static_cast<const char*>(ogg_6_start),
        static_cast<size_t>(ogg_6_end - ogg_6_start)
        };

        extern const char ogg_7_start[] asm("_binary_7_ogg_start");
        extern const char ogg_7_end[] asm("_binary_7_ogg_end");
        static const std::string_view OGG_7 {
        static_cast<const char*>(ogg_7_start),
        static_cast<size_t>(ogg_7_end - ogg_7_start)
        };

        extern const char ogg_8_start[] asm("_binary_8_ogg_start");
        extern const char ogg_8_end[] asm("_binary_8_ogg_end");
        static const std::string_view OGG_8 {
        static_cast<const char*>(ogg_8_start),
        static_cast<size_t>(ogg_8_end - ogg_8_start)
        };

        extern const char ogg_9_start[] asm("_binary_9_ogg_start");
        extern const char ogg_9_end[] asm("_binary_9_ogg_end");
        static const std::string_view OGG_9 {
        static_cast<const char*>(ogg_9_start),
        static_cast<size_t>(ogg_9_end - ogg_9_start)
        };

        extern const char ogg_activation_start[] asm("_binary_activation_ogg_start");
        extern const char ogg_activation_end[] asm("_binary_activation_ogg_end");
        static const std::string_view OGG_ACTIVATION {
        static_cast<const char*>(ogg_activation_start),
        static_cast<size_t>(ogg_activation_end - ogg_activation_start)
        };

        extern const char ogg_err_pin_start[] asm("_binary_err_pin_ogg_start");
        extern const char ogg_err_pin_end[] asm("_binary_err_pin_ogg_end");
        static const std::string_view OGG_ERR_PIN {
        static_cast<const char*>(ogg_err_pin_start),
        static_cast<size_t>(ogg_err_pin_end - ogg_err_pin_start)
        };

        extern const char ogg_err_reg_start[] asm("_binary_err_reg_ogg_start");
        extern const char ogg_err_reg_end[] asm("_binary_err_reg_ogg_end");
        static const std::string_view OGG_ERR_REG {
        static_cast<const char*>(ogg_err_reg_start),
        static_cast<size_t>(ogg_err_reg_end - ogg_err_reg_start)
        };

        extern const char ogg_exclamation_start[] asm("_binary_exclamation_ogg_start");
        extern const char ogg_exclamation_end[] asm("_binary_exclamation_ogg_end");
        static const std::string_view OGG_EXCLAMATION {
        static_cast<const char*>(ogg_exclamation_start),
        static_cast<size_t>(ogg_exclamation_end - ogg_exclamation_start)
        };

        extern const char ogg_low_battery_start[] asm("_binary_low_battery_ogg_start");
        extern const char ogg_low_battery_end[] asm("_binary_low_battery_ogg_end");
        static const std::string_view OGG_LOW_BATTERY {
        static_cast<const char*>(ogg_low_battery_start),
        static_cast<size_t>(ogg_low_battery_end - ogg_low_battery_start)
        };

        extern const char ogg_popup_start[] asm("_binary_popup_ogg_start");
        extern const char ogg_popup_end[] asm("_binary_popup_ogg_end");
        static const std::string_view OGG_POPUP {
        static_cast<const char*>(ogg_popup_start),
        static_cast<size_t>(ogg_popup_end - ogg_popup_start)
        };

        extern const char ogg_success_start[] asm("_binary_success_ogg_start");
        extern const char ogg_success_end[] asm("_binary_success_ogg_end");
        static const std::string_view OGG_SUCCESS {
        static_cast<const char*>(ogg_success_start),
        static_cast<size_t>(ogg_success_end - ogg_success_start)
        };

        extern const char ogg_upgrade_start[] asm("_binary_upgrade_ogg_start");
        extern const char ogg_upgrade_end[] asm("_binary_upgrade_ogg_end");
        static const std::string_view OGG_UPGRADE {
        static_cast<const char*>(ogg_upgrade_start),
        static_cast<size_t>(ogg_upgrade_end - ogg_upgrade_start)
        };

        extern const char ogg_vibration_start[] asm("_binary_vibration_ogg_start");
        extern const char ogg_vibration_end[] asm("_binary_vibration_ogg_end");
        static const std::string_view OGG_VIBRATION {
        static_cast<const char*>(ogg_vibration_start),
        static_cast<size_t>(ogg_vibration_end - ogg_vibration_start)
        };

        extern const char ogg_welcome_start[] asm("_binary_welcome_ogg_start");
        extern const char ogg_welcome_end[] asm("_binary_welcome_ogg_end");
        static const std::string_view OGG_WELCOME {
        static_cast<const char*>(ogg_welcome_start),
        static_cast<size_t>(ogg_welcome_end - ogg_welcome_start)
        };

        extern const char ogg_wificonfig_start[] asm("_binary_wificonfig_ogg_start");
        extern const char ogg_wificonfig_end[] asm("_binary_wificonfig_ogg_end");
        static const std::string_view OGG_WIFICONFIG {
        static_cast<const char*>(ogg_wificonfig_start),
        static_cast<size_t>(ogg_wificonfig_end - ogg_wificonfig_start)
        };
    }
}
