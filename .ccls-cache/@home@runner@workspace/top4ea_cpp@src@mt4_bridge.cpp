// ============================================================
//  TOP4EA — mt4_bridge.cpp
//  Implémentation TCP bridge vers MT4
//  Protocole JSON-ligne (newline-delimited)
// ============================================================
#include "../include/mt4_bridge.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cassert>
#include <charconv>
#include <algorithm>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/select.h>
#  include <errno.h>
#endif

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ────────────────────────────────────────────────────────────
MT4Bridge::MT4Bridge(const Config& cfg) : cfg_(cfg) {
    socketInit();
}

MT4Bridge::~MT4Bridge() {
    stop();
    socketCleanup();
}

// ────────────────────────────────────────────────────────────
//  start / stop
// ────────────────────────────────────────────────────────────
void MT4Bridge::start() {
    running_.store(true);
    ioThread_   = std::jthread([this](std::stop_token st){ ioLoop(); });
    pingThread_ = std::jthread([this](std::stop_token st){ pingLoop(); });
}

void MT4Bridge::stop() {
    running_.store(false);
    if (ioThread_.joinable())   ioThread_.request_stop();
    if (pingThread_.joinable()) pingThread_.request_stop();
    disconnect();
}

// ────────────────────────────────────────────────────────────
//  I/O Loop principal
// ────────────────────────────────────────────────────────────
void MT4Bridge::ioLoop() {
    while (running_.load()) {
        if (!isConnected()) {
            if (!tryConnect()) {
                std::this_thread::sleep_for(Ms(cfg_.reconnectMs));
                continue;
            }
        }

        std::string line;
        if (readLine(line, 100)) {
            if (!line.empty())
                processLine(line);
        }

        // Envoyer commandes en attente
        {
            std::unique_lock lk(cmdMu_);
            while (!cmdQueue_.empty()) {
                PendingCmd* pc = cmdQueue_.front();
                cmdQueue_.pop();
                lk.unlock();

                if (writeLine(pc->json)) {
                    std::string resp;
                    if (readLine(resp, 3000)) {
                        pc->prom.set_value(parseResp(resp));
                    } else {
                        BridgeResponse r;
                        r.success = false;
                        r.error   = -1;
                        r.msg     = "timeout";
                        pc->prom.set_value(r);
                    }
                } else {
                    BridgeResponse r;
                    r.success = false;
                    r.error   = -2;
                    r.msg     = "write failed";
                    pc->prom.set_value(r);
                    disconnect();
                }
                delete pc;
                lk.lock();
            }
        }
    }
}

// ────────────────────────────────────────────────────────────
//  Ping loop (maintien connexion)
// ────────────────────────────────────────────────────────────
void MT4Bridge::pingLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(Ms(cfg_.pingIntervalMs));
        if (isConnected()) {
            writeLine(R"({"cmd":"ping"})");
        }
    }
}

// ────────────────────────────────────────────────────────────
//  Connexion TCP
// ────────────────────────────────────────────────────────────
bool MT4Bridge::tryConnect() {
    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == SOCK_INVALID) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.bridgePort);
    ::inet_pton(AF_INET, cfg_.bridgeHost.c_str(), &addr.sin_addr);

    if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        socketClose(sock_);
        return false;
    }
    setNonBlocking(sock_, true);
    connected_.store(true);
    readBuf_.clear();
    if (connectCb_) connectCb_(true);
    std::cout << "[MT4Bridge] Connecté à " << cfg_.bridgeHost
              << ":" << cfg_.bridgePort << "\n";
    return true;
}

void MT4Bridge::disconnect() {
    if (connected_.exchange(false)) {
        socketClose(sock_);
        if (connectCb_) connectCb_(false);
        std::cout << "[MT4Bridge] Déconnecté\n";
    }
}

