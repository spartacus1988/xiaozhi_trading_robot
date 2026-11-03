/*
 * market_notifier.cc
 *
 * C++ port of https://github.com/spartacus1988/Gold_and_crypto_notifyer (main.py)
 *
 * Structure mirrors the original file section by section:
 *   1. bot state      (was bot_state.json -> now NVS)
 *   2. data loading   (Yahoo Finance chart API over HTTPS)
 *   3. indicators     (EMA, RSI, MACD, ATR, ADX, Bollinger)
 *   4. signal         (play_signal -> Application::Alert, trades only)
 *   5. bot logic      (stop-loss / take-profit / time exit / signal exit)
 *   6. analysis       (score + verdict)
 *   7. monitor loop   (main() -> FreeRTOS task + self.market.monitor tool)
 */

#include "market_notifier.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <time.h>
#include <nvs.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"

#define TAG "MarketNotifier"

// Defined in my_board.cc: reports which DNS-over-HTTPS address was served for
// `host` right before its TLS handshake failed, so the cache can stop using
// it. Kept as a plain extern because the DoH cache lives with the board's DNS
// hook, not with the market code.
extern void DohNoteConnectFailure(const char* host);
extern bool DohHostIsUsable(const char* host);

namespace {

// A cycle called from self.market.analyze and one called by the monitor task
// can overlap in time; the NVS bot state must never be read-modify-written by
// two cycles at once, so every cycle takes this lock.
std::mutex g_cycle_mutex;

// ---------- Settings (from the "НАСТРОЙКИ" block of main.py) ----------
struct AssetConfig {
    const char* key;     // name used by the tool argument
    const char* ticker;  // Yahoo symbol
    const char* name;    // human readable name
    const char* range;   // Yahoo range
    const char* interval;// Yahoo interval
    int decimals;
};

// The original script used range=5d/interval=1m for gold. On the device that
// is ~7200 bars and a multi-megabyte cJSON tree, so gold is requested with
// 5 minute candles over the same 5 day window (~1440 bars).
const AssetConfig kAssets[] = {
    {"gold", "GC=F",    "Золото (фьючерс)", "5d", "5m", 2},
    {"btc",  "BTC-USD", "Bitcoin",          "1d", "1m", 2},
};
constexpr size_t kAssetCount = sizeof(kAssets) / sizeof(kAssets[0]);

constexpr int    kRsiPeriod   = 14;
constexpr int    kAtrPeriod   = 14;
constexpr int    kAdxPeriod   = 14;
constexpr int    kBbPeriod    = 20;
constexpr double kBbMult      = 2.0;
constexpr int    kVolAvgPeriod = 20;
constexpr int    kEmaFast     = 9;
constexpr int    kEmaSlow     = 21;
constexpr int    kMacdFast    = 12;
constexpr int    kMacdSlow    = 26;
constexpr int    kMacdSignal  = 9;
constexpr const char* kRsiMode = "reversion";  // "reversion" | "trend"

// ---------- Bot (from the "БОТ" block of main.py) ----------
constexpr double kInitialBalance   = 100.0;
constexpr double kStopLossPct      = 2.0;
constexpr double kTakeProfitPct    = 5.0;
constexpr int    kMaxHoldTimeMin   = 60;

struct BotState {
    double usd          = kInitialBalance;
    double asset        = 0.0;
    double entry        = 0.0;
    double entry_time   = 0.0;
    double base_volume  = 0.0;
};

// ---------- 1. Bot state ----------

std::string StateNamespace(const AssetConfig& cfg) {
    return std::string("mkt_") + cfg.key;
}

BotState LoadState(const AssetConfig& cfg) {
    BotState state;

    nvs_handle_t handle;
    if (nvs_open(StateNamespace(cfg).c_str(), NVS_READONLY, &handle) != ESP_OK) {
        return state;  // first run: defaults from BotState
    }

    char buf[256] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(handle, "state", buf, &len) == ESP_OK) {
        cJSON* root = cJSON_Parse(buf);
        if (root != nullptr) {
            cJSON* item;
            if ((item = cJSON_GetObjectItem(root, "usd")) && cJSON_IsNumber(item)) state.usd = item->valuedouble;
            if ((item = cJSON_GetObjectItem(root, "asset")) && cJSON_IsNumber(item)) state.asset = item->valuedouble;
            if ((item = cJSON_GetObjectItem(root, "entry")) && cJSON_IsNumber(item)) state.entry = item->valuedouble;
            if ((item = cJSON_GetObjectItem(root, "entry_time")) && cJSON_IsNumber(item)) state.entry_time = item->valuedouble;
            if ((item = cJSON_GetObjectItem(root, "base_volume")) && cJSON_IsNumber(item)) state.base_volume = item->valuedouble;
            cJSON_Delete(root);
        }
    }
    nvs_close(handle);
    return state;
}

