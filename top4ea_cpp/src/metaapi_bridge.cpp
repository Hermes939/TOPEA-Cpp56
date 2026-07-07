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

        // ── Détection plateforme (MT4 ou MT5) ────────────────
        // Applique la plateforme détectée sauf si la config force une valeur.
        if (cfg_.platform == PlatformType::Auto) {
            platform_.store(acc->platform);
        } else {
            platform_.store(cfg_.platform);  // surcharge manuelle
        }
        const char* platStr = (platform_.load() == PlatformType::MT5) ? "MT5"
                            : (platform_.load() == PlatformType::MT4) ? "MT4"
                            : "Auto";
        std::cout << "[MetaApi] Connecté — " << accountId_
                  << " | Balance=" << acc->balance << " " << acc->currency
                  << " | Platform=" << platStr << "\n";
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
    if (tickThread_.joinable()) {
        tickThread_.request_stop();
        tickThread_.join();
    }
    if (stateThread_.joinable()) {
        stateThread_.request_stop();
        stateThread_.join();
    }
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
//
//  FIX : Retry avec backoff linéaire pour les commandes critiques
//  (CloseOrder, CloseBy, DeletePending, ModifyOrder).
//  Stratégie : 3 tentatives, délai 200ms → 400ms → 600ms.
//  Les ordres d'ouverture (MarketOrder, BuyStop, SellStop) ne sont
//  PAS retentés pour éviter la duplication de positions.
// ────────────────────────────────────────────────────────────
BridgeResponse MetaApiBridge::sendCommand(const TradeCmd& cmd, int /*timeoutMs*/) {
    std::string body = cmdToJson(cmd);
    std::string path = "/users/current/accounts/" + accountId_ + "/trade";

    // Les commandes critiques (fermeture/modification) bénéficient du retry
    bool isCritical = (cmd.op == TradeCmd::Op::CloseOrder  ||
                       cmd.op == TradeCmd::Op::CloseBy     ||
                       cmd.op == TradeCmd::Op::DeletePending||
                       cmd.op == TradeCmd::Op::ModifyOrder);

    static constexpr int MAX_RETRIES   = 3;
    static constexpr int BACKOFF_MS    = 200;  // délai de base (linéaire)

    int attempts = isCritical ? MAX_RETRIES : 1;

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        std::string resp = httpPost(path, body);

        if (!resp.empty()) {
            BridgeResponse r = parseTradeResult(resp);
            if (r.success || !isCritical) return r;
            // Échec applicatif sur commande critique → retry
            std::cerr << "[MetaApi] sendCommand échoué tentative " << attempt
                      << "/" << attempts << " : " << r.msg << "\n";
        } else {
            std::cerr << "[MetaApi] sendCommand pas de réponse tentative " << attempt
                      << "/" << attempts << "\n";
        }

        if (attempt < attempts) {
            std::this_thread::sleep_for(Ms(BACKOFF_MS * attempt));
        }
    }

    BridgeResponse r;
    r.success = false;
    r.error   = -1;
    r.msg     = "Pas de réponse MetaApi après " + std::to_string(attempts) + " tentatives";
    return r;
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