// ────────────────────────────────────────────────────────────
//  Lecture / Écriture réseau
// ────────────────────────────────────────────────────────────
bool MT4Bridge::writeLine(const std::string& line) {
    if (!isConnected()) return false;
    std::string msg = line + "\n";
    int total = 0;
    while (total < (int)msg.size()) {
        int n = ::send(sock_, msg.c_str() + total,
                       (int)msg.size() - total, 0);
        if (n <= 0) { disconnect(); return false; }
        total += n;
    }
    return true;
}

bool MT4Bridge::readLine(std::string& out, int timeoutMs) {
    auto deadline = Clock::now() + Ms(timeoutMs);
    char buf[4096];

    while (Clock::now() < deadline) {
        // Vérifier buffer existant
        auto pos = readBuf_.find('\n');
        if (pos != std::string::npos) {
            out = readBuf_.substr(0, pos);
            readBuf_.erase(0, pos + 1);
            return true;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_, &fds);
        timeval tv{};
        tv.tv_usec = 10000; // 10ms
        int sel = ::select((int)sock_+1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        int n = ::recv(sock_, buf, sizeof(buf)-1, 0);
        if (n <= 0) { disconnect(); return false; }
        readBuf_.append(buf, n);
    }
    return false;
}

// ────────────────────────────────────────────────────────────
//  Traitement d'un message JSON reçu
// ────────────────────────────────────────────────────────────
void MT4Bridge::processLine(const std::string& line) {
    std::string type = jsonGet(line, "type");

    if (type == "tick") {
        if (tickCb_) tickCb_(parseTick(line));
    } else if (type == "order") {
        if (orderCb_) orderCb_(parseOrder(line));
    } else if (type == "account") {
        if (accountCb_) accountCb_(parseAccount(line));
    } else if (type == "error") {
        if (errorCb_) errorCb_(jsonGetI(line, "code"), jsonGet(line, "msg"));
    } else if (type == "pong") {
        // keepalive OK
    }
}

// ────────────────────────────────────────────────────────────
//  Envoi de commande de trading (synchrone)
// ────────────────────────────────────────────────────────────
BridgeResponse MT4Bridge::sendCommand(const TradeCmd& cmd, int timeoutMs) {
    auto* pc = new PendingCmd();
    pc->json = cmdToJson(cmd);
    auto fut = pc->prom.get_future();

    {
        std::lock_guard lk(cmdMu_);
        cmdQueue_.push(pc);
    }
    cmdCv_.notify_one();

    if (fut.wait_for(Ms(timeoutMs + 500)) == std::future_status::ready)
        return fut.get();

    BridgeResponse r;
    r.success = false;
    r.error   = -3;
    r.msg     = "timeout sendCommand";
    return r;
}

// ────────────────────────────────────────────────────────────
//  Requêtes bloquantes
// ────────────────────────────────────────────────────────────
std::vector<OrderInfo> MT4Bridge::requestOrders(int timeoutMs) {
    writeLine(R"({"cmd":"get_orders"})");
    std::string resp;
    if (!readLine(resp, timeoutMs)) return {};
    // Le bridge retourne un tableau JSON simplifié
    // Format: {"type":"orders","data":[{...},{...}]}
    std::vector<OrderInfo> result;
    std::string data = jsonGet(resp, "data");
    // Parsing simplifié : extraction d'objets entre accolades
    size_t pos = 0;
    while ((pos = data.find('{', pos)) != std::string::npos) {
        size_t end = data.find('}', pos);
        if (end == std::string::npos) break;
        result.push_back(parseOrder(data.substr(pos, end-pos+1)));
        pos = end + 1;
    }
    return result;
}

std::optional<AccountSnapshot> MT4Bridge::requestAccount(int timeoutMs) {
    writeLine(R"({"cmd":"get_account"})");
    std::string resp;
    if (!readLine(resp, timeoutMs)) return std::nullopt;
    return parseAccount(resp);
}

// ────────────────────────────────────────────────────────────
//  Sérialisation / Désérialisation JSON (minimal, sans lib)
// ────────────────────────────────────────────────────────────
std::string MT4Bridge::cmdToJson(const TradeCmd& cmd) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(5);
    oss << R"({"cmd":"trade")";
    oss << R"(,"op":)"    << (int)cmd.op;
    oss << R"(,"type":)"  << (int)cmd.type;
    oss << R"(,"ticket":)"<< cmd.ticket;
    oss << R"(,"t2":)"    << cmd.ticket2;
    oss << R"(,"lots":)"  << cmd.lots;
    oss << R"(,"price":)" << cmd.price;
    oss << R"(,"sl":)"    << cmd.sl;
    oss << R"(,"tp":)"    << cmd.tp;
    oss << R"(,"slip":)"  << cmd.slippage;
    oss << R"(,"magic":)" << cmd.magic;
    oss << R"(,"sym":")"  << cmd.symbol  << '"';
    oss << R"(,"cmt":")"  << cmd.comment << '"';
    oss << "}";
    return oss.str();
}