void SaveState(const AssetConfig& cfg, const BotState& state) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "usd", state.usd);
    cJSON_AddNumberToObject(root, "asset", state.asset);
    cJSON_AddNumberToObject(root, "entry", state.entry);
    cJSON_AddNumberToObject(root, "entry_time", state.entry_time);
    cJSON_AddNumberToObject(root, "base_volume", state.base_volume);
    char* json_str = cJSON_PrintUnformatted(root);
    std::string payload(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    nvs_handle_t handle;
    if (nvs_open(StateNamespace(cfg).c_str(), NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for bot state");
        return;
    }
    nvs_set_str(handle, "state", payload.c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

// ---------- 2. Data loading ----------

// Yahoo serves the identical chart API from two hostnames that resolve to
// different edge IPs. On some networks only one of them answers: DNS is fine
// for both, but the TCP handshake to the other one is silently dropped and
// esp-tls gives up after its connect timeout. Since the whole fetch dies on
// the very first Open(), a failed open is retried on the second host instead
// of being reported as an outage right away.
const char* kYahooHosts[] = {
    "query1.finance.yahoo.com",
    "query2.finance.yahoo.com",
};
constexpr size_t kYahooHostCount = sizeof(kYahooHosts) / sizeof(kYahooHosts[0]);

bool FetchOhlcv(const AssetConfig& cfg,
                std::vector<double>& highs,
                std::vector<double>& lows,
                std::vector<double>& closes,
                std::vector<double>& volumes,
                std::string& error) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "network is not available";
        return false;
    }

    std::string body;
    bool opened = false;

    for (size_t attempt = 0; attempt < kYahooHostCount; ++attempt) {
        const char* host = kYahooHosts[attempt];

        // A host whose whole address pool is already known not to answer TLS
        // would only burn the connect timeout; the fallback host gets a chance
        // instead and the pool is refreshed in the background anyway.
        if (!DohHostIsUsable(host)) {
            ESP_LOGW(TAG, "Skipping %s: every address it resolves to is marked bad", host);
            continue;
        }

        const std::string url = std::string("https://") + host + "/v8/finance/chart/" +
                                cfg.ticker + "?interval=" + cfg.interval + "&range=" + cfg.range;

        auto http = network->CreateHttp(0);
        // NOTE: with this client the timeout is a per-read wait, not a whole-body
        // budget, so 30 s of silence between two chunks is already generous.
        http->SetTimeout(30000);
        http->SetHeader("User-Agent", "Mozilla/5.0");

        if (!http->Open("GET", url)) {
            // The address the DNS hook served for this host is the one
            // esp-tls tried and gave up on; telling the DoH cache about it
            // stops it from being offered again for the next half hour.
            DohNoteConnectFailure(host);
            ESP_LOGW(TAG, "Open failed on %s, %s", host,
                     attempt + 1 < kYahooHostCount ? "trying the fallback host"
                                                   : "no hosts left");
            continue;
        }
        opened = true;

        int status_code = http->GetStatusCode();
        if (status_code == 429) {
            http->Close();
            error = "Yahoo Finance rate limit (HTTP 429), try again later";
            return false;
        }
        if (status_code != 200) {
            http->Close();
            error = "Yahoo Finance replied with HTTP " + std::to_string(status_code);
            return false;
        }

        // Read the body in chunks instead of ReadAll().
        //
        // Http::ReadAll() waits for eof_ without ever draining body_chunks_,
        // while the receive path blocks itself once 8 kB are buffered there
        // (MAX_BODY_CHUNKS_SIZE in http_client.cc) and is only released by a
        // Read() or a Close(). Any response larger than 8 kB therefore
        // deadlocks and ReadAll() times out and returns an empty string, which
        // then looks like a JSON parse error. Both Yahoo payloads are way
        // above that limit (BTC ~35 kB, gold ~80 kB), so streaming them is
        // mandatory.
        char buffer[1536];
        while (true) {
            int bytes_read = http->Read(buffer, sizeof(buffer));
            if (bytes_read < 0) {
                http->Close();
                error = "connection to Yahoo Finance was reset while downloading";
                return false;
            }
            if (bytes_read == 0) {
                break;  // end of body
            }
            body.append(buffer, static_cast<size_t>(bytes_read));
        }
        http->Close();

        ESP_LOGI(TAG, "Downloaded %u bytes from %s for %s",
                 static_cast<unsigned>(body.size()), host, cfg.ticker);
        break;  // complete body received, no need for the fallback host
    }

    if (!opened) {
        error = "failed to open a connection to Yahoo Finance";
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "failed to parse the Yahoo Finance response";
        return false;
    }

    cJSON* results = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "chart"), "result");
    cJSON* first = cJSON_GetArrayItem(results, 0);
    if (first == nullptr) {
        cJSON_Delete(root);
        error = "no data returned for ticker " + std::string(cfg.ticker);
        return false;
    }

    cJSON* quote = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(first, "indicators"), "quote"), 0);
    cJSON* h_arr = quote ? cJSON_GetObjectItem(quote, "high") : nullptr;
    cJSON* l_arr = quote ? cJSON_GetObjectItem(quote, "low") : nullptr;
    cJSON* c_arr = quote ? cJSON_GetObjectItem(quote, "close") : nullptr;
    cJSON* v_arr = quote ? cJSON_GetObjectItem(quote, "volume") : nullptr;

    if (h_arr == nullptr || l_arr == nullptr || c_arr == nullptr || v_arr == nullptr) {
        cJSON_Delete(root);
        error = "incomplete candle data for ticker " + std::string(cfg.ticker);
        return false;
    }

    int n = cJSON_GetArraySize(h_arr);
    for (int i = 0; i < n; i++) {
        cJSON* h = cJSON_GetArrayItem(h_arr, i);
        cJSON* l = cJSON_GetArrayItem(l_arr, i);
        cJSON* c = cJSON_GetArrayItem(c_arr, i);
        cJSON* v = cJSON_GetArrayItem(v_arr, i);

        double volume = (v != nullptr && cJSON_IsNumber(v)) ? v->valuedouble : 0.0;
        if (h == nullptr || l == nullptr || c == nullptr) continue;
        if (!cJSON_IsNumber(h) || !cJSON_IsNumber(l) || !cJSON_IsNumber(c)) continue;

        highs.push_back(h->valuedouble);
        lows.push_back(l->valuedouble);
        closes.push_back(c->valuedouble);
        volumes.push_back(volume);
    }

    cJSON_Delete(root);

    if (closes.empty()) {
        error = "no usable candles for ticker " + std::string(cfg.ticker);
        return false;
    }
    return true;
}

