#pragma once
// ============================================================
//  TOP4EA — market_intelligence.hpp  C++20
//
//  ASSISTANTE VISUELLE AI — Audit marché temps réel
//  ─────────────────────────────────────────────────────────
//  Analyse continue multi-timeframe (iniMTA) :
//    • Barres synthétiques M1 / M5 / M15 / M30 depuis tick stream
//    • ATR(14), RSI(14), MACD(12-26-9), Bollinger(20), Momentum
//    • Détection de session (Tokyo / London / NY / Overlap)
//    • Analyse de structure (HH/HL/LH/LL, breakout, piège)
//
//  OUTPUT : MarketDecision → diffusé aux sous-agents L0..Ln
//    CONTINUE, TRAIL_AGGRESSIVE, TRAIL_NATURAL, TRAIL_FREEZE,
//    ADD_ORDER, REMOVE_TRAIL, NO_TRADE, OBSERVE,
//    LOCK_WAIT, STOP_ORDERS
//
//  COUCHE IA : DeepSeekVision — confirmation LLM async
//    Signal DeepSeek enrichit la décision locale (boost confiance
//    ou surcharge en cas de signal fort désaccordé)
// ============================================================
#include "types.hpp"
#include "deepseek_vision.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <cmath>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace top4ea {

// ============================================================
//  Bar — chandelier synthétique depuis tick stream
// ============================================================
struct Bar {
    double open  = 0.0;
    double high  = 0.0;
    double low   = 0.0;
    double close = 0.0;
    int    ticks = 0;
    std::chrono::system_clock::time_point time;

    [[nodiscard]] double range()   const noexcept { return high - low; }
    [[nodiscard]] double midpoint() const noexcept { return (high + low) * 0.5; }
    [[nodiscard]] bool   valid()   const noexcept { return ticks > 0 && high >= low; }
};

// ============================================================
//  IndiSet — snapshot d'indicateurs pour un timeframe
// ============================================================
struct IndiSet {
    double atr        = 0.0;   // Average True Range(14)
    double rsi        = 50.0;  // RSI(14) [0–100]
    double macdLine   = 0.0;   // MACD = EMA12 - EMA26
    double macdSignal = 0.0;   // Signal = EMA9(MACD)
    double macdHist   = 0.0;   // Histogramme = macdLine - macdSignal
    double bbUpper    = 0.0;   // Bollinger supérieur (20, 2σ)
    double bbMid      = 0.0;   // Bollinger central (SMA20)
    double bbLower    = 0.0;   // Bollinger inférieur
    double bbWidth    = 0.0;   // (bbUpper - bbLower) / bbMid  en %
    double momentum   = 0.0;   // (close - close[10]) / close[10]
    int    barCount   = 0;     // nombre de barres disponibles

    [[nodiscard]] bool  ready()          const noexcept { return barCount >= 14; }
    [[nodiscard]] bool  rsiOverbought()  const noexcept { return rsi >= 85.0;    }
    [[nodiscard]] bool  rsiOversold()    const noexcept { return rsi <= 15.0;    }
    [[nodiscard]] bool  macdBullish()    const noexcept { return macdHist > 0.0; }
    [[nodiscard]] bool  bbNarrow()       const noexcept { return bbWidth < 0.003;}// < 0.3%
};

// ============================================================
//  VisualDecision — sortie complète du Visual AI
// ============================================================
struct VisualDecision {
    MarketDecision  decision   = MarketDecision::CONTINUE;
    MarketStructure structure  = MarketStructure::UNKNOWN;
    TradingSession  session    = TradingSession::CLOSED;
    double          confidence = 0.0;      // 0–100
    int             obsLevel   = -1;       // niveau observé (-1 = aucun)
    std::string     reasoning;             // explication lisible

    // iniMTA snapshot
    IndiSet m1, m5, m15, m30;

