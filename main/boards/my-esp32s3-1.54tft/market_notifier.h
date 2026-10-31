#ifndef MARKET_NOTIFIER_H
#define MARKET_NOTIFIER_H

#include <string>

/**
 * @brief Device-side port of the Python bot from
 *        https://github.com/spartacus1988/Gold_and_crypto_notifyer (main.py).
 *
 * The original script ran an endless loop: fetch OHLCV candles from Yahoo
 * Finance, compute technical indicators, reduce them to a score in
 * [-6, +6], let a small risk-managed trading bot act on that score and
 * finally beep. MarketNotifier::Analyze() performs exactly one iteration of
 * that loop, so it can be exposed to the LLM as a single MCP tool.
 *
 * Differences forced by running on an ESP32:
 *  - bot_state.json  -> NVS (one namespace per asset);
 *  - winsound/vibrate -> Application::Alert(), the board has a speaker;
 *  - gold candles are requested at 5m instead of 1m, so that the 5 day
 *    window of the original script fits into ~1440 bars (~100 kB of JSON)
 *    instead of ~7200 bars.
 */
class MarketNotifier {
public:
    /**
     * @brief Fetch fresh candles, compute indicators, run the trading bot
     *        for one cycle and persist its state.
     *
     * @param asset "gold" or "btc" (case-insensitive).
     * @return The MCP tool payload as a JSON string. On failure it is an
     *         object with a single "error" key.
     */
    static std::string Analyze(const std::string& asset);
};

#endif // MARKET_NOTIFIER_H