// ---------- 3. Indicators ----------

std::vector<double> EmaSeries(const std::vector<double>& values, int period) {
    std::vector<double> out;
    if ((int)values.size() < period) return out;

    double k = 2.0 / (period + 1);
    double seed = 0.0;
    for (int i = 0; i < period; i++) seed += values[i];
    seed /= period;
    out.push_back(seed);

    for (size_t i = period; i < values.size(); i++) {
        out.push_back(values[i] * k + out.back() * (1.0 - k));
    }
    return out;
}

std::optional<double> Rsi(const std::vector<double>& values, int period) {
    if ((int)values.size() < period + 1) return std::nullopt;

    std::vector<double> gains, losses;
    gains.reserve(values.size() - 1);
    losses.reserve(values.size() - 1);
    for (size_t i = 1; i < values.size(); i++) {
        double diff = values[i] - values[i - 1];
        gains.push_back(std::max(diff, 0.0));
        losses.push_back(std::max(-diff, 0.0));
    }

    double avg_gain = 0.0, avg_loss = 0.0;
    for (int i = 0; i < period; i++) {
        avg_gain += gains[i];
        avg_loss += losses[i];
    }
    avg_gain /= period;
    avg_loss /= period;

    for (size_t i = period; i < gains.size(); i++) {
        avg_gain = (avg_gain * (period - 1) + gains[i]) / period;
        avg_loss = (avg_loss * (period - 1) + losses[i]) / period;
    }

    if (avg_loss == 0.0 && avg_gain == 0.0) return 50.0;
    if (avg_loss == 0.0) return 100.0;
    double rs = avg_gain / avg_loss;
    return 100.0 - 100.0 / (1.0 + rs);
}

struct MacdValue {
    bool valid = false;
    double macd = 0.0;
    double signal = 0.0;
    double hist = 0.0;
    double hist_prev = 0.0;
};

MacdValue Macd(const std::vector<double>& values, int fast, int slow, int signal) {
    MacdValue result;
    if ((int)values.size() < slow + signal) return result;

    auto ema_fast = EmaSeries(values, fast);
    auto ema_slow = EmaSeries(values, slow);

    size_t offset = slow - fast;
    std::vector<double> line;
    line.reserve(ema_slow.size());
    for (size_t i = 0; i < ema_slow.size(); i++) {
        line.push_back(ema_fast[i + offset] - ema_slow[i]);
    }

    auto signal_line = EmaSeries(line, signal);
    if (signal_line.empty()) return result;

    result.macd = line.back();
    result.signal = signal_line.back();
    result.hist = result.macd - result.signal;
    result.hist_prev = signal_line.size() > 1
        ? line[line.size() - 2] - signal_line[signal_line.size() - 2]
        : result.hist;
    result.valid = true;
    return result;
}

std::optional<double> Atr(const std::vector<double>& highs,
                          const std::vector<double>& lows,
                          const std::vector<double>& closes,
                          int period) {
    size_t n = std::min({highs.size(), lows.size(), closes.size()});
    if (n < (size_t)period + 1) return std::nullopt;

    std::vector<double> trs;
    trs.reserve(n - 1);
    for (size_t i = 1; i < n; i++) {
        trs.push_back(std::max({highs[i] - lows[i],
                                std::fabs(highs[i] - closes[i - 1]),
                                std::fabs(lows[i] - closes[i - 1])}));
    }
    if ((int)trs.size() < period) return std::nullopt;

    double a = 0.0;
    for (int i = 0; i < period; i++) a += trs[i];
    a /= period;
    for (size_t i = period; i < trs.size(); i++) {
        a = (a * (period - 1) + trs[i]) / period;
    }
    return a;
}

struct AdxValue {
    bool valid = false;
    double adx = 0.0;
    double plus_di = 0.0;
    double minus_di = 0.0;
};

