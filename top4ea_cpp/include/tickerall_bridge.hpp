#pragma once

#include "metaapi_bridge.hpp"
#include <curl/curl.h>
#include <unordered_set>
#include <string>
#include <unordered_map>

namespace top4ea {

class TickerallBridge final : public TradingBridge {
public:
    explicit TickerallBridge(const Config& cfg);
    ~TickerallBridge() override;

    void start() override;
    void stop() override;
    [[nodiscard]] bool isConnected() const noexcept override {
        return connected_.load();
    }

    BridgeResponse sendCommand(const TradeCmd& cmd,
                               int timeoutMs = 5000) override;
    void onTick(TickCallback cb) override { tickCb_ = std::move(cb); }
    void onOrder(OrderCallback cb) override { orderCb_ = std::move(cb); }
    void onAccount(AccountCallback cb) override { accountCb_ = std::move(cb); }
    void onError(ErrorCallback cb) override { errorCb_ = std::move(cb); }
    void onConnect(ConnectCallback cb) override { connectCb_ = std::move(cb); }

    [[nodiscard]] std::vector<OrderInfo> requestOrders(
        int timeoutMs = 3000) override;
    [[nodiscard]] std::optional<AccountSnapshot> requestAccount(
        int timeoutMs = 3000) override;
    [[nodiscard]] SymbolSpec requestSymbolSpec(
        const std::string& symbol, int timeoutMs = 8000) override;
    void updateConfig(const Config& cfg) noexcept override { cfg_ = cfg; }

private:
    struct HttpResponse {
        long status = 0;
        std::string body;
    };

    [[nodiscard]] HttpResponse request(const std::string& method,
                                       const std::string& path,
                                       const std::string& body = {},
                                       int timeoutMs = 8000,
                                       bool write = false);
    [[nodiscard]] bool discoverDemoAccount();
    [[nodiscard]] bool verifyDemoAccount(AccountSnapshot* snapshot = nullptr);
    [[nodiscard]] bool connectStream();
    void closeStream() noexcept;
    void tickLoop();
    void stateLoop();

    [[nodiscard]] std::string accountPath() const;
    [[nodiscard]] std::string resolveTicket(int ticket) const;
    int registerTicket(const std::string& ticket);

    static std::string jsonString(const std::string& value);
    static std::string jsonValue(const std::string& json,
                                 const std::string& key);
    static std::string jsonObject(const std::string& json,
                                  const std::string& key);
    static std::vector<std::string> jsonObjects(const std::string& json,
                                                const std::string& key);
    static double jsonNumber(const std::string& json, const std::string& key);
    static bool jsonTrue(const std::string& json, const std::string& key);
    static AccountSnapshot parseAccount(const std::string& json);
    OrderInfo parsePosition(const std::string& json);
    OrderInfo parsePendingOrder(const std::string& json);

    Config cfg_;
    std::string apiKey_;
    std::string accountId_;
    std::string baseUrl_ = "https://api.tickerall.com";
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::jthread tickThread_;
    std::jthread stateThread_;

    mutable std::mutex ticketMutex_;
    std::unordered_map<int, std::string> ticketMap_;
    int nextTicket_ = 1000000;
    std::mutex uncertainWritesMutex_;
    std::unordered_set<std::string> uncertainWrites_;

    CURL* stream_ = nullptr;
    std::mutex streamMutex_;
    TickCallback tickCb_;
    OrderCallback orderCb_;
    AccountCallback accountCb_;
    ErrorCallback errorCb_;
    ConnectCallback connectCb_;
};

} // namespace top4ea
