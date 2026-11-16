#include "../include/tickerall_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <random>
#include <sstream>

namespace top4ea {
namespace {

size_t curlWrite(char* ptr, size_t size, size_t count, void* data) {
    auto* response = static_cast<std::string*>(data);
    response->append(ptr, size * count);
    return size * count;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string idempotencyKey() {
    static std::atomic<uint64_t> sequence{0};
    thread_local std::mt19937_64 random(std::random_device{}());
    std::ostringstream key;
    key << "top4ea-" << std::hex << random() << '-' << ++sequence;
    return key.str();
}

} // namespace

TickerallBridge::TickerallBridge(const Config& cfg) : cfg_(cfg) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const char* key = std::getenv("TICKERALL_API_KEY");
    apiKey_ = key ? key : "";
}

TickerallBridge::~TickerallBridge() {
    stop();
    curl_global_cleanup();
}

std::string TickerallBridge::accountPath() const {
    return "/v1/accounts/" + accountId_;
}

TickerallBridge::HttpResponse TickerallBridge::request(
    const std::string& method, const std::string& path, const std::string& body,
    int timeoutMs, bool write) {
    HttpResponse result;
    if (apiKey_.empty()) return result;

    CURL* curl = curl_easy_init();
    if (!curl) return result;

    struct curl_slist* headers = nullptr;
    const std::string auth = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (write) {
        const std::string idempotency = "Idempotency-Key: " + idempotencyKey();
        headers = curl_slist_append(headers, idempotency.c_str());
    }

    const std::string url = baseUrl_ + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body.size()));
    }

    const CURLcode status = curl_easy_perform(curl);
    if (status == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (status != CURLE_OK) {
        result.body.clear();
        result.status = 0;
    }
    return result;
}

std::string TickerallBridge::jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20)
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<int>(ch);
                else
                    out << static_cast<char>(ch);
        }
    }
    out << '"';
    return out.str();
}

std::string TickerallBridge::jsonValue(const std::string& json,
                                       const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        ++pos;
    if (pos == json.size() || json.compare(pos, 4, "null") == 0) return {};

    if (json[pos] == '"') {
        std::string value;
        for (++pos; pos < json.size(); ++pos) {
            const char ch = json[pos];
            if (ch == '"') return value;
            if (ch == '\\' && pos + 1 < json.size()) {
                const char escaped = json[++pos];
                switch (escaped) {
                    case '"': value += '"'; break;
                    case '\\': value += '\\'; break;
                    case '/': value += '/'; break;
                    case 'b': value += '\b'; break;
                    case 'f': value += '\f'; break;
                    case 'n': value += '\n'; break;
                    case 'r': value += '\r'; break;
                    case 't': value += '\t'; break;
                    default: value += escaped; break;
                }
            } else {
                value += ch;
            }
        }
        return {};
    }

    const size_t end = json.find_first_of(",}]", pos);
    return json.substr(pos, end == std::string::npos ? end : end - pos);
}

std::string TickerallBridge::jsonObject(const std::string& json,
                                        const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('{', pos + 1);
    if (pos == std::string::npos) return {};
    const size_t start = pos;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') quoted = true;
        else if (ch == '{') ++depth;
        else if (ch == '}' && --depth == 0)
            return json.substr(start, pos - start + 1);
    }
    return {};
}