AdxValue Adx(const std::vector<double>& highs,
             const std::vector<double>& lows,
             const std::vector<double>& closes,
             int period) {
    AdxValue result;
    size_t n = std::min({highs.size(), lows.size(), closes.size()});
    if (n < (size_t)period * 2) return result;

    std::vector<double> trs, plus_dms, minus_dms;
    trs.reserve(n - 1);
    plus_dms.reserve(n - 1);
    minus_dms.reserve(n - 1);
    for (size_t i = 1; i < n; i++) {
        trs.push_back(std::max({highs[i] - lows[i],
                                std::fabs(highs[i] - closes[i - 1]),
                                std::fabs(lows[i] - closes[i - 1])}));
        double up_move = highs[i] - highs[i - 1];
        double down_move = lows[i - 1] - lows[i];
        plus_dms.push_back((up_move > down_move && up_move > 0) ? up_move : 0.0);
        minus_dms.push_back((down_move > up_move && down_move > 0) ? down_move : 0.0);
    }

    auto smoothed = [period](const std::vector<double>& source) {
        std::vector<double> out;
        double sum = 0.0;
        for (int i = 0; i < period; i++) sum += source[i];
        out.push_back(sum);
        for (size_t i = period; i < source.size(); i++) {
            out.push_back(out.back() - (out.back() / period) + source[i]);
        }
        return out;
    };

    auto smoothed_tr = smoothed(trs);
    auto smoothed_plus = smoothed(plus_dms);
    auto smoothed_minus = smoothed(minus_dms);

    std::vector<double> plus_di, minus_di, dx;
    for (size_t i = 0; i < smoothed_tr.size(); i++) {
        double t = smoothed_tr[i];
        double p = t != 0.0 ? 100.0 * smoothed_plus[i] / t : 0.0;
        double m = t != 0.0 ? 100.0 * smoothed_minus[i] / t : 0.0;
        plus_di.push_back(p);
        minus_di.push_back(m);
        dx.push_back((p + m) != 0.0 ? 100.0 * std::fabs(p - m) / (p + m) : 0.0);
    }

    if ((int)dx.size() < period) return result;

    double adx_val = 0.0;
    for (int i = 0; i < period; i++) adx_val += dx[i];
    adx_val /= period;
    for (size_t i = period; i < dx.size(); i++) {
        adx_val = (adx_val * (period - 1) + dx[i]) / period;
    }

    result.valid = true;
    result.adx = adx_val;
    result.plus_di = plus_di.back();
    result.minus_di = minus_di.back();
    return result;
}

struct BollingerValue {
    bool valid = false;
    double upper = 0.0;
    double mid = 0.0;
    double lower = 0.0;
};

BollingerValue Bollinger(const std::vector<double>& values, int period, double mult) {
    BollingerValue result;
    if ((int)values.size() < period) return result;

    double sma = 0.0;
    for (int i = 0; i < period; i++) sma += values[values.size() - period + i];
    sma /= period;

    double variance = 0.0;
    for (int i = 0; i < period; i++) {
        double x = values[values.size() - period + i];
        variance += (x - sma) * (x - sma);
    }
    variance /= period;
    double std_dev = std::sqrt(variance);

    result.valid = true;
    result.upper = sma + std_dev * mult;
    result.mid = sma;
    result.lower = sma - std_dev * mult;
    return result;
}

// ---------- 4. Signal ----------

// The Python script plays a beep on *every* iteration, scaled by |score|
// (play_signal). That was fine for a console script, but the device now runs
// the same loop unattended every minute, so a beep per iteration would be
// constant noise. Per the requested behaviour the board stays silent unless
// the trading bot actually opens or closes a position: BUY, STRONG_BUY,
// STOP_LOSS, TAKE_PROFIT, SELL and TIME_EXIT announce themselves, while HOLD
// and WAIT announce nothing at all.
//
// The message is shown on the display and written to the log. Speech is not
// available here: the device has no local TTS, everything spoken by the
// assistant comes from the server over MQTT.
void NotifyTrade(const AssetConfig* cfg, const char* action, int score,
                 double price, double pnl_pct, double pnl_usd, double hold_min,
                 const BotState& state) {
    const bool is_entry = strcmp(action, "BUY") == 0 || strcmp(action, "STRONG_BUY") == 0;
    const bool is_exit  = strcmp(action, "STOP_LOSS") == 0 || strcmp(action, "TAKE_PROFIT") == 0 ||
                          strcmp(action, "SELL") == 0 || strcmp(action, "TIME_EXIT") == 0;
    if (!is_entry && !is_exit) {
        return;  // HOLD or WAIT: no sound, no display update
    }

    char message[160];
    if (is_entry) {
        snprintf(message, sizeof(message),
                 "%s: position opened, %.8f at $%.*f (score %+d)",
                 cfg->name, state.asset, cfg->decimals, price, score);
    } else if (strcmp(action, "STOP_LOSS") == 0) {
        snprintf(message, sizeof(message),
                 "Stop loss: %.2f%%, result %+.2f USD", pnl_pct, pnl_usd);
    } else if (strcmp(action, "TAKE_PROFIT") == 0) {
        snprintf(message, sizeof(message),
                 "Take profit: +%.2f%%, result %+.2f USD", pnl_pct, pnl_usd);
    } else if (strcmp(action, "SELL") == 0) {
        snprintf(message, sizeof(message),
                 "Sold on signal (score %+d), result %+.2f USD", score, pnl_usd);
    } else {
        snprintf(message, sizeof(message),
                 "Exit by time after %.0f min, result %+.2f USD", hold_min, pnl_usd);
    }

    auto& app = Application::GetInstance();
    if (is_entry) {
        app.Alert("Market", message, "happy", Lang::Sounds::OGG_SUCCESS);
    } else {
        app.Alert("Market", message, "angry", Lang::Sounds::OGG_EXCLAMATION);
    }
}

