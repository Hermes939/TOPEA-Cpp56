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
    DSSignal    signal              = DSSignal::UNKNOWN;
    double      confidence          = 0.0;    // 0–100 (auto-rapporté par le LLM)
    double      effectiveConfidence = 0.0;    // confidence × decay (utilisée pour les seuils)
    int         consecutiveCount    = 0;      // nombre de signaux identiques consécutifs reçus
    std::string reason              = {};     // explication courte
    std::string rawResponse         = {};     // réponse brute pour diagnostic
    bool        fresh               = false;  // true si < cacheTtl secondes ET pas hard-stale
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

    // M30 indicateurs
    double m30Atr      = 0.0;
    double m30Rsi      = 50.0;
    double m30Macd     = 0.0;
    double m30MacdHist = 0.0;

    // Context basket
    double basketPnl   = 0.0;
    int    openOrders  = 0;
    double spreadRatio = 1.0;

    // Balance de lots BS vs SS — critique pour recovery directionnel
    double bsLots  = 0.0;   // total lots côté Buy  (positions haussières)
    double ssLots  = 0.0;   // total lots côté Sell (positions baissières)
    double netDelta = 0.0;  // bsLots - ssLots  (>0 = surexposé BS)
    bool   inRecovery = false; // vrai si le basket est en mode recovery
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

    // ─── Âge du dernier signal en secondes (0 = tout frais) ───
    [[nodiscard]] long signalAgeSec() const noexcept {
        std::lock_guard lk(signalMu_);
        if (lastSignal_.signal == DSSignal::UNKNOWN) return -1;
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - lastSignal_.timestamp).count();
    }

    // ─── Âge du dernier APPEL API tenté (pas forcément réussi) ─
    [[nodiscard]] long lastCallAgeSec() const noexcept {
        std::lock_guard lk(callMu_);
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - lastCallTime_).count();
    }

    // ─── Reset complet — efface le signal en cache ─────────────
    // Utilisé si le signal est trop vieux ou si les conditions
    // ont changé radicalement (ex: changement de session)
    void resetSignal() noexcept {
        std::lock_guard lk(signalMu_);
        lastSignal_ = DeepSeekSignal{};
    }

    // TTL du cache en secondes — 45s max pour garantir fraîcheur en temps réel
    static constexpr int CACHE_TTL_SEC = 45;

    // Intervalle minimum entre deux appels API — 5s pour ne jamais s'endormir
    static constexpr int MIN_CALL_INTERVAL_SEC = 5;

    // Délai de réveil forcé — si signal > FORCE_WAKE_SEC, bypass throttle
    static constexpr int FORCE_WAKE_SEC = 30;

    // ─── Anti-biais statique : decay de répétitions ──────────
    // Nombre de signaux identiques consécutifs avant que le decay commence
    static constexpr int    DS_REPEAT_DECAY_START = 3;
    // Nombre max de répétitions pour atteindre le plancher du decay
    static constexpr int    DS_REPEAT_DECAY_N_MAX = 10;
    // Plancher du decay (50% = signal répété 10× vaut moitié de sa confidence)
    static constexpr double DS_REPEAT_DECAY_FLOOR = 0.5;

    // ─── Timeout absolu de signal répétitif (configurable) ───
    // Après dsStaleHardSec_ secondes du premier signal identique consécutif,
    // fresh est forcé à false indépendamment du TTL de 45s.
    void setStaleHardSec(int sec) noexcept { dsStaleHardSec_ = sec; }
    [[nodiscard]] int staleHardSec() const noexcept { return dsStaleHardSec_; }

    // ─── Watchdog : détecte un appel coincé ──────────────────
    // Retourne le nombre de secondes écoulées depuis le début du dernier
    // appel en cours, ou 0 si aucun appel n'est en cours.
    // CALL_STUCK_SEC : durée au-delà de laquelle callInProgress_ est réinitialisé.
    static constexpr int CALL_STUCK_SEC = 30;
    [[nodiscard]] long callProgressSec() const noexcept {
        std::lock_guard lk(callMu_);
        if (!callInProgress_) return 0;
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - callStartTime_).count();
    }

    // ─── Compteur de répétitions courant ─────────────────────
    [[nodiscard]] int consecutiveCount() const noexcept {
        std::lock_guard lk(signalMu_);
        return consecutiveIdenticalCount_;
    }

    // ─── Réveil forcé — contourne le throttling si signal périmé ──────
    // Appelle requestAnalysis en ignorant lastCallTime_ si le signal
    // est absent ou plus vieux que FORCE_WAKE_SEC secondes.
    void forceWake(const DSMarketContext& ctx);

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

    // ─── Anti-biais statique — compteur de répétitions ────────
    // Protégés par signalMu_
    int consecutiveIdenticalCount_ = 0;
    std::chrono::steady_clock::time_point firstIdenticalTimestamp_;

    // ─── Détection de changement de session ───────────────────
    // Protégé par signalMu_
    std::string lastSession_;

    // Timeout absolu configurable (secondes) — 0 = désactivé
    int dsStaleHardSec_ = 300;

    // Thread d'appel asynchrone + watchdog
    mutable std::mutex    callMu_;
    bool                  callInProgress_ = false;
    std::chrono::steady_clock::time_point lastCallTime_;
    std::chrono::steady_clock::time_point callStartTime_;  // horodatage début d'appel (watchdog)

    std::atomic<int> callCount_  { 0 };
    std::atomic<int> errorCount_ { 0 };
};

} // namespace top4ea
