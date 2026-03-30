// ============================================================
//  TOP4EA — metaapi_bridge.cpp  C++20
//  Connexion MetaApi :
//   - REST polling /symbols/{sym}/currentPrice pour ticks fiables
//   - REST POST /trade pour ordres
//   - REST polling /positions /orders /accountInformation
// ============================================================
#include "../include/metaapi_bridge.hpp"
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>

namespace top4ea {

// ─── Callback libcurl pour accumuler la réponse ─────────────
static size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// ────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ────────────────────────────────────────────────────────────
MetaApiBridge::MetaApiBridge(const Config& cfg) : cfg_(cfg) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char* tok = std::getenv("METAAPI_TOKEN");
    const char* aid = std::getenv("METAAPI_ACCOUNT_ID");
    const char* reg = std::getenv("METAAPI_REGION");

    token_     = tok ? tok : "";
    accountId_ = aid ? aid : "";
    std::string region = (reg && *reg) ? reg : "new-york";

    if (token_.empty() || accountId_.empty()) {
        std::cerr << "[MetaApi] ERREUR : METAAPI_TOKEN ou METAAPI_ACCOUNT_ID "
                     "non définis\n";
    }

    baseUrl_ = "https://mt-client-api-v1." + region + ".agiliumtrade.ai";
    std::cout << "[MetaApi] URL : " << baseUrl_ << "\n";
}

MetaApiBridge::~MetaApiBridge() {
    stop();
    curl_global_cleanup();
}

// ────────────────────────────────────────────────────────────
//  start / stop
// ────────────────────────────────────────────────────────────
void MetaApiBridge::start() {
    running_.store(true);

    auto acc = requestAccount(8000);
    if (acc) {
        connected_.store(true);
        if (connectCb_) connectCb_(true);
        std::cout << "[MetaApi] Connecté — " << accountId_
                  << " | Balance=" << acc->balance << " " << acc->currency << "\n";
        if (accountCb_) accountCb_(*acc);
    } else {
        std::cerr << "[MetaApi] Connexion initiale échouée — "
                     "vérifiez METAAPI_TOKEN / METAAPI_ACCOUNT_ID\n";
    }

    tickThread_  = std::jthread([this](std::stop_token){ tickLoop();  });
    stateThread_ = std::jthread([this](std::stop_token){ stateLoop(); });
}

void MetaApiBridge::stop() {
    running_.store(false);
    if (tickThread_.joinable())  tickThread_.request_stop();
    if (stateThread_.joinable()) stateThread_.request_stop();
    connected_.store(false);
}

// ============================================================
//  TICK LOOP — REST polling /symbols/{sym}/currentPrice
//  Remplace Socket.IO (trop instable). Simple et robuste.
// ============================================================
void MetaApiBridge::tickLoop() {
    int failCount = 0;
    const int maxFail = 10;

    std::cout << "[MetaApi] Tick REST polling actif ("
              << cfg_.tickPollMs << " ms) — " << cfg_.symbol << "\n";

    while (running_.load()) {
        std::string path = "/users/current/accounts/" + accountId_
                         + "/symbols/" + cfg_.symbol + "/current-price";
        std::string resp = httpGet(path);

        if (!resp.empty()) {
            Tick t = parseTick(resp);
            if (t.bid > 0.0 && t.ask > 0.0) {
                failCount = 0;

                if (!connected_.load()) {
                    connected_.store(true);
                    if (connectCb_) connectCb_(true);
                    std::cout << "[MetaApi] Prix actif — "
                              << cfg_.symbol
                              << " Bid=" << t.bid
                              << " Ask=" << t.ask << "\n";
                }

                if (tickCb_) tickCb_(t);

            } else {
                ++failCount;
            }
        } else {
            ++failCount;
            if (failCount >= maxFail) {
                std::cerr << "[MetaApi] " << failCount
                          << " échecs tick consécutifs — tentative reconnexion\n";
                if (connected_.exchange(false)) {
                    if (connectCb_) connectCb_(false);
                }
                failCount = 0;
                std::this_thread::sleep_for(Ms(cfg_.reconnectMs * 3));

                auto acc = requestAccount(5000);
                if (acc) {
                    connected_.store(true);
                    if (connectCb_) connectCb_(true);
                    std::cout << "[MetaApi] Reconnecté\n";
                }
            }
        }

        std::this_thread::sleep_for(Ms(cfg_.tickPollMs));
    }
    std::cout << "[MetaApi] Tick loop terminée\n";
}