// ============================================================
//  requestSymbolSpec — Auto-détection spécifications broker
//  Endpoint : GET /users/current/accounts/{id}/symbols/{sym}
//  Peut être appelé AVANT start() (appel synchrone HTTP brut)
//
//  Champs extraits :
//    digits, point, tickValue, tickSize, contractSize,
//    volumeMin, volumeMax, volumeStep, stopsLevel, spread
//
//  Fallback intelligent :
//    Si digits absent mais tickSize présent → digits=round(-log10(tickSize))
//    Si tickValue absent → estimé depuis contractSize et point
//    Si tout échoue → detected=false, moteur utilise config fichier
// ============================================================
SymbolSpec MetaApiBridge::requestSymbolSpec(const std::string& symbol,
                                             int /*timeoutMs*/) {
    SymbolSpec spec;
    spec.symbol   = symbol;
    spec.detected = false;

    if (token_.empty() || accountId_.empty()) {
        std::cerr << "[BrokerSpec] Tokens manquants — impossible de détecter "
                     "les specs\n";
        return spec;
    }

    std::string path = "/users/current/accounts/" + accountId_
                     + "/symbols/" + symbol;
    std::cout << "[BrokerSpec] Interrogation broker → " << path << "\n";

    std::string resp = httpGet(path);
    if (resp.empty()) {
        std::cerr << "[BrokerSpec] Pas de réponse broker (weekend/offline?)"
                     " — config fichier conservée\n";
        return spec;
    }

    spec = parseSymbolSpec(resp, symbol);

    if (spec.detected) {
        std::cout << "[BrokerSpec] " << spec.report() << "\n";
    } else {
        std::cerr << "[BrokerSpec] Données insuffisantes dans la réponse"
                     " — config fichier conservée\n";
    }
    return spec;
}