    // ── Audit Recovery — score mathématique ──────────────────
    // Chaque facteur contribue à recoveryScore (0–100).
    // allowRecovery = true  → le moteur PEUT déclencher un ordre recovery stop.
    // allowRecovery = false → le déclenchement est bloqué (mauvaises conditions).
    bool   allowRecovery  = true;   // autorisation globale recovery (les deux côtés)
    double recoveryScore  = 50.0;   // qualité multi-facteur du signal (0=non / 100=parfait)
    std::string recoveryReason;     // explication lisible du score

    // ── Bloc directionnel (NOUVEAU) ───────────────────────────
    // allowBSRecovery = false → bloquer les ordres BUY recovery (tendance baissière)
    // allowSSRecovery = false → bloquer les ordres SELL recovery (tendance haussière)
    // Ces flags affinent allowRecovery par direction de marché + balance de lots.
    bool   allowBSRecovery = true;   // autorisation d'ajouter un BUY Stop recovery
    bool   allowSSRecovery = true;   // autorisation d'ajouter un SELL Stop recovery
    std::string bsBlockReason;       // raison du blocage BS (diagnostic)
    std::string ssBlockReason;       // raison du blocage SS (diagnostic)

    // ── Couche DeepSeek LLM ───────────────────────────────────
    DeepSeekSignal  dsSignal;              // dernier signal LLM (UNKNOWN si inactif)
    bool            dsOverride = false;    // true si DeepSeek a surpassé l'analyse locale

    // Biais DS sur les facteurs de trail (appliqués dans trailGrid)
    // < 1.0 = serrer ce côté (trigger plus tôt), > 1.0 = élargir (protection)
    double dsTrailSSBias = 1.0;
    double dsTrailBSBias = 1.0;
};

// ============================================================
//  MarketIntelligence — Visual AI + iniMTA + DeepSeek LLM
// ============================================================
class MarketIntelligence {
public:
    explicit MarketIntelligence() { resetPartials(); }

    // ─── Feed — appelé à chaque tick ─────────────────────────
    // basketPnl / openOrders / bsLots / ssLots : contexte panier pour DeepSeek
    void update(const Tick& tick,
                double pointSize   = 0.01,
                double basketPnl   = 0.0,
                int    openOrders  = 0,
                double bsLots      = 0.0,
                double ssLots      = 0.0,
                bool   inRecovery  = false) noexcept;

    // ─── Analyse complète — thread-safe ──────────────────────
    [[nodiscard]] VisualDecision analyze() const;

    // ─── Accès rapide decision courante ──────────────────────
    [[nodiscard]] MarketDecision decision()         const noexcept;
    [[nodiscard]] std::string    reasoning()        const;
    // Retourne la dernière VisualDecision complète (thread-safe)
    [[nodiscard]] VisualDecision getLastDecision()  const noexcept {
        std::lock_guard lk(mu_);
        return lastDecision_;
    }

    // ─── Vérifie si la clé API DeepSeek est configurée ───────
    [[nodiscard]] bool hasDeepSeekKey() const noexcept {
        return deepseek_.hasApiKey();
    }

    // ─── Stats DeepSeek pour le dashboard ─────────────────────
    [[nodiscard]] long  dsSignalAgeSec()  const noexcept { return deepseek_.signalAgeSec();   }
    [[nodiscard]] long  dsLastCallAge()   const noexcept { return deepseek_.lastCallAgeSec(); }
    [[nodiscard]] int   dsCallCount()     const noexcept { return deepseek_.callCount();      }
    [[nodiscard]] int   dsErrorCount()    const noexcept { return deepseek_.errorCount();     }

    // ─── Reset forcé du signal (si périmé depuis trop longtemps) ─
    void dsResetIfStale(long maxAgeSec = 300) noexcept {
        if (deepseek_.signalAgeSec() > maxAgeSec)
            deepseek_.resetSignal();
    }

