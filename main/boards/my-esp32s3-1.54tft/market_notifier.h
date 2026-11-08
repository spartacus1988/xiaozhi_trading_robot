#ifndef MARKET_NOTIFIER_H
#define MARKET_NOTIFIER_H

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * @brief Device-side port of the Python bot from
 *        https://github.com/spartacus1988/Gold_and_crypto_notifyer (main.py).
 *
 * The original script ran an endless loop (`main()`): fetch OHLCV candles from
 * Yahoo Finance, compute technical indicators, reduce them to a score in
 * [-6, +6], let a small risk-managed trading bot act on that score, beep and
 * sleep `INTERVAL` seconds. On the device that loop is split in two halves:
 *
 *  - Analyze() runs exactly one iteration and is exposed to the LLM as the
 *    `self.market.analyze` MCP tool;
 *  - StartMonitor() spawns a FreeRTOS task repeating the same iteration every
 *    `interval_sec` seconds, which is the device-side equivalent of
 *    `while True: analyze(); time.sleep(INTERVAL)`. It is controlled by the
 *    `self.market.monitor` MCP tool and is not restored after a reboot.
 *
 * Differences forced by running on an ESP32:
 *  - bot_state.json  -> NVS (one namespace per asset). The state is still
 *    durable, it just lives in flash instead of next to the script;
 *  - winsound/vibrate -> Application::Alert(), the board has a speaker;
 *  - gold candles are requested at 5m instead of 1m, so that the 5 day
 *    window of the original script fits into ~1440 bars (~100 kB of JSON)
 *    instead of ~7200 bars;
 *  - play_signal() beeps on every iteration, which would be constant noise
 *    once the monitor runs unattended. The board stays silent unless the bot
 *    actually opens or closes a position. A trade made by the background
 *    monitor is then spoken by the assistant: the report (the same JSON a
 *    manual self.market.analyze returns) is queued and injected as a wake
 *    word text once the device is idle, so the LLM tells the user what was
 *    bought or sold and, on an exit, the PnL of the trade.
 */
class MarketNotifier {
public:
    MarketNotifier() = default;
    ~MarketNotifier();

    MarketNotifier(const MarketNotifier&) = delete;
    MarketNotifier& operator=(const MarketNotifier&) = delete;

    /**
     * @brief Fetch fresh candles, compute indicators, run the trading bot
     *        for one cycle and persist its state.
     *
     * @param asset One of "gold", "btc", "eth", "ltc", "doge" or "trump"
     *              (case-insensitive).
     * @return The MCP tool payload as a JSON string. On failure it is an
     *         object with a single "error" key.
     */
    std::string Analyze(const std::string& asset);

    /**
     * @brief Start the background monitor for one asset.
     *
     * The first cycle runs synchronously so the caller immediately gets a
     * full report back; every following cycle runs in the background task.
     *
     * @param asset One of "gold", "btc", "eth", "ltc", "doge" or "trump"
     *              (case-insensitive).
     * @param interval_sec polling period, clamped to [10, 3600] seconds.
     * @return JSON with the monitor state merged into the first report.
     */
    std::string StartMonitor(const std::string& asset, int interval_sec);

    /** @brief Stop the background monitor. Safe to call when it is stopped. */
    std::string StopMonitor();

    /** @brief JSON describing whether the monitor runs and with what settings. */
    std::string MonitorStatus();

private:
    static void MonitorTaskEntry(void* arg);
    void MonitorTaskLoop();
    bool ResolveAsset(const std::string& asset, size_t& index, std::string& error) const;

    std::atomic<bool> monitor_running_{false};
    std::atomic<bool> monitor_task_alive_{false};
    TaskHandle_t monitor_task_handle_ = nullptr;
    size_t monitor_asset_index_ = 0;
    int monitor_interval_sec_ = 60;
};

#endif // MARKET_NOTIFIER_H
