#pragma once
// ============================================================
//  TOP4EA — metaapi_bridge.hpp
//  Connexion MetaApi REST (libcurl) — MT4 via cloud
//  Auth   : Bearer token (env METAAPI_TOKEN)
//  Account: env METAAPI_ACCOUNT_ID
//  Ticks  : polling /symbols/{sym}/currentPrice toutes les 200ms
//  Ordres : polling /positions + /orders toutes les 2s
//  Trades : POST /trade (synchrone, libcurl)
// ============================================================
#include "types.hpp"
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <optional>
#include <string>
#include <vector>
#include <future>
#include <queue>
#include <condition_variable>

namespace top4ea {

using TickCallback    = std::function<void(const Tick&)>;
using OrderCallback   = std::function<void(const OrderInfo&)>;
using AccountCallback = std::function<void(const AccountSnapshot&)>;
using ErrorCallback   = std::function<void(int code, const std::string& msg)>;
using ConnectCallback = std::function<void(bool connected)>;

struct TradeCmd {
    enum class Op : uint8_t {
        MarketOrder,
        DeletePending,
        CloseOrder,
        CloseBy,
        ModifyOrder
    };
    Op          op        = Op::MarketOrder;
    OrderType   type      = OrderType::Buy;
    int         ticket    = -1;
    int         ticket2   = -1;
    double      lots      = 0.0;
    double      price     = 0.0;
    double      sl        = 0.0;
    double      tp        = 0.0;
    int         slippage  = 3;
    int         magic     = 0;
    std::string comment   = {};
    std::string symbol    = {};
};

struct BridgeResponse {
    bool        success  = false;
    int         ticket   = -1;
    int         error    = 0;
    double      price    = 0.0;
    std::string msg      = {};
};

// ============================================================
//  MetaApiBridge — connexion REST cloud MetaApi
// ============================================================
class MetaApiBridge {
public:
    explicit MetaApiBridge(const Config& cfg);
    ~MetaApiBridge();

    void start();
    void stop();
    [[nodiscard]] bool isConnected() const noexcept { return connected_.load(); }

    BridgeResponse sendCommand(const TradeCmd& cmd, int timeoutMs = 5000);

    void onTick   (TickCallback    cb) { tickCb_    = std::move(cb); }
    void onOrder  (OrderCallback   cb) { orderCb_   = std::move(cb); }
    void onAccount(AccountCallback cb) { accountCb_ = std::move(cb); }
    void onError  (ErrorCallback   cb) { errorCb_   = std::move(cb); }
    void onConnect(ConnectCallback cb) { connectCb_ = std::move(cb); }

    [[nodiscard]] std::vector<OrderInfo>          requestOrders (int timeoutMs = 3000);
    [[nodiscard]] std::optional<AccountSnapshot>  requestAccount(int timeoutMs = 3000);

private:
    // Threads de polling
    void tickLoop();
    void stateLoop();

    // ─── Socket.IO EIO4 long-polling ─────────────────────────
    std::string              sioGet (const std::string& sid, int timeoutSec = 35);
    bool                     sioPost(const std::string& sid, const std::string& body);
    std::vector<std::string> parseEIOPackets(const std::string& payload);
    std::string              sioRequest(const std::string& jsonBody);
    void                     processSocketIOPacket(const std::string& pkt);
    void                     processMetaApiEvent(const std::string& event,
                                                 const std::string& data);

    // ─── Appels REST (trades, compte, positions) ──────────────
    std::string httpGet (const std::string& path);
    std::string httpPost(const std::string& path, const std::string& body);

    // ─── Parsing JSON (sans lib externe) ─────────────────────
    static std::string jsonGet (const std::string& json, const std::string& key);
    static double      jsonGetD(const std::string& json, const std::string& key);
    static int         jsonGetI(const std::string& json, const std::string& key);
    static bool        jsonGetB(const std::string& json, const std::string& key);
    static std::string jsonGetNested(const std::string& json,
                                     const std::string& key);

    // ─── Parseurs JSON → types métier ────────────────────────
    static Tick            parseTick        (const std::string& json);
    static OrderInfo       parsePosition    (const std::string& json);
    static OrderInfo       parsePendingOrder(const std::string& json);
    static AccountSnapshot parseAccount     (const std::string& json);
    static BridgeResponse  parseTradeResult (const std::string& json);

    // ─── Sérialisation commande → JSON MetaApi ───────────────
    std::string cmdToJson(const TradeCmd& cmd) const;

    // ─── Extraction tableau JSON [{...},{...}] ────────────────
    static std::vector<std::string> parseJsonArray(const std::string& json);

    Config      cfg_;
    std::string token_;
    std::string accountId_;
    std::string baseUrl_;

    std::atomic<bool> connected_  { false };
    std::atomic<bool> running_    { false };

    std::jthread tickThread_;
    std::jthread stateThread_;

    TickCallback    tickCb_;
    OrderCallback   orderCb_;
    AccountCallback accountCb_;
    ErrorCallback   errorCb_;
    ConnectCallback connectCb_;
};

} // namespace top4ea