// ────────────────────────────────────────────────────────────
//  parseSymbolSpec — extraction JSON MetaApi symbol spec
//
//  MetaApi retourne un objet JSON avec les champs suivants
//  (noms documentés v5 MetaApi REST) :
//    "digits", "point", "tickValue", "tickSize",
//    "contractSize", "volumeMin", "volumeMax", "volumeStep",
//    "stopsLevel", "freezeLevel", "spread",
//    "description", "baseCurrency", "quoteCurrency"
// ────────────────────────────────────────────────────────────
SymbolSpec MetaApiBridge::parseSymbolSpec(const std::string& json,
                                           const std::string& sym) {
    SymbolSpec s;
    s.symbol = sym;

    // ── Précision ────────────────────────────────────────────
    int rawDigits = jsonGetI(json, "digits");

    // tickSize est souvent plus fiable (ex: "0.001" → 3 digits)
    double tickSize = jsonGetD(json, "tickSize");
    if (tickSize <= 0.0) tickSize = jsonGetD(json, "tickvalue"); // fallback
    if (tickSize <= 0.0) tickSize = jsonGetD(json, "tick_size");

    if (rawDigits > 0) {
        s.digits   = rawDigits;
        s.point    = std::pow(10.0, -rawDigits);
    } else if (tickSize > 0.0) {
        // Déduire digits depuis tickSize
        double log10ts = -std::log10(tickSize);
        s.digits   = std::max(1, (int)std::round(log10ts));
        s.point    = std::pow(10.0, -s.digits);
        std::cout << "[BrokerSpec] digits déduit depuis tickSize="
                  << tickSize << " → " << s.digits << "\n";
    } else {
        // Impossible de déterminer — ne pas marquer detected
        std::cerr << "[BrokerSpec] digits/tickSize absents du JSON\n";
        return s;
    }
    s.tickSize = (tickSize > 0.0) ? tickSize : s.point;

    // ── Point (surcharge si fourni directement) ──────────────
    double rawPoint = jsonGetD(json, "point");
    if (rawPoint > 0.0) s.point = rawPoint;

    // ── Tick value ───────────────────────────────────────────
    // Cas nominal : MetaApi fournit tickValue directement
    double tv = jsonGetD(json, "tickValue");
    if (tv <= 0.0) tv = jsonGetD(json, "tick_value");

    // Cas alternatif : calculer depuis contractSize × point
    // Pour XAU/USD : 1 lot = 100oz → tickValue = 100 × point $
    double cs = jsonGetD(json, "contractSize");
    if (cs <= 0.0) cs = jsonGetD(json, "contract_size");

    if (tv > 0.0) {
        s.tickValue    = tv;
        s.contractSize = (cs > 0.0) ? cs : 100.0;
    } else if (cs > 0.0) {
        s.tickValue    = cs * s.point;
        s.contractSize = cs;
        std::cout << "[BrokerSpec] tickValue calculé: "
                  << cs << " × " << s.point << " = " << s.tickValue << "\n";
    } else {
        // Dernier recours : or standard 100oz
        s.contractSize = 100.0;
        s.tickValue    = 100.0 * s.point;
        std::cout << "[BrokerSpec] tickValue estimé (100oz standard): "
                  << s.tickValue << "\n";
    }

    // ── Volumes ──────────────────────────────────────────────
    double vmin  = jsonGetD(json, "volumeMin");
    double vmax  = jsonGetD(json, "volumeMax");
    double vstep = jsonGetD(json, "volumeStep");
    if (vmin  <= 0.0) vmin  = jsonGetD(json, "minVolume");
    if (vmax  <= 0.0) vmax  = jsonGetD(json, "maxVolume");
    if (vstep <= 0.0) vstep = jsonGetD(json, "volumeIncrement");

    s.volumeMin  = (vmin  > 0.0) ? vmin  : 0.01;
    s.volumeMax  = (vmax  > 0.0) ? vmax  : 100.0;
    s.volumeStep = (vstep > 0.0) ? vstep : 0.01;

    // ── Distances minimales ──────────────────────────────────
    double sl = jsonGetD(json, "stopsLevel");
    if (sl <= 0.0) sl = jsonGetD(json, "stopLevel");
    if (sl <= 0.0) sl = jsonGetD(json, "stops_level");
    s.stopLevelPoints  = sl;

    double fl = jsonGetD(json, "freezeLevel");
    if (fl <= 0.0) fl = jsonGetD(json, "freeze_level");
    s.freezeLevelPoints = fl;

    // ── Spread ───────────────────────────────────────────────
    double sp = jsonGetD(json, "spread");
    s.spreadPoints = sp;

    // ── Description & identifiants ───────────────────────────
    s.description   = jsonGet(json, "description");
    s.baseCurrency  = jsonGet(json, "baseCurrency");
    s.quoteCurrency = jsonGet(json, "quoteCurrency");

    // Broker name parfois dans le JSON de spec
    std::string bn = jsonGet(json, "brokerName");
    if (bn.empty()) bn = jsonGet(json, "broker");
    s.brokerName = bn;

    // ── Validation finale ────────────────────────────────────
    if (s.digits > 0 && s.point > 0.0 && s.tickValue > 0.0) {
        s.detected = true;
    }
    return s;
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
    if (httpCode == 429) {
        std::cerr << "[MetaApi] GET HTTP 429 (rate-limit) — attente 5s avant retry\n";
        std::this_thread::sleep_for(std::chrono::seconds(5));
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
//
//  POINT CRITIQUE : la précision des prix doit correspondre
//  exactement au nombre de décimales du broker.
//  Ancienne version : setprecision(2) hardcodé → CASSÉ pour
//  Exness XAUUSD 3 digits (prix tronqué = rejet broker).
//  Nouvelle version : cfg_.digits détecté dynamiquement.
//    Exness 3d → setprecision(3)  ex: 1.923
//    IC Markets 2d → setprecision(2)  ex: 1923.45
//    Forex 5d → setprecision(5)  ex: 1.23456
// ────────────────────────────────────────────────────────────
std::string MetaApiBridge::cmdToJson(const TradeCmd& cmd) const {
    // Précision prix = digits broker (défaut sécurisé : 5 si non détecté)
    int pricePrecision = (cfg_.digits > 0) ? cfg_.digits : 5;

    // Plateforme effective : config ou détectée
    const bool mt5 = (platform_.load() == PlatformType::MT5)
                  || (platform_.load() == PlatformType::Auto
                      && cfg_.platform == PlatformType::MT5);

    std::ostringstream oss;
    oss << std::fixed;

    // ── Formateurs ───────────────────────────────────────────
    auto priceFmt = [&](double price) -> std::string {
        std::ostringstream tmp;
        tmp << std::fixed << std::setprecision(pricePrecision) << price;
        return tmp.str();
    };
    auto lotFmt = [&](double lots) -> std::string {
        std::ostringstream tmp;
        tmp << std::fixed << std::setprecision(2) << lots;
        return tmp.str();
    };
    // MT5 limite les commentaires à 31 caractères
    auto commentFmt = [&](const std::string& c) -> std::string {
        return mt5 ? c.substr(0, std::min((int)c.size(), 31)) : c;
    };

    // ── Type d'action MetaApi ────────────────────────────────
    auto actionType = [&]() -> std::string {
        if (cmd.op == TradeCmd::Op::DeletePending)  return "ORDER_CANCEL";
        if (cmd.op == TradeCmd::Op::CloseOrder)     return "POSITION_CLOSE_ID";
        if (cmd.op == TradeCmd::Op::CloseBy)        return "POSITION_CLOSE_ID";
        if (cmd.op == TradeCmd::Op::ModifyOrder)    return "ORDER_MODIFY";
        if (cmd.op == TradeCmd::Op::ModifyPosition) {
            // MT5 : POSITION_MODIFY pour modifier SL/TP d'une position ouverte
            // MT4 : ORDER_MODIFY avec positionId fonctionne aussi via MetaApi
            return mt5 ? "POSITION_MODIFY" : "ORDER_MODIFY";
        }
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
        oss << ",\"symbol\":\""   << sym              << "\"";
        oss << ",\"volume\":"     << lotFmt(cmd.lots);
        if (cmd.price > 0.0)
            oss << ",\"openPrice\":" << priceFmt(cmd.price);
        if (cmd.sl > 0.0)
            oss << ",\"stopLoss\":"   << priceFmt(cmd.sl);
        if (cmd.tp > 0.0)
            oss << ",\"takeProfit\":" << priceFmt(cmd.tp);
        oss << ",\"comment\":\""  << commentFmt(cmd.comment) << "\"";
        oss << ",\"magic\":"      << cmd.magic;

        // deviation / slippage + fillingMode :
        // UNIQUEMENT pour les ordres au MARCHÉ (Buy/Sell directs).
        // Les ordres PENDING (BuyStop, SellStop, BuyLimit, SellLimit)
        // ne les acceptent pas — MetaApi retourne HTTP 400 si présents.
        const bool isMarketExec = (cmd.type == OrderType::Buy ||
                                   cmd.type == OrderType::Sell);
        if (isMarketExec) {
            if (mt5) {
                // MT5 : "deviation" en points (≠ MT4 "slippage" en ticks)
                // + fillingMode requis par certains brokers MT5
                oss << ",\"deviation\":"   << cmd.slippage;
                oss << ",\"fillingMode\":\"ORDER_FILLING_FOK\"";
            } else {
                // MT4 : "slippage" en ticks
                oss << ",\"slippage\":"    << cmd.slippage;
            }
        }

    } else if (cmd.op == TradeCmd::Op::CloseOrder ||
               cmd.op == TradeCmd::Op::CloseBy) {
        // Résolution ID MetaApi réel (UUID ou numérique) depuis la table idMap_
        oss << ",\"positionId\":\"" << resolveId(cmd.ticket) << "\"";

    } else if (cmd.op == TradeCmd::Op::DeletePending) {
        oss << ",\"orderId\":\"" << resolveId(cmd.ticket) << "\"";

    } else if (cmd.op == TradeCmd::Op::ModifyOrder) {
        // Ordres pending (SellStop / BuyStop) → orderId + openPrice obligatoire
        // Précision broker correcte — CRITIQUE pour les ordres Exness 3-digit
        oss << ",\"orderId\":\"" << resolveId(cmd.ticket) << "\"";
        if (cmd.price > 0.0)
            oss << ",\"openPrice\":" << priceFmt(cmd.price);
        if (cmd.sl > 0.0)
            oss << ",\"stopLoss\":"   << priceFmt(cmd.sl);
        if (cmd.tp > 0.0)
            oss << ",\"takeProfit\":" << priceFmt(cmd.tp);

    } else if (cmd.op == TradeCmd::Op::ModifyPosition) {
        // Modification SL/TP d'une position ouverte
        // MT5 : POSITION_MODIFY + positionId
        // MT4 : ORDER_MODIFY + positionId (MetaApi accepte les deux)
        oss << ",\"positionId\":\"" << resolveId(cmd.ticket) << "\"";
        if (cmd.sl > 0.0)
            oss << ",\"stopLoss\":"   << priceFmt(cmd.sl);
        if (cmd.tp > 0.0)
            oss << ",\"takeProfit\":" << priceFmt(cmd.tp);
    }

    oss << "}";
    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  Table de correspondance ID MetaApi ↔ int synthétique
//
//  MetaApi retourne des IDs UUID-like (ex: "8f3a1b2c-...") ou
//  des IDs numériques formatés en string ("12345678").
//  stoi() échoue sur les UUIDs → ticket = -1 → boucle infinie.
//
//  registerOrFindId() garantit :
//   - Si l'ID est déjà connu, retourne le même int synthétique
//   - Sinon génère un nouvel int unique ≥ 1'000'001
//   - La table inverse permet de résoudre le vrai ID MetaApi
//     lors des appels modify/delete (cmdToJson → orderId/positionId)
// ────────────────────────────────────────────────────────────
int MetaApiBridge::registerOrFindId(const std::string& realId) {
    if (realId.empty()) return -1;

    // Essai d'abord une conversion directe (ID purement numérique)
    try {
        int numId = std::stoi(realId);
        if (numId > 0) {
            std::lock_guard lk(idMapMu_);
            idMap_[numId] = realId;
            return numId;
        }
    } catch (...) {}

    // UUID / alphanumerique → chercher dans la table existante
    {
        std::lock_guard lk(idMapMu_);
        for (const auto& [k, v] : idMap_) {
            if (v == realId) return k;
        }
        // Générer un nouvel int synthétique unique
        int synth = ++syntheticCounter_;
        idMap_[synth] = realId;
        return synth;
    }
}

std::string MetaApiBridge::resolveId(int syntheticTicket) const {
    if (syntheticTicket <= 0) return std::to_string(syntheticTicket);
    std::lock_guard lk(idMapMu_);
    auto it = idMap_.find(syntheticTicket);
    return (it != idMap_.end()) ? it->second : std::to_string(syntheticTicket);
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
    o.id     = jsonGet(j, "id");
    o.ticket = registerOrFindId(o.id);   // int synthétique stable ≥ 1

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
    o.id     = jsonGet(j, "id");
    o.ticket = registerOrFindId(o.id);   // int synthétique stable ≥ 1

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

    // ── Détection plateforme MT4 / MT5 ───────────────────────
    // MetaApi retourne "platform": "mt4" ou "platform": "mt5"
    // dans accountInformation. On mappe ici pour adapter le comportement.
    std::string plat = jsonGet(j, "platform");
    if      (plat == "mt5" || plat == "MT5") a.platform = PlatformType::MT5;
    else if (plat == "mt4" || plat == "MT4") a.platform = PlatformType::MT4;
    else                                      a.platform = PlatformType::Auto;
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

    // MetaApi peut retourner un orderId (pending) ou positionId (market)
    // Ces IDs peuvent être des UUIDs impossibles à convertir via stoi().
    // registerOrFindId() garantit un int synthétique positif stable.
    std::string ordId = jsonGet(j, "orderId");
    std::string posId = jsonGet(j, "positionId");
    std::string idStr = posId.empty() ? ordId : posId;

    r.orderId = idStr;
    r.ticket  = registerOrFindId(idStr);  // ≥ 1 si ID valide, -1 si vide

    // Diagnostic : afficher correspondance UUID ↔ int synthétique
    if (!idStr.empty() && r.ticket > 0) {
        std::cout << "[ID] " << idStr
                  << " → ticket synthétique #" << r.ticket << "\n";
    }

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