// ---------- 5. Bot logic ----------

// The Python bot uses time.time() (wall clock). The device has real
// wall-clock time after the boot-time OTA check syncs it (see ota.cc,
// settimeofday), so the same source is used here; entry times stored in NVS
// remain valid across reboots.
// pnl_pct, pnl_usd and hold_min are reported through out parameters so that
// NotifyTrade() can announce them after the state has already been closed.
const char* BotAction(double price, int score, BotState& state,
                      double& pnl_pct, double& pnl_usd, double& hold_time_min) {
    const double now = (double)time(nullptr);
    const char* action = "WAIT";

    pnl_pct = 0.0;
    pnl_usd = 0.0;
    hold_time_min = 0.0;

    if (state.asset > 0.0 && state.entry > 0.0) {
        // We are in a position.
        pnl_pct = ((price - state.entry) / state.entry) * 100.0;
        hold_time_min = (now - state.entry_time) / 60.0;
        pnl_usd = (price - state.entry) * state.asset;

        if (pnl_pct <= -kStopLossPct) {
            action = "STOP_LOSS";
            ESP_LOGW(TAG, "STOP LOSS! Drawdown %.2f%% (PnL: %+.2f USD)", pnl_pct, pnl_usd);
        } else if (pnl_pct >= kTakeProfitPct) {
            action = "TAKE_PROFIT";
            ESP_LOGW(TAG, "TAKE PROFIT! Profit %.2f%% (PnL: %+.2f USD)", pnl_pct, pnl_usd);
        } else if (score <= -2) {
            action = "SELL";
            ESP_LOGW(TAG, "SELL ON SIGNAL (Score: %d, PnL: %+.2f USD)", score, pnl_usd);
        } else if (hold_time_min > kMaxHoldTimeMin && pnl_pct < 1.0) {
            action = "TIME_EXIT";
            ESP_LOGW(TAG, "TIME EXIT (held %.0f min, PnL: %+.2f USD)", hold_time_min, pnl_usd);
        } else {
            action = "HOLD";
        }

        if (strcmp(action, "STOP_LOSS") == 0 || strcmp(action, "TAKE_PROFIT") == 0 ||
            strcmp(action, "SELL") == 0 || strcmp(action, "TIME_EXIT") == 0) {
            state.usd = state.asset * price;
            state.asset = 0.0;
            state.entry = 0.0;
            state.entry_time = 0.0;
        }
    } else {
        // We are in cash and looking for an entry.
        if (score >= 4) {
            action = "STRONG_BUY";
        } else if (score >= 2) {
            action = "BUY";
        } else {
            action = "WAIT";
        }

        if (strcmp(action, "STRONG_BUY") == 0 || strcmp(action, "BUY") == 0) {
            state.asset = state.usd / price;
            state.usd = 0.0;
            state.entry = price;
            state.entry_time = now;
        }
    }

    return action;
}

std::string BotMessage(const char* action, int score, double price, const BotState& state) {
    char buf[96];
    double equity = state.usd + state.asset * price;

    if (strcmp(action, "STRONG_BUY") == 0) {
        snprintf(buf, sizeof(buf), "STRONG BUY (Score: %+d)", score);
        return buf;
    }
    if (strcmp(action, "BUY") == 0) {
        snprintf(buf, sizeof(buf), "BUY (Score: %+d)", score);
        return buf;
    }
    if (strcmp(action, "SELL") == 0) {
        snprintf(buf, sizeof(buf), "SELL ON SIGNAL (Score: %+d)", score);
        return buf;
    }
    if (strcmp(action, "STOP_LOSS") == 0) return "STOP LOSS";
    if (strcmp(action, "TAKE_PROFIT") == 0) return "TAKE PROFIT";
    if (strcmp(action, "TIME_EXIT") == 0) return "EXIT BY TIME";
    if (strcmp(action, "HOLD") == 0) {
        snprintf(buf, sizeof(buf), "HOLDING THE POSITION, equity $%.2f", equity);
        return buf;
    }
    snprintf(buf, sizeof(buf), "WAITING FOR A SIGNAL, cash $%.2f", state.usd);
    return buf;
}

// ---------- 6. Analysis ----------

const char* RsiTag(double r) {
    if (r < 30.0) return "oversold, a bounce is expected";
    if (r > 70.0) return "overbought, correction risk";
    if (r > 50.0) return "bullish sentiment";
    return "bearish sentiment";
}

const char* AdxTag(double v) {
    if (v < 20.0) return "FLAT";
    if (v < 25.0) return "trend forming";
    return "STRONG TREND";
}

const char* AtrTag(double pct) {
    if (pct < 0.15) return "low";
    if (pct < 0.3) return "moderate";
    return "high";
}