std::vector<std::string> TickerallBridge::jsonObjects(
    const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    size_t pos = std::string::npos;
    if (key.empty()) {
        pos = json.find('[');
    } else {
        const std::string needle = "\"" + key + "\"";
        pos = json.find(needle);
        if (pos == std::string::npos) return result;
        pos = json.find('[', pos + needle.size());
    }
    if (pos == std::string::npos) return result;

    int depth = 0;
    size_t start = std::string::npos;
    bool quoted = false;
    bool escaped = false;
    for (++pos; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') quoted = true;
        else if (ch == '{') {
            if (depth++ == 0) start = pos;
        } else if (ch == '}') {
            if (--depth == 0 && start != std::string::npos) {
                result.push_back(json.substr(start, pos - start + 1));
                start = std::string::npos;
            }
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return result;
}

double TickerallBridge::jsonNumber(const std::string& json,
                                   const std::string& key) {
    const std::string value = jsonValue(json, key);
    if (value.empty()) return 0.0;
    try { return std::stod(value); } catch (...) { return 0.0; }
}

bool TickerallBridge::jsonTrue(const std::string& json,
                               const std::string& key) {
    return jsonValue(json, key) == "true";
}

AccountSnapshot TickerallBridge::parseAccount(const std::string& json) {
    const std::string account = jsonObject(json, "account");
    AccountSnapshot snapshot;
    snapshot.balance = jsonNumber(account, "balance");
    snapshot.equity = jsonNumber(account, "equity");
    snapshot.freeMargin = jsonNumber(account, "freeMargin");
    snapshot.marginLevel = jsonNumber(account, "marginLevel");
    snapshot.leverage = static_cast<int>(jsonNumber(account, "leverage"));
    snapshot.currency = jsonValue(account, "currency");
    snapshot.platform = PlatformType::MT5;
    return snapshot;
}

int TickerallBridge::registerTicket(const std::string& ticket) {
    if (ticket.empty()) return -1;
    std::lock_guard lock(ticketMutex_);
    for (const auto& [synthetic, real] : ticketMap_)
        if (real == ticket) return synthetic;
    const int synthetic = ++nextTicket_;
    ticketMap_.emplace(synthetic, ticket);
    return synthetic;
}

std::string TickerallBridge::resolveTicket(int ticket) const {
    std::lock_guard lock(ticketMutex_);
    const auto it = ticketMap_.find(ticket);
    return it == ticketMap_.end() ? std::string{} : it->second;
}

OrderInfo TickerallBridge::parsePosition(const std::string& json) {
    OrderInfo order;
    order.id = jsonValue(json, "ticket");
    order.ticket = registerTicket(order.id);
    const std::string side = jsonValue(json, "side");
    if (side == "BUY") order.type = OrderType::Buy;
    else if (side == "SELL") order.type = OrderType::Sell;
    order.lots = jsonNumber(json, "volume");
    order.openPrice = jsonNumber(json, "entryPrice");
    order.sl = jsonNumber(json, "stopLoss");
    order.tp = jsonNumber(json, "takeProfit");
    order.profit = jsonNumber(json, "profit");
    order.swap = jsonNumber(json, "swap");
    order.commission = jsonNumber(json, "commission");
    order.comment = jsonValue(json, "comment");
    return order;
}

OrderInfo TickerallBridge::parsePendingOrder(const std::string& json) {
    OrderInfo order;
    order.id = jsonValue(json, "ticket");
    order.ticket = registerTicket(order.id);
    const std::string type = jsonValue(json, "type");
    if (type == "BUY_STOP") order.type = OrderType::BuyStop;
    else if (type == "SELL_STOP") order.type = OrderType::SellStop;
    else if (type == "BUY_LIMIT") order.type = OrderType::BuyLimit;
    else if (type == "SELL_LIMIT") order.type = OrderType::SellLimit;
    order.lots = jsonNumber(json, "volume");
    order.openPrice = jsonNumber(json, "price");
    order.sl = jsonNumber(json, "stopLoss");
    order.tp = jsonNumber(json, "takeProfit");
    order.comment = jsonValue(json, "comment");
    return order;
}

std::optional<AccountSnapshot> TickerallBridge::requestAccount(int timeoutMs) {
    if (!discoverDemoAccount()) return std::nullopt;
    const auto response = request("GET", accountPath(), {}, timeoutMs);
    if (response.status < 200 || response.status >= 300 ||
        jsonValue(response.body, "status") != "online")
        return std::nullopt;
    auto account = parseAccount(response.body);
    if (account.balance <= 0.0 || account.currency.empty()) return std::nullopt;
    return account;
}

bool TickerallBridge::discoverDemoAccount() {
    if (!accountId_.empty()) return true;
    if (apiKey_.empty()) return false;

    const auto response = request("GET", "/v1/accounts", {}, 8000);
    if (response.status < 200 || response.status >= 300) {
        std::cerr << "[TickerAll] Impossible de lister les comptes liés\n";
        return false;
    }

    std::vector<std::string> candidates;
    for (const auto& account : jsonObjects(response.body, {})) {
        if (jsonValue(account, "broker") != "mt5" ||
            !jsonTrue(account, "isDemo"))
            continue;
        if (!cfg_.tickerallServer.empty() &&
            lower(jsonValue(account, "server")) != lower(cfg_.tickerallServer))
            continue;
        candidates.push_back(account);
    }

    if (candidates.size() != 1) {
        std::cerr << "[TickerAll] "
                  << (candidates.empty()
                          ? "Aucun compte MT5 démo ne correspond au serveur configuré"
                          : "Plusieurs comptes MT5 démo correspondent au serveur configuré")
                  << " — moteur désarmé\n";
        return false;
    }

    accountId_ = jsonValue(candidates.front(), "id");
    if (accountId_.empty()) {
        std::cerr << "[TickerAll] Compte démo sans identifiant — moteur désarmé\n";
        return false;
    }
    return true;
}

bool TickerallBridge::verifyDemoAccount(AccountSnapshot* snapshot) {
    if (!discoverDemoAccount()) return false;
    const auto response = request("GET", accountPath(), {}, 8000);
    if (response.status < 200 || response.status >= 300) {
        std::cerr << "[TickerAll] Compte inaccessible — moteur désarmé\n";
        return false;
    }
    const std::string broker = jsonValue(response.body, "broker");
    const std::string server = jsonValue(response.body, "server");
    if (!jsonTrue(response.body, "isDemo") || broker != "mt5" ||
        jsonValue(response.body, "status") != "online" ||
        (!cfg_.tickerallServer.empty() &&
         lower(server) != lower(cfg_.tickerallServer))) {
        std::cerr << "[TickerAll] Vérification démo MT5/serveur échouée — "
                     "aucun ordre ne sera envoyé\n";
        return false;
    }
    if (snapshot) *snapshot = parseAccount(response.body);
    return true;
}

SymbolSpec TickerallBridge::requestSymbolSpec(const std::string& symbol,
                                               int timeoutMs) {
    SymbolSpec spec;
    spec.symbol = symbol;
    if (!discoverDemoAccount()) return spec;

    CURL* escapeHandle = curl_easy_init();
    if (!escapeHandle) return spec;
    char* escapedSymbol = curl_easy_escape(
        escapeHandle, symbol.c_str(), static_cast<int>(symbol.size()));
    if (!escapedSymbol) {
        curl_easy_cleanup(escapeHandle);
        return spec;
    }
    const std::string path = accountPath() + "/symbols/" + escapedSymbol + "/spec";
    curl_free(escapedSymbol);
    curl_easy_cleanup(escapeHandle);

    const auto response = request("GET", path, {}, timeoutMs);
    if (response.status < 200 || response.status >= 300) return spec;
    const std::string body = jsonObject(response.body, "spec");
    if (body.empty() || jsonValue(body, "name") != symbol) return spec;

    spec.digits = static_cast<int>(jsonNumber(body, "digits"));
    spec.point = jsonNumber(body, "point");
    spec.tickSize = jsonNumber(body, "tickSize");
    spec.contractSize = jsonNumber(body, "contractSize");
    spec.tickValue = jsonNumber(body, "tickValueAccount");
    if (spec.tickValue <= 0.0) spec.tickValue = jsonNumber(body, "tickValue");
    spec.volumeMin = jsonNumber(body, "volumeMin");
    spec.volumeMax = jsonNumber(body, "volumeMax");
    spec.volumeStep = jsonNumber(body, "volumeStep");
    spec.stopLevelPoints = jsonNumber(body, "stopLevel");
    spec.freezeLevelPoints = jsonNumber(body, "freezeLevel");
    spec.baseCurrency = jsonValue(body, "baseCurrency");
    spec.quoteCurrency = jsonValue(body, "profitCurrency");
    spec.detected = spec.digits > 0 && spec.point > 0.0 &&
                    spec.tickValue > 0.0 && spec.volumeMin > 0.0 &&
                    spec.volumeStep > 0.0;
    if (spec.detected)
        std::cout << "[TickerAll] " << spec.report() << "\n";
    return spec;
}

std::vector<OrderInfo> TickerallBridge::requestOrders(int timeoutMs) {
    std::vector<OrderInfo> orders;
    if (!discoverDemoAccount()) return orders;
    const auto account = request("GET", accountPath(), {}, timeoutMs);
    if (account.status < 200 || account.status >= 300) {
        if (errorCb_) errorCb_(static_cast<int>(account.status),
                               "Lecture du compte TickerAll échouée");
        return orders;
    }
    for (const auto& position : jsonObjects(account.body, "positions"))
        orders.push_back(parsePosition(position));

    const auto pending = request("GET", accountPath() + "/orders", {}, timeoutMs);
    if (pending.status < 200 || pending.status >= 300) {
        if (errorCb_) errorCb_(static_cast<int>(pending.status),
                               "Lecture des ordres en attente TickerAll échouée");
        return orders;
    }
    for (const auto& order : jsonObjects(pending.body, "orders"))
        orders.push_back(parsePendingOrder(order));
    return orders;
}

BridgeResponse TickerallBridge::sendCommand(const TradeCmd& cmd, int timeoutMs) {
    BridgeResponse result;
    if (!connected_.load()) {
        result.error = -1;
        result.msg = "Compte TickerAll non connecté ou non vérifié";
        return result;
    }

    std::string method;
    std::string path = accountPath();
    std::ostringstream body;
    body << std::setprecision(12);
    bool supported = true;

    switch (cmd.op) {
        case TradeCmd::Op::MarketOrder: {
            const std::string symbol = cmd.symbol.empty() ? cfg_.symbol : cmd.symbol;
            std::string type;
            std::string side;
            switch (cmd.type) {
                case OrderType::Buy: type = "market"; side = "BUY"; break;
                case OrderType::Sell: type = "market"; side = "SELL"; break;
                case OrderType::BuyStop: type = "stop"; side = "BUY"; break;
                case OrderType::SellStop: type = "stop"; side = "SELL"; break;
                case OrderType::BuyLimit: type = "limit"; side = "BUY"; break;
                case OrderType::SellLimit: type = "limit"; side = "SELL"; break;
                default: supported = false; break;
            }
            if (supported) {
                method = "POST";
                path += "/orders";
                body << "{\"type\":\"" << type << "\",\"symbol\":"
                     << jsonString(symbol) << ",\"side\":\"" << side
                     << "\",\"volume\":" << cmd.lots;
                if (type != "market") body << ",\"price\":" << cmd.price;
                if (cmd.sl > 0.0) body << ",\"stopLoss\":" << cmd.sl;
                if (cmd.tp > 0.0) body << ",\"takeProfit\":" << cmd.tp;
                if (!cmd.comment.empty())
                    body << ",\"comment\":" << jsonString(cmd.comment.substr(0, 31));
                body << '}';
            }
            break;
        }
        case TradeCmd::Op::CloseBy:
            supported = false;
            break;
        case TradeCmd::Op::CloseOrder:
            method = "DELETE";
            path += "/positions/" + resolveTicket(cmd.ticket);
            if (cmd.lots > 0.0) body << "{\"volume\":" << cmd.lots << '}';
            break;
        case TradeCmd::Op::DeletePending:
            method = "DELETE";
            path += "/orders/" + resolveTicket(cmd.ticket);
            break;
        case TradeCmd::Op::ModifyOrder:
            method = "PATCH";
            path += "/orders/" + resolveTicket(cmd.ticket);
            body << '{';
            if (cmd.price > 0.0) body << "\"price\":" << cmd.price;
            if (cmd.sl > 0.0) {
                if (body.tellp() > 1) body << ',';
                body << "\"stopLoss\":" << cmd.sl;
            }
            if (cmd.tp > 0.0) {
                if (body.tellp() > 1) body << ',';
                body << "\"takeProfit\":" << cmd.tp;
            }
            body << '}';
            break;
        case TradeCmd::Op::ModifyPosition:
            method = "PATCH";
            path += "/positions/" + resolveTicket(cmd.ticket);
            body << '{';
            if (cmd.sl > 0.0) body << "\"stopLoss\":" << cmd.sl;
            if (cmd.tp > 0.0) {
                if (body.tellp() > 1) body << ',';
                body << "\"takeProfit\":" << cmd.tp;
            }
            body << '}';
            break;
    }

    if (!supported || path.ends_with('/')) {
        result.error = -2;
        result.msg = supported ? "Ticket TickerAll inconnu" :
                                 "Commande non prise en charge par TickerAll";
        return result;
    }

    const std::string requestBody = body.str();
    const std::string fingerprint = method + '\n' + path + '\n' + requestBody;
    {
        std::lock_guard lock(uncertainWritesMutex_);
        if (uncertainWrites_.contains(fingerprint)) {
            result.error = -3;
            result.msg = "Résultat d'un ordre précédent incertain; "
                         "vérification manuelle du compte requise";
            return result;
        }
    }

    const auto response = request(method, path, requestBody, timeoutMs, true);
    result.error = static_cast<int>(response.status);
    result.success = response.status >= 200 && response.status < 300;
    result.msg = jsonValue(response.body, "message");
    result.orderId = jsonValue(response.body, "ticket");
    result.ticket = result.orderId.empty() ? cmd.ticket : registerTicket(result.orderId);
    result.price = jsonNumber(response.body, "price");
    if (response.status == 0 ||
        (response.status == 409 &&
         jsonValue(response.body, "code") == "TRADE_UNCONFIRMED")) {
        std::lock_guard lock(uncertainWritesMutex_);
        uncertainWrites_.insert(fingerprint);
        result.msg = "Résultat de l'ordre incertain; vérifiez les positions "
                     "et ordres avant toute reprise";
    }
    if (!result.success && result.msg.empty())
        result.msg = response.status == 0 ? "Échec de connexion TickerAll" :
                     "TickerAll HTTP " + std::to_string(response.status);
    return result;
}

bool TickerallBridge::connectStream() {
    AccountSnapshot account;
    if (!verifyDemoAccount(&account)) return false;

    CURL* stream = curl_easy_init();
    if (!stream) return false;
    struct curl_slist* headers = nullptr;
    const std::string auth = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(stream, CURLOPT_URL, "wss://api.tickerall.com/v1/stream");
    curl_easy_setopt(stream, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(stream, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(stream, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(stream, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(stream, CURLOPT_SSL_VERIFYHOST, 2L);
    const CURLcode connected = curl_easy_perform(stream);
    long httpStatus = 0;
    curl_easy_getinfo(stream, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_slist_free_all(headers);
    if (connected != CURLE_OK) {
        std::cerr << "[TickerAll] Échec handshake WebSocket: "
                  << curl_easy_strerror(connected)
                  << " (HTTP " << httpStatus << ")\n";
        curl_easy_cleanup(stream);
        return false;
    }

    std::ostringstream subscription;
    subscription << "{\"type\":\"subscribe\",\"channels\":["
                 << "{\"kind\":\"ticks\",\"accountId\":"
                 << jsonString(accountId_) << ",\"symbols\":"
                 << '[' << jsonString(cfg_.symbol) << ']'
                 << "}]}";
    size_t sent = 0;
    const std::string frame = subscription.str();
    const CURLcode sendStatus = curl_ws_send(
        stream, frame.data(), frame.size(), &sent, 0, CURLWS_TEXT);
    if (sendStatus != CURLE_OK || sent != frame.size()) {
        std::cerr << "[TickerAll] Échec abonnement WebSocket: "
                  << curl_easy_strerror(sendStatus) << "\n";
        curl_easy_cleanup(stream);
        return false;
    }

    {
        std::lock_guard lock(streamMutex_);
        stream_ = stream;
    }
    if (accountCb_) accountCb_(account);
    return true;
}

void TickerallBridge::closeStream() noexcept {
    std::lock_guard lock(streamMutex_);
    if (stream_) {
        curl_easy_cleanup(stream_);
        stream_ = nullptr;
    }
}

void TickerallBridge::start() {
    if (running_.exchange(true)) return;
    if (apiKey_.empty()) {
        std::cerr << "[TickerAll] TICKERALL_API_KEY requis — moteur désarmé\n";
        running_.store(false);
        return;
    }
    if (!discoverDemoAccount() || !verifyDemoAccount() ||
        !requestSymbolSpec(cfg_.symbol, 8000).detected) {
        std::cerr << "[TickerAll] Compte démo ou symbole non vérifié — "
                     "moteur désarmé\n";
        running_.store(false);
        return;
    }
    if (!connectStream()) {
        std::cerr << "[TickerAll] Flux temps réel indisponible — moteur désarmé\n";
        running_.store(false);
        return;
    }

    connected_.store(true);
    if (connectCb_) connectCb_(true);
    std::cout << "[TickerAll] MT5 démo vérifié — " << cfg_.symbol
              << " — flux temps réel actif\n";
    tickThread_ = std::jthread([this](std::stop_token) { tickLoop(); });
    stateThread_ = std::jthread([this](std::stop_token) { stateLoop(); });
}

void TickerallBridge::stop() {
    running_.store(false);
    if (tickThread_.joinable()) {
        tickThread_.request_stop();
        tickThread_.join();
    }
    if (stateThread_.joinable()) {
        stateThread_.request_stop();
        stateThread_.join();
    }
    closeStream();
    if (connected_.exchange(false) && connectCb_) connectCb_(false);
}

void TickerallBridge::tickLoop() {
    std::string message;
    std::array<char, 8192> buffer{};
    int reconnectDelay = std::max(1000, cfg_.reconnectMs);
    while (running_.load()) {
        {
            std::lock_guard lock(streamMutex_);
            if (!stream_) {
                // Connect outside the stream lock.
            } else {
                size_t received = 0;
                const curl_ws_frame* metadata = nullptr;
                const CURLcode status = curl_ws_recv(
                    stream_, buffer.data(), buffer.size(), &received, &metadata);
                if (status == CURLE_OK && metadata) {
                    message.append(buffer.data(), received);
                    if (metadata->bytesleft == 0) {
                        if ((metadata->flags & CURLWS_TEXT) != 0 &&
                            jsonValue(message, "type") == "tick" &&
                            jsonValue(message, "symbol") == cfg_.symbol) {
                            Tick tick;
                            tick.bid = jsonNumber(message, "bid");
                            tick.ask = jsonNumber(message, "ask");
                            tick.spread = tick.ask - tick.bid;
                            tick.localTime = Clock::now();
                            if (tick.bid > 0.0 && tick.ask >= tick.bid && tickCb_)
                                tickCb_(tick);
                        } else if (jsonValue(message, "type") == "error") {
                            if (errorCb_) errorCb_(-1, "Erreur flux TickerAll");
                        }
                        message.clear();
                    }
                } else if (status != CURLE_AGAIN) {
                    curl_easy_cleanup(stream_);
                    stream_ = nullptr;
                }
            }
        }

        bool needsReconnect = false;
        {
            std::lock_guard lock(streamMutex_);
            needsReconnect = stream_ == nullptr;
        }
        if (needsReconnect && running_.load()) {
            if (connected_.exchange(false) && connectCb_) connectCb_(false);
            closeStream();
            std::this_thread::sleep_for(Ms(reconnectDelay));
            if (!running_.load()) break;
            if (connectStream()) {
                connected_.store(true);
                if (connectCb_) connectCb_(true);
                reconnectDelay = std::max(1000, cfg_.reconnectMs);
            } else {
                reconnectDelay = std::min(reconnectDelay * 2, 30000);
                if (errorCb_) errorCb_(-1, "Reconnexion TickerAll échouée");
            }
        }
        std::this_thread::sleep_for(Ms(10));
    }
    closeStream();
}

void TickerallBridge::stateLoop() {
    const int interval = std::max(1000, cfg_.statePollMs);
    while (running_.load()) {
        std::this_thread::sleep_for(Ms(interval));
        if (!running_.load() || !connected_.load()) continue;

        AccountSnapshot account;
        if (!verifyDemoAccount(&account)) {
            if (connected_.exchange(false) && connectCb_) connectCb_(false);
            continue;
        }
        if (accountCb_) accountCb_(account);
        for (const auto& order : requestOrders(std::min(interval, 5000)))
            if (order.ticket >= 0 && orderCb_) orderCb_(order);
    }
}

} // namespace top4ea
