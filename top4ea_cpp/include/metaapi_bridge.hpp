#pragma once
// ============================================================
//  TOP4EA — metaapi_bridge.hpp  C++20
//  Connexion MetaApi REST (libcurl)
//  Auth    : env METAAPI_TOKEN
//  Account : env METAAPI_ACCOUNT_ID
//  Region  : env METAAPI_REGION (défaut: new-york)
//  Ticks   : REST polling /symbols/{sym}/currentPrice
//  Ordres  : REST polling /positions + /orders
//  Trades  : REST POST /trade
//  Specs   : REST /symbols/{sym} — auto-adaptation broker
// ============================================================
#include "types.hpp"
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace top4ea {

// ============================================================
//  SymbolSpec — Spécifications complètes du symbole broker
//  Détectées dynamiquement via MetaApi /symbols/{sym}
//  Utilisées pour adapter digits, point, tickValue à N'IMPORTE
//  quel broker (Exness 3d, IC Markets 2d, FXTM 2d, etc.)
// ============================================================
struct SymbolSpec {
    // ── Identification ──────────────────────────────────────
    std::string symbol        = {};
    std::string description   = {};
    std::string brokerName    = {};    // nom du broker (si disponible)
    std::string baseCurrency  = {};    // "XAU" pour l'or
    std::string quoteCurrency = {};    // "USD"

    // ── Précision prix ───────────────────────────────────────
    int    digits             = 0;     // décimales : 2 → XAUUSDm IC/FXTM
                                       //             3 → XAUUSDm Exness
                                       //             5 → EURUSD 5-digit
    double point              = 0.0;   // 1 point = pow(10, -digits)

    // ── Valeur du tick ───────────────────────────────────────
    // Critique pour sizing SRH, CPPI, VABT
    // Or : contractSize=100oz × point
    //   2d : 100 × 0.01 = 1.00 $ / lot / point (pour mini lot = 0.10 $/lot/pt)
    //   3d : 100 × 0.001 = 0.10 $ / lot / point
    double tickValue          = 0.0;   // $ par lot par point
    double tickSize           = 0.0;   // incrément minimum de prix (= point)

    // ── Contrat ──────────────────────────────────────────────
    double contractSize       = 0.0;   // oz pour XAU (100), unités pour forex
    double marginRate         = 0.0;   // marge requise / lot (fraction)

    // ── Volumes ──────────────────────────────────────────────
    double volumeMin          = 0.01;
    double volumeMax          = 100.0;
    double volumeStep         = 0.01;

    // ── Distances minimales ──────────────────────────────────
    double stopLevelPoints    = 0.0;   // distance min SL/TP en points
    double freezeLevelPoints  = 0.0;   // freeze level

    // ── Spread ───────────────────────────────────────────────
    double spreadPoints       = 0.0;   // spread courant en points

    // ── État détection ───────────────────────────────────────
    bool   detected           = false; // true si données reçues du broker

    // ── Rapport lisible ─────────────────────────────────────
    [[nodiscard]] std::string report() const {
        if (!detected) return "BrokerSpec: non détecté — config fichier";
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(digits > 0 ? digits : 2);
        oss << "BrokerSpec [" << symbol << "]"
            << "  digits=" << digits
            << "  point="  << point
            << "  tickVal=" << tickValue << "$/lot/pt"
            << "  contract=" << (int)contractSize
            << "  vol[" << volumeMin << "/" << volumeMax << "/" << volumeStep << "]"
            << "  stopLvl=" << (int)stopLevelPoints << "pts";
        return oss.str();
    }

    // ── ATR grid min/max adaptatifs ──────────────────────────
    // Les valeurs en points de la config (200/1000) sont définies
    // pour un broker 2-digit. On rescale si le broker diffère.
    [[nodiscard]] int rescalePoints(int configPoints, int refDigits = 2) const {
        if (!detected || digits == refDigits || digits == 0) return configPoints;
        // Exemple : configPoints=500 pts @ 2-digit = 5.00$ de déplacement
        // Pour 3-digit : 500 pts @ 3-digit = 0.500$ → besoin de 5000 pts
        double scale = std::pow(10.0, digits - refDigits);
        return (int)std::round(configPoints * scale);
    }
};

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
    Op          op       = Op::MarketOrder;
    OrderType   type     = OrderType::Buy;
    int         ticket   = -1;
    int         ticket2  = -1;
    double      lots     = 0.0;
    double      price    = 0.0;
    double      sl       = 0.0;
    double      tp       = 0.0;
    int         slippage = 3;
    int         magic    = 0;
    std::string comment  = {};
    std::string symbol   = {};
};