const char* Verdict(int score) {
    if (score >= 4) return "STRONG BULLISH SIGNAL";
    if (score >= 2) return "moderately bullish";
    if (score == 1) return "weakly bullish";
    if (score == 0) return "neutral";
    if (score == -1) return "weakly bearish";
    if (score >= -3) return "moderately bearish";
    return "STRONG BEARISH SIGNAL";
}

std::string ErrorJson(const std::string& message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message.c_str());
    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return result;
}

}  // namespace

namespace {

// One full iteration of the Python loop: download candles, score them, let the
// trading bot act on that score, persist the new state, announce a real trade
// and build the report. Both self.market.analyze and the monitor task run this
// same function, so the two paths can never drift apart.
bool RunCycle(const AssetConfig* cfg, std::string& report, std::string& error) {
    // The tool and the monitor task can overlap in time; the NVS state is a
    // read-modify-write and must not be interleaved.
    std::lock_guard<std::mutex> cycle_lock(g_cycle_mutex);

    std::vector<double> highs, lows, closes, volumes;
    std::string fetch_error;
    if (!FetchOhlcv(*cfg, highs, lows, closes, volumes, fetch_error)) {
        ESP_LOGE(TAG, "Fetch failed: %s", fetch_error.c_str());
        error = fetch_error;
        return false;
    }

    // The Python script quits the cycle when there are not enough candles for
    // MACD (MACD_SLOW + MACD_SIGNAL bars).
    if (closes.size() < (size_t)(kMacdSlow + kMacdSignal)) {
        ESP_LOGW(TAG, "Only %d candles for %s, not enough for the analysis",
                 (int)closes.size(), cfg->ticker);
        error = "not enough candles for ticker " + std::string(cfg->ticker);
        return false;
    }

    BotState state = LoadState(*cfg);

    // The volume of the very first bar is the baseline for the relative volume.
    if (state.base_volume == 0.0 && !volumes.empty()) {
        state.base_volume = volumes.back();
        SaveState(*cfg, state);
    }
    double base_vol = state.base_volume > 0.0 ? state.base_volume : 1.0;
    double current_vol = volumes.back();
    double rel_vol = current_vol / base_vol;

    const double price = closes.back();
    const int dec = cfg->decimals;

    auto rsi_value = Rsi(closes, kRsiPeriod);
    auto macd_value = Macd(closes, kMacdFast, kMacdSlow, kMacdSignal);
    auto ema_fast = EmaSeries(closes, kEmaFast);
    auto ema_slow = EmaSeries(closes, kEmaSlow);
    auto atr_value = Atr(highs, lows, closes, kAtrPeriod);
    auto adx_value = Adx(highs, lows, closes, kAdxPeriod);
    auto bb_value = Bollinger(closes, kBbPeriod, kBbMult);

    double avg_vol = 0.0;
    if ((int)volumes.size() >= kVolAvgPeriod) {
        for (int i = 0; i < kVolAvgPeriod; i++) {
            avg_vol += volumes[volumes.size() - kVolAvgPeriod + i];
        }
        avg_vol /= kVolAvgPeriod;
    }

    // ---------- Score, exactly as in the "ГЛОБАЛЬНЫЙ ПОДСЧЕТ ОЧКОВ" block ----------
    int score = 0;
    if (rsi_value.has_value()) {
        double r = rsi_value.value();
        if (strcmp(kRsiMode, "reversion") == 0) {
            score += r < 30.0 ? 1 : (r > 70.0 ? -1 : 0);
        } else {
            score += r > 50.0 ? 1 : -1;
        }
    }
    if (macd_value.valid) {
        score += macd_value.macd > macd_value.signal ? 1 : -1;
    }
    if (!ema_fast.empty() && !ema_slow.empty()) {
        score += ema_fast.back() > ema_slow.back() ? 1 : -1;
    }
    if (adx_value.valid && adx_value.adx > 20.0) {
        score += adx_value.plus_di > adx_value.minus_di ? 1 : -1;
    }
    if (bb_value.valid) {
        if (price > bb_value.upper) score -= 1;  // overheated, expect a pullback
        else if (price < bb_value.lower) score += 1;  // oversold, expect a rally
    }
    if (avg_vol > 0.0 && current_vol > avg_vol * 1.5) {
        if (price > closes[closes.size() - 2]) score += 1;
        else if (price < closes[closes.size() - 2]) score -= 1;
    }

    const char* verdict = Verdict(score);

    // ---------- Run the trading bot and persist the new state ----------
    double pnl_pct = 0.0, pnl_usd = 0.0, hold_time_min = 0.0;
    const char* action = BotAction(price, score, state, pnl_pct, pnl_usd, hold_time_min);
    SaveState(*cfg, state);

    // ---------- Build the MCP payload ----------
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "asset", cfg->key);
    cJSON_AddStringToObject(root, "name", cfg->name);
    cJSON_AddStringToObject(root, "ticker", cfg->ticker);
    cJSON_AddNumberToObject(root, "price", price);
    cJSON_AddNumberToObject(root, "bars", (double)closes.size());

    cJSON* indicators = cJSON_CreateObject();

    if (rsi_value.has_value()) {
        cJSON* rsi_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(rsi_json, "value", rsi_value.value());
        cJSON_AddStringToObject(rsi_json, "tag", RsiTag(rsi_value.value()));
        cJSON_AddItemToObject(indicators, "rsi", rsi_json);
    }

