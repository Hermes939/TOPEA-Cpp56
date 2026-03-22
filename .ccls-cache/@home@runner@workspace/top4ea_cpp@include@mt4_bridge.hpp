#pragma once
// ============================================================
//  TOP4EA — mt4_bridge.hpp
//  Couche réseau TCP vers MT4 (socket POSIX/Winsock)
//  Protocole : JSON-ligne (newline-delimited JSON)
//  Connexion : C++20 app ← TCP → MQL4 Bridge EA
// ============================================================
#include "types.hpp"
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <vector>
#include <atomic>
#include <optional>
#include <chrono>
#include <future>
#include <stdexcept>

// Portabilité POSIX / Windows
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socket_t = SOCKET;
#  define SOCK_INVALID INVALID_SOCKET
#  define SOCK_ERR     SOCKET_ERROR
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
   using socket_t = int;
#  define SOCK_INVALID (-1)
#  define SOCK_ERR     (-1)
#  define closesocket  close
#endif

namespace top4ea {

// ─── Callbacks depuis le bridge ─────────────────────────────
using TickCallback    = std::function<void(const Tick&)>;
using OrderCallback   = std::function<void(const OrderInfo&)>;
using AccountCallback = std::function<void(const AccountSnapshot&)>;
using ErrorCallback   = std::function<void(int code, const std::string& msg)>;
using ConnectCallback = std::function<void(bool connected)>;

// ─── Commandes envoyées vers MT4 ────────────────────────────
struct TradeCmd {
    enum class Op : uint8_t {
        MarketOrder,    // ouvrir ordre marché
        DeletePending,  // supprimer ordre pendand
        CloseOrder,     // fermer ordre marché
        CloseBy,        // netting buy/sell
        ModifyOrder     // modifier SL/TP/prix
    };
    Op         op        = Op::MarketOrder;
    OrderType  type      = OrderType::Buy;
    int        ticket    = -1;
    int        ticket2   = -1;   // pour CloseBy
    double     lots      = 0.0;
    double     price     = 0.0;
    double     sl        = 0.0;
    double     tp        = 0.0;
    int        slippage  = 3;
    int        magic     = 0;
    std::string comment  = {};
    std::string symbol   = {};
};

// ─── Réponse du bridge ──────────────────────────────────────
struct BridgeResponse {
    bool        success  = false;
    int         ticket   = -1;
    int         error    = 0;
    double      price    = 0.0;
    std::string msg      = {};
};

// ============================================================
//  MT4Bridge — connexion TCP asynchrone vers MT4
// ============================================================
class MT4Bridge {
public:
    explicit MT4Bridge(const Config& cfg);
    ~MT4Bridge();

    // Lifecycle
    void start();
    void stop();
    [[nodiscard]] bool isConnected() const noexcept { return connected_.load(); }

    // Envoi de commandes de trading (bloquant, timeout configurable)
    BridgeResponse sendCommand(const TradeCmd& cmd, int timeoutMs = 3000);

    // Abonnements callbacks (thread-safe)
    void onTick   (TickCallback    cb) { tickCb_    = std::move(cb); }
    void onOrder  (OrderCallback   cb) { orderCb_   = std::move(cb); }
    void onAccount(AccountCallback cb) { accountCb_ = std::move(cb); }
    void onError  (ErrorCallback   cb) { errorCb_   = std::move(cb); }
    void onConnect(ConnectCallback cb) { connectCb_ = std::move(cb); }

    // Requête synchrone : liste des ordres courants
    [[nodiscard]] std::vector<OrderInfo>   requestOrders (int timeoutMs = 2000);
    [[nodiscard]] std::optional<AccountSnapshot> requestAccount(int timeoutMs = 2000);

private:
    void   ioLoop();          // thread principal I/O
    void   processLine(const std::string& line);
    bool   tryConnect();
    void   disconnect();
    bool   writeLine(const std::string& line);
    bool   readLine(std::string& out, int timeoutMs);
    void   pingLoop();

    // Parsing JSON minimal (sans dépendance externe)
    static Tick            parseTick   (const std::string& json);
    static OrderInfo       parseOrder  (const std::string& json);
    static AccountSnapshot parseAccount(const std::string& json);
    static BridgeResponse  parseResp   (const std::string& json);

    // Sérialisation commande → JSON
    static std::string cmdToJson(const TradeCmd& cmd);

    // Extraction valeur simple depuis JSON plat
    static std::string jsonGet(const std::string& json, const std::string& key);
    static double      jsonGetD(const std::string& json, const std::string& key);
    static int         jsonGetI(const std::string& json, const std::string& key);
    static bool        jsonGetB(const std::string& json, const std::string& key);

    Config   cfg_;
    socket_t sock_        = SOCK_INVALID;

    std::atomic<bool> connected_  { false };
    std::atomic<bool> running_    { false };

    std::jthread ioThread_;
    std::jthread pingThread_;

    // File de commandes en attente
    struct PendingCmd {
        std::string              json;
        std::promise<BridgeResponse> prom;
    };
    std::mutex              cmdMu_;
    std::queue<PendingCmd*> cmdQueue_;
    std::condition_variable cmdCv_;

    // Réponse en attente (1 seule commande à la fois)
    std::mutex              respMu_;
    std::promise<BridgeResponse>* pendingProm_ = nullptr;

    // Buffer de lecture réseau
    std::string readBuf_;

    // Callbacks
    TickCallback    tickCb_;
    OrderCallback   orderCb_;
    AccountCallback accountCb_;
    ErrorCallback   errorCb_;
    ConnectCallback connectCb_;
};

// ============================================================
//  Utilitaires socket inline
// ============================================================
inline void socketInit() {
#ifdef _WIN32
    WSADATA wd;
    WSAStartup(MAKEWORD(2,2), &wd);
#endif
}
inline void socketCleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}
inline void socketClose(socket_t& s) {
    if (s != SOCK_INVALID) { closesocket(s); s = SOCK_INVALID; }
}
inline bool setNonBlocking(socket_t s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) != SOCK_ERR;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

} // namespace top4ea