struct BridgeResponse {
    bool        success = false;
    int         ticket  = -1;   // int synthétique stable (peut être un hash)
    std::string orderId = {};   // ID MetaApi brut — utilisé pour modify/delete
    int         error   = 0;
    double      price   = 0.0;
    std::string msg     = {};
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

    [[nodiscard]] std::vector<OrderInfo>         requestOrders (int timeoutMs = 3000);
    [[nodiscard]] std::optional<AccountSnapshot> requestAccount(int timeoutMs = 3000);

    // ── Auto-détection broker (appelable AVANT start()) ──────
    // Interroge /symbols/{symbol} pour obtenir :
    //   digits, point, tickValue, contractSize, volume limits, stopLevel
    // Retourne un SymbolSpec avec detected=true si succès.
    // Retourne detected=false si le broker ne répond pas (weekend, etc.)
    // → Dans ce cas le moteur conserve les valeurs du fichier config.
    [[nodiscard]] SymbolSpec requestSymbolSpec(const std::string& symbol,
                                               int timeoutMs = 8000);

    // ── Met à jour la config interne (digits → precision JSON) ─
    void updateConfig(const Config& cfg) noexcept { cfg_ = cfg; }

private:
    void tickLoop();
    void stateLoop();

    std::string httpGet (const std::string& path);
    std::string httpPost(const std::string& path, const std::string& body);

    static std::string jsonGet    (const std::string& json, const std::string& key);
    static double      jsonGetD   (const std::string& json, const std::string& key);
    static int         jsonGetI   (const std::string& json, const std::string& key);
    static bool        jsonGetB   (const std::string& json, const std::string& key);
    static std::string jsonGetNested(const std::string& json, const std::string& key);

    // ── Parseurs (non-static pour accéder à idMap_) ──────────
    static Tick            parseTick        (const std::string& json);
    OrderInfo              parsePosition    (const std::string& json);
    OrderInfo              parsePendingOrder(const std::string& json);
    static AccountSnapshot parseAccount     (const std::string& json);
    BridgeResponse         parseTradeResult (const std::string& json);
    static SymbolSpec      parseSymbolSpec  (const std::string& json,
                                             const std::string& symbol);

    std::string cmdToJson(const TradeCmd& cmd) const;

    static std::vector<std::string> parseJsonArray(const std::string& json);

    // ── Table de correspondance ID MetaApi ↔ int synthétique ─
    // MetaApi retourne des UUIDs ou IDs alphanumériques impossibles
    // à convertir via stoi(). On génère un int synthétique stable
    // basé sur std::hash, et on maintient la table inverse pour
    // pouvoir résoudre la vraie string ID lors des modify/delete.
    int  registerOrFindId(const std::string& realId);
    std::string resolveId(int syntheticTicket) const;

    mutable std::mutex                  idMapMu_;
    mutable std::unordered_map<int, std::string> idMap_;  // synth → real string
    std::atomic<int>                    syntheticCounter_{ 1'000'000 };

    Config      cfg_;
    std::string token_;
    std::string accountId_;
    std::string baseUrl_;

    std::atomic<bool> connected_ { false };
    std::atomic<bool> running_   { false };

    std::jthread tickThread_;
    std::jthread stateThread_;

    TickCallback    tickCb_;
    OrderCallback   orderCb_;
    AccountCallback accountCb_;
    ErrorCallback   errorCb_;
    ConnectCallback connectCb_;
};

} // namespace top4ea