    // ─── Réchauffement synthétique (cold-start fix) ───────────
    // Pré-popule les déques de barres avec des bougies synthétiques
    // basées sur le prix courant et l'ATR estimé (gridGap × point).
    // Permet aux indicateurs (ATR/RSI/MACD/BB) de fonctionner
    // dès le premier tick sans attendre des heures de données réelles.
    // À appeler une seule fois au démarrage, avant le premier tick.
    //
    // @param price    prix mid actuel (ex: 4794.0)
    // @param atrPrice ATR estimé en prix (ex: gridGap × point = 500 × 0.01 = 5.0)
    // @param nBars    nombre de bougies synthétiques à générer (défaut 30)
    void warmUp(double price, double atrPrice, int nBars = 30) noexcept;

    // ─── Convertisseurs texte ─────────────────────────────────
    static std::string decisionName (MarketDecision  d) noexcept;
    static std::string structureName(MarketStructure s) noexcept;
    static std::string sessionName  (TradingSession  s) noexcept;

    // ─── Session courante (UTC horloge système) ───────────────
    static TradingSession detectSession() noexcept;

private:
    // ── Construction des barres synthétiques ──────────────────
    void resetPartials() noexcept;
    void pushTick(const Tick& t) noexcept;
    void closeBars(const Tick& t) noexcept;

    // ── Calcul des indicateurs ────────────────────────────────
    [[nodiscard]] IndiSet computeIndicators(
        const std::deque<Bar>& bars) const noexcept;

    [[nodiscard]] double computeATR(
        const std::deque<Bar>& bars, int period = 14) const noexcept;

    [[nodiscard]] double computeRSI(
        const std::deque<Bar>& bars, int period = 14) const noexcept;

    [[nodiscard]] std::pair<double,double> computeMACD(
        const std::deque<Bar>& bars) const noexcept;

    [[nodiscard]] std::tuple<double,double,double> computeBB(
        const std::deque<Bar>& bars, int period = 20) const noexcept;

    // ── Structure & swings ────────────────────────────────────
    [[nodiscard]] MarketStructure analyzeStructure(
        const std::deque<Bar>& bars) const noexcept;

    // ── Decision engine (Visual AI) ───────────────────────────
    [[nodiscard]] VisualDecision computeDecision(
        const IndiSet& m1, const IndiSet& m5,
        const IndiSet& m15, const IndiSet& m30,
        MarketStructure structure,
        TradingSession  session,
        double spreadRatio,
        double basketPnl  = 0.0,
        int    openOrders = 0) const noexcept;

    // ── Stockage barres (FIFO) ────────────────────────────────
    static constexpr int MAX_M1  = 200;
    static constexpr int MAX_M5  = 200;
    static constexpr int MAX_M15 = 100;
    static constexpr int MAX_M30 = 200;  // 200 × 30min = ~100h d'historique

    std::deque<Bar> m1Bars_, m5Bars_, m15Bars_, m30Bars_;

    // Barres en construction
    Bar curM1_, curM5_, curM15_, curM30_;
    std::chrono::system_clock::time_point
        m1Start_, m5Start_, m15Start_, m30Start_;

    // Spread courant et EWMA (pour spreadRatio)
    double spreadEwma_    = 0.0;
    double curSpread_     = 0.0;  // spread du dernier tick

    // Cache contexte panier (mis à jour via update())
    double basketPnlCache_  = 0.0;
    int    openOrdersCache_ = 0;

    // Cache balance de lots BS/SS (pour couche DeepSeek recovery)
    double bsLotsCache_     = 0.0;
    double ssLotsCache_     = 0.0;
    bool   inRecoveryCache_ = false;

    // Cache décision courante (thread-safe)
    mutable std::mutex   mu_;
    mutable VisualDecision lastDecision_;

    int tickCount_ = 0;

    // ── Couche DeepSeek LLM (async, non-bloquant) ─────────────
    mutable DeepSeekVision deepseek_;
};

} // namespace top4ea