// ────────────────────────────────────────────────────────────
//  STATE LOOP — polling compte + positions + ordres
// ────────────────────────────────────────────────────────────
void MetaApiBridge::stateLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(Ms(cfg_.statePollMs));
        if (!running_.load()) break;

        std::string accResp = httpGet(
            "/users/current/accounts/" + accountId_ + "/accountInformation");
        if (!accResp.empty()) {
            auto acc = parseAccount(accResp);
            if (acc.balance > 0.0 && accountCb_) accountCb_(acc);
        }

        std::string posResp = httpGet(
            "/users/current/accounts/" + accountId_ + "/positions");
        if (!posResp.empty()) {
            for (const auto& item : parseJsonArray(posResp)) {
                auto ord = parsePosition(item);
                if (ord.ticket >= 0 && orderCb_) orderCb_(ord);
            }
        }

        std::string ordResp = httpGet(
            "/users/current/accounts/" + accountId_ + "/orders");
        if (!ordResp.empty()) {
            for (const auto& item : parseJsonArray(ordResp)) {
                auto ord = parsePendingOrder(item);
                if (ord.ticket >= 0 && orderCb_) orderCb_(ord);
            }
        }
    }
}

// ────────────────────────────────────────────────────────────
//  sendCommand — trade via REST POST
// ────────────────────────────────────────────────────────────
BridgeResponse MetaApiBridge::sendCommand(const TradeCmd& cmd, int /*timeoutMs*/) {
    std::string body = cmdToJson(cmd);
    std::string path = "/users/current/accounts/" + accountId_ + "/trade";
    std::string resp = httpPost(path, body);

    if (resp.empty()) {
        BridgeResponse r;
        r.success = false;
        r.error   = -1;
        r.msg     = "Pas de réponse MetaApi";
        return r;
    }
    return parseTradeResult(resp);
}

// ────────────────────────────────────────────────────────────
//  requestOrders / requestAccount (synchrones via REST)
// ────────────────────────────────────────────────────────────
std::vector<OrderInfo> MetaApiBridge::requestOrders(int /*timeoutMs*/) {
    std::vector<OrderInfo> result;

    std::string posResp = httpGet(
        "/users/current/accounts/" + accountId_ + "/positions");
    if (!posResp.empty()) {
        for (const auto& item : parseJsonArray(posResp))
            result.push_back(parsePosition(item));
    }

    std::string ordResp = httpGet(
        "/users/current/accounts/" + accountId_ + "/orders");
    if (!ordResp.empty()) {
        for (const auto& item : parseJsonArray(ordResp))
            result.push_back(parsePendingOrder(item));
    }

    return result;
}

std::optional<AccountSnapshot> MetaApiBridge::requestAccount(int /*timeoutMs*/) {
    std::string resp = httpGet(
        "/users/current/accounts/" + accountId_ + "/accountInformation");
    if (resp.empty()) return std::nullopt;
    auto acc = parseAccount(resp);
    if (acc.balance <= 0.0) return std::nullopt;
    return acc;
}

// ────────────────────────────────────────────────────────────
//  HTTP GET via libcurl
// ────────────────────────────────────────────────────────────
std::string MetaApiBridge::httpGet(const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    std::string fullUrl    = baseUrl_ + path;
    std::string authHeader = "auth-token: " + token_;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            fullUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[MetaApi] curl: " << curl_easy_strerror(res) << "\n";
        return {};
    }
    if (httpCode < 200 || httpCode >= 300) {
        std::cerr << "[MetaApi] GET HTTP " << httpCode << " — "
                  << path.substr(0, 80) << "\n";
        return {};
    }
    return response;
}

// ────────────────────────────────────────────────────────────
//  HTTP POST via libcurl
// ────────────────────────────────────────────────────────────
std::string MetaApiBridge::httpPost(const std::string& path,
                                    const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    std::string fullUrl    = baseUrl_ + path;
    std::string authHeader = "auth-token: " + token_;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            fullUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    if (httpCode < 200 || httpCode >= 300) {
        std::cerr << "[MetaApi] POST HTTP " << httpCode
                  << " — " << response.substr(0, 200) << "\n";
        return {};
    }
    return response;
}