    if (macd_value.valid) {
        cJSON* macd_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(macd_json, "macd", macd_value.macd);
        cJSON_AddNumberToObject(macd_json, "signal", macd_value.signal);
        cJSON_AddNumberToObject(macd_json, "hist", macd_value.hist);
        cJSON_AddNumberToObject(macd_json, "hist_prev", macd_value.hist_prev);
        cJSON_AddStringToObject(macd_json, "direction",
                                macd_value.macd > macd_value.signal ? "BULLISH" : "BEARISH");
        bool growing = (macd_value.macd > macd_value.signal && macd_value.hist > macd_value.hist_prev) ||
                       (macd_value.macd < macd_value.signal && macd_value.hist < macd_value.hist_prev);
        cJSON_AddStringToObject(macd_json, "tag", growing ? "momentum growing" : "momentum weakening");
        cJSON_AddItemToObject(indicators, "macd", macd_json);
    }

    if (!ema_fast.empty() && !ema_slow.empty()) {
        cJSON* ema_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(ema_json, "fast", ema_fast.back());
        cJSON_AddNumberToObject(ema_json, "slow", ema_slow.back());
        double diff = ema_fast.back() - ema_slow.back();
        const char* tag;
        if (diff > 0) {
            tag = price > ema_fast.back() ? "uptrend" : "correction";
        } else {
            tag = price < ema_fast.back() ? "downtrend" : "bounce";
        }
        cJSON_AddStringToObject(ema_json, "tag", tag);
        cJSON_AddItemToObject(indicators, "ema", ema_json);
    }

    if (atr_value.has_value()) {
        cJSON* atr_json = cJSON_CreateObject();
        double atr_pct = atr_value.value() / price * 100.0;
        cJSON_AddNumberToObject(atr_json, "value", atr_value.value());
        cJSON_AddNumberToObject(atr_json, "percent", atr_pct);
        cJSON_AddStringToObject(atr_json, "tag", AtrTag(atr_pct));
        cJSON_AddItemToObject(indicators, "atr", atr_json);
    }

    if (adx_value.valid) {
        cJSON* adx_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(adx_json, "value", adx_value.adx);
        cJSON_AddNumberToObject(adx_json, "plus_di", adx_value.plus_di);
        cJSON_AddNumberToObject(adx_json, "minus_di", adx_value.minus_di);
        cJSON_AddStringToObject(adx_json, "tag", AdxTag(adx_value.adx));
        cJSON_AddStringToObject(adx_json, "direction",
                                adx_value.plus_di > adx_value.minus_di ? "BULLS (+DI)" : "BEARS (-DI)");
        cJSON_AddItemToObject(indicators, "adx", adx_json);
    }

    if (bb_value.valid) {
        cJSON* bb_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(bb_json, "lower", bb_value.lower);
        cJSON_AddNumberToObject(bb_json, "mid", bb_value.mid);
        cJSON_AddNumberToObject(bb_json, "upper", bb_value.upper);
        const char* tag = "inside the channel";
        if (price > bb_value.upper) tag = "breakout above the upper band";
        else if (price < bb_value.lower) tag = "breakdown below the lower band";
        cJSON_AddStringToObject(bb_json, "tag", tag);
        cJSON_AddItemToObject(indicators, "bollinger", bb_json);
    }

    if (avg_vol > 0.0) {
        cJSON* vol_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(vol_json, "current", current_vol);
        cJSON_AddNumberToObject(vol_json, "average", avg_vol);
        cJSON_AddNumberToObject(vol_json, "relative", rel_vol);
        cJSON_AddStringToObject(vol_json, "tag",
                                current_vol > avg_vol * 1.5 ? "SPIKE" : "normal");
        cJSON_AddItemToObject(indicators, "volume", vol_json);
    }

    cJSON_AddItemToObject(root, "indicators", indicators);
    cJSON_AddNumberToObject(root, "score", score);
    cJSON_AddStringToObject(root, "verdict", verdict);

    cJSON* bot = cJSON_CreateObject();
    cJSON_AddStringToObject(bot, "action", action);
    cJSON_AddStringToObject(bot, "message", BotMessage(action, score, price, state).c_str());
    cJSON_AddNumberToObject(bot, "usd", state.usd);
    cJSON_AddNumberToObject(bot, "asset_amount", state.asset);
    cJSON_AddNumberToObject(bot, "entry_price", state.entry);
    cJSON_AddNumberToObject(bot, "equity", state.usd + state.asset * price);
    cJSON_AddBoolToObject(bot, "in_position", state.asset > 0.0);
    cJSON_AddItemToObject(root, "bot", bot);

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "%s (%s) price %.*f, score %+d, action %s",
             cfg->name, cfg->ticker, dec, price, score, action);

    NotifyTrade(cfg, action, score, price, pnl_pct, pnl_usd, hold_time_min, state);

    report = std::move(result);
    return true;
}

}  // namespace

// ---------- 7. Monitor loop (the Python main()) ----------