Tick MT4Bridge::parseTick(const std::string& j) {
    Tick t;
    t.bid       = jsonGetD(j, "bid");
    t.ask       = jsonGetD(j, "ask");
    t.spread    = t.ask - t.bid;
    t.localTime = Clock::now();
    return t;
}

OrderInfo MT4Bridge::parseOrder(const std::string& j) {
    OrderInfo o;
    o.ticket     = jsonGetI(j, "ticket");
    o.type       = (OrderType)jsonGetI(j, "type");
    o.lots       = jsonGetD(j, "lots");
    o.openPrice  = jsonGetD(j, "open");
    o.sl         = jsonGetD(j, "sl");
    o.tp         = jsonGetD(j, "tp");
    o.profit     = jsonGetD(j, "profit");
    o.swap       = jsonGetD(j, "swap");
    o.commission = jsonGetD(j, "commission");
    o.comment    = jsonGet (j, "comment");
    return o;
}

AccountSnapshot MT4Bridge::parseAccount(const std::string& j) {
    AccountSnapshot a;
    a.balance    = jsonGetD(j, "balance");
    a.equity     = jsonGetD(j, "equity");
    a.freeMargin = jsonGetD(j, "free_margin");
    a.marginLevel= jsonGetD(j, "margin_level");
    a.marginReq  = jsonGetD(j, "margin_req");
    a.leverage   = jsonGetI(j, "leverage");
    a.currency   = jsonGet (j, "currency");
    return a;
}

BridgeResponse MT4Bridge::parseResp(const std::string& j) {
    BridgeResponse r;
    r.success = jsonGetB(j, "ok");
    r.ticket  = jsonGetI(j, "ticket");
    r.error   = jsonGetI(j, "error");
    r.price   = jsonGetD(j, "price");
    r.msg     = jsonGet (j, "msg");
    return r;
}

// ────────────────────────────────────────────────────────────
//  Extracteur JSON minimal (clé:valeur plat, pas d'imbrication)
// ────────────────────────────────────────────────────────────
std::string MT4Bridge::jsonGet(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;

    if (json[pos] == '"') {
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    }
    auto end = json.find_first_of(",}", pos);
    return (end != std::string::npos) ? json.substr(pos, end-pos) : json.substr(pos);
}

double MT4Bridge::jsonGetD(const std::string& j, const std::string& k) {
    auto s = jsonGet(j, k);
    if (s.empty()) return 0.0;
    try { return std::stod(s); } catch(...) { return 0.0; }
}

int MT4Bridge::jsonGetI(const std::string& j, const std::string& k) {
    auto s = jsonGet(j, k);
    if (s.empty()) return 0;
    try { return std::stoi(s); } catch(...) { return 0; }
}

bool MT4Bridge::jsonGetB(const std::string& j, const std::string& k) {
    auto s = jsonGet(j, k);
    return (s == "true" || s == "1");
}

} // namespace top4ea