// ────────────────────────────────────────────────────────────
//  Sérialisation commande → JSON MetaApi
// ────────────────────────────────────────────────────────────
std::string MetaApiBridge::cmdToJson(const TradeCmd& cmd) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(5);

    auto actionType = [&]() -> std::string {
        if (cmd.op == TradeCmd::Op::DeletePending) return "ORDER_CANCEL";
        if (cmd.op == TradeCmd::Op::CloseOrder)   return "POSITION_CLOSE_ID";
        if (cmd.op == TradeCmd::Op::CloseBy)      return "POSITION_CLOSE_ID";
        if (cmd.op == TradeCmd::Op::ModifyOrder)  return "ORDER_MODIFY";
        switch (cmd.type) {
            case OrderType::Buy:       return "ORDER_TYPE_BUY";
            case OrderType::Sell:      return "ORDER_TYPE_SELL";
            case OrderType::BuyLimit:  return "ORDER_TYPE_BUY_LIMIT";
            case OrderType::SellLimit: return "ORDER_TYPE_SELL_LIMIT";
            case OrderType::BuyStop:   return "ORDER_TYPE_BUY_STOP";
            case OrderType::SellStop:  return "ORDER_TYPE_SELL_STOP";
            default:                   return "ORDER_TYPE_BUY";
        }
    }();

    oss << "{\"actionType\":\"" << actionType << "\"";

    if (cmd.op == TradeCmd::Op::MarketOrder) {
        std::string sym = cmd.symbol.empty() ? cfg_.symbol : cmd.symbol;
        oss << ",\"symbol\":\""   << sym          << "\"";
        oss << ",\"volume\":"     << cmd.lots;
        if (cmd.price > 0.0)
            oss << ",\"openPrice\":" << std::fixed << std::setprecision(2) << cmd.price << std::defaultfloat;
        if (cmd.sl > 0.0)
            oss << ",\"stopLoss\":"   << cmd.sl;
        if (cmd.tp > 0.0)
            oss << ",\"takeProfit\":" << cmd.tp;
        oss << ",\"comment\":\""  << cmd.comment << "\"";
        oss << ",\"magic\":"      << cmd.magic;
        oss << ",\"slippage\":"   << cmd.slippage;
    } else if (cmd.op == TradeCmd::Op::CloseOrder ||
               cmd.op == TradeCmd::Op::CloseBy) {
        oss << ",\"positionId\":\"" << cmd.ticket << "\"";
    } else if (cmd.op == TradeCmd::Op::DeletePending) {
        oss << ",\"orderId\":\"" << cmd.ticket << "\"";
    } else if (cmd.op == TradeCmd::Op::ModifyOrder) {
        // Ordres pending (SellStop / BuyStop) → orderId + openPrice obligatoire
        oss << ",\"orderId\":\"" << cmd.ticket << "\"";
        if (cmd.price > 0.0)
            oss << ",\"openPrice\":" << std::fixed << std::setprecision(2) << cmd.price << std::defaultfloat;
        if (cmd.sl > 0.0)
            oss << ",\"stopLoss\":"   << std::fixed << std::setprecision(2) << cmd.sl;
        if (cmd.tp > 0.0)
            oss << ",\"takeProfit\":" << std::fixed << std::setprecision(2) << cmd.tp;
    }

    oss << "}";
    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  Parseurs JSON
// ────────────────────────────────────────────────────────────
Tick MetaApiBridge::parseTick(const std::string& j) {
    Tick t;
    t.bid       = jsonGetD(j, "bid");
    t.ask       = jsonGetD(j, "ask");
    if (t.ask <= 0.0) t.ask = t.bid;
    t.spread    = t.ask - t.bid;
    t.localTime = Clock::now();
    return t;
}

OrderInfo MetaApiBridge::parsePosition(const std::string& j) {
    OrderInfo o;
    std::string idStr = jsonGet(j, "id");
    try { o.ticket = idStr.empty() ? -1 : std::stoi(idStr); }
    catch (...) { o.ticket = -1; }

    std::string typeStr = jsonGet(j, "type");
    if      (typeStr == "POSITION_TYPE_BUY")  o.type = OrderType::Buy;
    else if (typeStr == "POSITION_TYPE_SELL") o.type = OrderType::Sell;
    else o.type = OrderType::None;

    o.lots       = jsonGetD(j, "volume");
    o.openPrice  = jsonGetD(j, "openPrice");
    o.sl         = jsonGetD(j, "stopLoss");
    o.tp         = jsonGetD(j, "takeProfit");
    o.profit     = jsonGetD(j, "profit");
    o.swap       = jsonGetD(j, "swap");
    o.commission = jsonGetD(j, "commission");
    o.comment    = jsonGet (j, "comment");
    return o;
}

