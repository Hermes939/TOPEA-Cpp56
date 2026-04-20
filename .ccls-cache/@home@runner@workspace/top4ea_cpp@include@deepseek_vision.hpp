#pragma once
// ============================================================
//  TOP4EA — deepseek_vision.hpp  C++20
//
//  ASSISTANTE VISUELLE AI — DeepSeek LLM Layer
//  ─────────────────────────────────────────────────────────
//  Intégration DeepSeek V3 (API OpenAI-compatible) :
//    • Appels asynchrones non-bloquants (thread dédié)
//    • Prompt institutionnel : session + structure + indicateurs
//    • Signal : BUY_BIAS / SELL_BIAS / NEUTRAL / WAIT / LOCK
//    • Confidence boost si accord avec analyse locale
//    • Cache 30s — l'analyse locale reste prioritaire
//
//  Variable d'environnement : DEEPSEEK_API_KEY
//  Endpoint                 : https://api.deepseek.com
//  Modèle                   : deepseek-chat (DeepSeek-V3)
// ============================================================
#include "types.hpp"
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <functional>

namespace top4ea {

// ============================================================
//  DeepSeekSignal — résultat parsé d'une réponse LLM
// ============================================================
enum class DSSignal : uint8_t {
    BUY_BIAS  = 0,   // tendance haussière confirmée
    SELL_BIAS = 1,   // tendance baissière confirmée
    NEUTRAL   = 2,   // signal mixte / indécis
    WAIT      = 3,   // attendre confirmation
    LOCK      = 4,   // bloquer tous les nouveaux ordres
    UNKNOWN   = 5    // pas encore de réponse / erreur
};

struct DeepSeekSignal {
    DSSignal    signal      = DSSignal::UNKNOWN;
    double      confidence  = 0.0;     // 0–100 (auto-rapporté par le LLM)
    std::string reason      = {};      // explication courte
    std::string rawResponse = {};      // réponse brute pour diagnostic
    bool        fresh       = false;   // true si < cacheTtl secondes
    std::chrono::steady_clock::time_point timestamp;

    [[nodiscard]] std::string signalName() const noexcept {
        switch (signal) {
            case DSSignal::BUY_BIAS:  return "BUY_BIAS";
            case DSSignal::SELL_BIAS: return "SELL_BIAS";
            case DSSignal::NEUTRAL:   return "NEUTRAL";
            case DSSignal::WAIT:      return "WAIT";
            case DSSignal::LOCK:      return "LOCK";
            default:                  return "UNKN";
        }
    }
};

// ============================================================
//  Contexte marché envoyé à DeepSeek à chaque appel
// ============================================================
struct DSMarketContext {
    // Session + structure
    std::string session;      // "TOKYO", "LONDON", "NY", "OVERLAP", "CLOSED"
    std::string structure;    // "BULLISH_TREND", "BEARISH_TREND", "RANGE_TIGHT", etc.

    // Prix courant
    double bid = 0.0;
    double ask = 0.0;

    // M5 indicateurs
    double m5Atr      = 0.0;
    double m5Rsi      = 50.0;
    double m5Macd     = 0.0;
    double m5MacdHist = 0.0;
    double m5BbWidth  = 0.0;
    double m5Momentum = 0.0;

    // H1 indicateurs
    double h1Atr      = 0.0;
    double h1Rsi      = 50.0;
    double h1Macd     = 0.0;
    double h1MacdHist = 0.0;

    // Context basket
    double basketPnl  = 0.0;
    int    openOrders = 0;
    double spreadRatio = 1.0;
};

// ============================================================
//  DeepSeekVision — client LLM asynchrone
// ============================================================
class DeepSeekVision {
public:
    explicit DeepSeekVision();
    ~DeepSeekVision();

    // ─── Demande une analyse asynchrone (non-bloquante) ──────
    // Retourne immédiatement. Le résultat sera disponible via
    // getLastSignal() après ~300-2000ms selon la latence API.
    void requestAnalysis(const DSMarketContext& ctx);

    // ─── Récupère le dernier signal (thread-safe) ─────────────
    [[nodiscard]] DeepSeekSignal getLastSignal() const;

    // ─── Vérifie si une clé API est configurée ────────────────
    [[nodiscard]] bool hasApiKey() const noexcept { return !apiKey_.empty(); }

    // ─── Statistiques ─────────────────────────────────────────
    [[nodiscard]] int callCount()  const noexcept { return callCount_.load();  }
    [[nodiscard]] int errorCount() const noexcept { return errorCount_.load(); }

    // TTL du cache en secondes (défaut 30s)
    static constexpr int CACHE_TTL_SEC = 30;

    // Intervalle minimum entre deux appels API (évite le spam)
    static constexpr int MIN_CALL_INTERVAL_SEC = 15;

private:
    // ─── HTTP synchrone (appelé depuis le thread dédié) ───────
    std::string httpPost(const std::string& url,
                         const std::string& body,
                         int timeoutMs = 8000);

    // ─── Construction du prompt institutionnel ────────────────
    static std::string buildPrompt(const DSMarketContext& ctx);

    // ─── Parseur de réponse JSON LLM ──────────────────────────
    static DeepSeekSignal parseResponse(const std::string& json);

    // ─── Extraction champ JSON simple ─────────────────────────
    static std::string jsonGet(const std::string& json,
                               const std::string& key);
    static double      jsonGetD(const std::string& json,
                                const std::string& key);

    std::string apiKey_;
    std::string endpoint_ = "https://api.deepseek.com/chat/completions";

    // Cache signal courant
    mutable std::mutex    signalMu_;
    DeepSeekSignal        lastSignal_;

    // Thread d'appel asynchrone
    std::mutex            callMu_;
    bool                  callInProgress_ = false;
    std::chrono::steady_clock::time_point lastCallTime_;

    std::atomic<int> callCount_  { 0 };
    std::atomic<int> errorCount_ { 0 };
};

} // namespace top4ea