bool MarketNotifier::ResolveAsset(const std::string& asset,
                                  size_t& index, std::string& error) const {
    std::string key = asset;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return (char)tolower(c);
    });

    for (size_t i = 0; i < kAssetCount; i++) {
        if (key == kAssets[i].key) {
            index = i;
            return true;
        }
    }

    error = "unknown asset '" + asset + "', use 'gold' or 'btc'";
    return false;
}

std::string MarketNotifier::Analyze(const std::string& asset) {
    size_t index = 0;
    std::string error;
    if (!ResolveAsset(asset, index, error)) {
        return ErrorJson(error);
    }

    std::string report, cycle_error;
    if (!RunCycle(&kAssets[index], report, cycle_error)) {
        return ErrorJson(cycle_error);
    }
    return report;
}

// Serialises the monitor state into the tool payload.
static std::string MonitorJson(bool running, const AssetConfig* cfg,
                               int interval_sec, const std::string& error,
                               const std::string& first_report) {
    cJSON* root = nullptr;

    // Reuse the first cycle's report as the base so the assistant can read the
    // analysis aloud right after starting the monitor.
    if (!first_report.empty()) {
        root = cJSON_Parse(first_report.c_str());
    }
    if (root == nullptr) {
        root = cJSON_CreateObject();
    }

    cJSON* monitor = cJSON_CreateObject();
    cJSON_AddBoolToObject(monitor, "running", running);
    if (cfg != nullptr) {
        cJSON_AddStringToObject(monitor, "asset", cfg->key);
        cJSON_AddNumberToObject(monitor, "interval_sec", interval_sec);
    }
    cJSON_AddItemToObject(root, "monitor", monitor);

    if (!error.empty()) {
        cJSON_AddStringToObject(root, "error", error.c_str());
    }

    char* json_str = cJSON_PrintUnformatted(root);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return result;
}

std::string MarketNotifier::MonitorStatus() {
    const AssetConfig* cfg = monitor_running_ ? &kAssets[monitor_asset_index_] : nullptr;
    return MonitorJson(monitor_running_, cfg, monitor_interval_sec_, "", "");
}

std::string MarketNotifier::StartMonitor(const std::string& asset, int interval_sec) {
    size_t index = 0;
    std::string error;
    if (!ResolveAsset(asset, index, error)) {
        return ErrorJson(error);
    }

    // Keep the request sane; the original script used a fixed 60 s.
    if (interval_sec < 10) interval_sec = 10;
    if (interval_sec > 3600) interval_sec = 3600;

    // Restart cleanly if it was already running for another asset.
    StopMonitor();

    // The first cycle runs synchronously so the tool call returns a full
    // report, exactly like the Python loop which starts with analyze().
    std::string first_report, cycle_error;
    RunCycle(&kAssets[index], first_report, cycle_error);
    if (!cycle_error.empty()) {
        ESP_LOGW(TAG, "Monitor first cycle failed: %s", cycle_error.c_str());
    }

    monitor_asset_index_ = index;
    monitor_interval_sec_ = interval_sec;
    monitor_task_alive_ = true;
    monitor_running_ = true;
    if (xTaskCreate(MonitorTaskEntry, "mkt_monitor", 8192, this, 3,
                    &monitor_task_handle_) != pdPASS) {
        monitor_running_ = false;
        monitor_task_alive_ = false;
        monitor_task_handle_ = nullptr;
        return ErrorJson("failed to create the monitor task");
    }

    ESP_LOGI(TAG, "Monitor started for %s every %d s",
             kAssets[index].key, interval_sec);
    return MonitorJson(true, &kAssets[index], interval_sec, cycle_error, first_report);
}

std::string MarketNotifier::StopMonitor() {
    if (!monitor_running_) {
        return MonitorStatus();
    }

    monitor_running_ = false;
    // The task sleeps in one second slices, so it notices the flag quickly.
    for (int i = 0; i < 15 && monitor_task_alive_; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (monitor_task_alive_) {
        ESP_LOGE(TAG, "Monitor task did not stop in time, killing it");
        if (monitor_task_handle_ != nullptr) {
            vTaskDelete(monitor_task_handle_);
        }
        monitor_task_alive_ = false;
    }
    monitor_task_handle_ = nullptr;

    ESP_LOGI(TAG, "Monitor stopped");
    return MonitorStatus();
}

void MarketNotifier::MonitorTaskEntry(void* arg) {
    auto* self = static_cast<MarketNotifier*>(arg);
    self->MonitorTaskLoop();
    self->monitor_task_alive_ = false;
    vTaskDelete(nullptr);
}

void MarketNotifier::MonitorTaskLoop() {
    const AssetConfig* cfg = &kAssets[monitor_asset_index_];

    while (monitor_running_) {
        // Sleep first: StartMonitor() already did the first cycle, and the
        // period matches the Python time.sleep(INTERVAL).
        for (int slept = 0; monitor_running_ && slept < monitor_interval_sec_; slept++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!monitor_running_) {
            break;
        }

        std::string report, error;
        if (!RunCycle(cfg, report, error)) {
            // Mirrors the Python loop, which logs the error and keeps going.
            ESP_LOGE(TAG, "Monitor cycle for %s failed: %s", cfg->key, error.c_str());
        }
    }
}

MarketNotifier::~MarketNotifier() {
    StopMonitor();
}