OrderInfo MetaApiBridge::parsePendingOrder(const std::string& j) {
    OrderInfo o;
    std::string idStr = jsonGet(j, "id");
    try { o.ticket = idStr.empty() ? -1 : std::stoi(idStr); }
    catch (...) { o.ticket = -1; }

    std::string typeStr = jsonGet(j, "type");
    if      (typeStr == "ORDER_TYPE_BUY_STOP")   o.type = OrderType::BuyStop;
    else if (typeStr == "ORDER_TYPE_SELL_STOP")  o.type = OrderType::SellStop;
    else if (typeStr == "ORDER_TYPE_BUY_LIMIT")  o.type = OrderType::BuyLimit;
    else if (typeStr == "ORDER_TYPE_SELL_LIMIT") o.type = OrderType::SellLimit;
    else o.type = OrderType::None;

    o.lots      = jsonGetD(j, "volume");
    o.openPrice = jsonGetD(j, "openPrice");
    o.sl        = jsonGetD(j, "stopLoss");
    o.tp        = jsonGetD(j, "takeProfit");
    o.comment   = jsonGet (j, "comment");
    return o;
}

AccountSnapshot MetaApiBridge::parseAccount(const std::string& j) {
    AccountSnapshot a;
    a.balance     = jsonGetD(j, "balance");
    a.equity      = jsonGetD(j, "equity");
    a.freeMargin  = jsonGetD(j, "freeMargin");
    a.marginLevel = jsonGetD(j, "marginLevel");
    a.leverage    = jsonGetI(j, "leverage");
    a.currency    = jsonGet (j, "currency");
    return a;
}

BridgeResponse MetaApiBridge::parseTradeResult(const std::string& j) {
    BridgeResponse r;
    std::string code = jsonGet(j, "numericCode");
    r.error = code.empty() ? 0 : std::stoi(code);

    std::string strCode = jsonGet(j, "stringCode");
    r.success = (strCode == "ERR_NO_ERROR" ||
                 strCode == "TRADE_RETCODE_DONE" ||
                 r.error == 0);

    std::string ordId = jsonGet(j, "orderId");
    std::string posId = jsonGet(j, "positionId");
    std::string idStr = posId.empty() ? ordId : posId;
    try { r.ticket = idStr.empty() ? -1 : std::stoi(idStr); }
    catch (...) { r.ticket = -1; }

    r.msg = jsonGet(j, "message");
    return r;
}

// ────────────────────────────────────────────────────────────
//  Extraction tableau JSON [{...},{...}]
// ────────────────────────────────────────────────────────────
std::vector<std::string> MetaApiBridge::parseJsonArray(const std::string& json) {
    std::vector<std::string> result;
    size_t start = json.find('[');
    if (start == std::string::npos) return result;
    ++start;

    int    depth    = 0;
    size_t objStart = std::string::npos;

    for (size_t i = start; i < json.size(); ++i) {
        char c = json[i];
        if (c == '{') {
            if (depth == 0) objStart = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && objStart != std::string::npos) {
                result.push_back(json.substr(objStart, i - objStart + 1));
                objStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return result;
}

// ────────────────────────────────────────────────────────────
//  Extracteur JSON minimal (clé:valeur plat)
// ────────────────────────────────────────────────────────────
std::string MetaApiBridge::jsonGet(const std::string& json,
                                   const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        ++pos;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    }
    if (json[pos] == 'n') return {};
    auto end = json.find_first_of(",}]", pos);
    return (end != std::string::npos)
           ? json.substr(pos, end - pos)
           : json.substr(pos);
}

double MetaApiBridge::jsonGetD(const std::string& j, const std::string& k) {
    auto s = jsonGet(j, k);
    if (s.empty()) return 0.0;
    try { return std::stod(s); } catch (...) { return 0.0; }
}

int MetaApiBridge::jsonGetI(const std::string& j, const std::string& k) {
    auto s = jsonGet(j, k);
    if (s.empty()) return 0;
    try { return std::stoi(s); } catch (...) { return 0; }
}

bool MetaApiBridge::jsonGetB(const std::string& j, const std::string& k) {
    auto s = jsonGet(j, k);
    return (s == "true" || s == "1");
}

std::string MetaApiBridge::jsonGetNested(const std::string& json,
                                          const std::string& key) {
    return jsonGet(json, key);
}

} // namespace top4ea
