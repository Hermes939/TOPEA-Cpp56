#pragma once
// ============================================================
//  TOP4EA — market_intelligence.hpp  C++20
//
//  ASSISTANTE VISUELLE AI — Audit marché temps réel
//  ─────────────────────────────────────────────────────────
//  Analyse continue multi-timeframe (iniMTA) :
//    • Barres synthétiques M1 / M5 / M15 / H1 depuis tick stream
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
    [[nodiscard]] bool  rsiOverbought()  const noexcept { return rsi >= 70.0;    }
    [[nodiscard]] bool  rsiOversold()    const noexcept { return rsi <= 30.0;    }
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
    IndiSet m1, m5, m15, h1;

    // ── Couche DeepSeek LLM ───────────────────────────────────
    DeepSeekSignal  dsSignal;              // dernier signal LLM (UNKNOWN si inactif)
    bool            dsOverride = false;    // true si DeepSeek a surpassé l'analyse locale
};

// ============================================================
//  MarketIntelligence — Visual AI + iniMTA + DeepSeek LLM
// ============================================================
class MarketIntelligence {
public:
    explicit MarketIntelligence() { resetPartials(); }

    // ─── Feed — appelé à chaque tick ─────────────────────────
    // basketPnl / openOrders : contexte panier pour prompt DeepSeek
    void update(const Tick& tick,
                double pointSize   = 0.01,
                double basketPnl   = 0.0,
                int    openOrders  = 0) noexcept;

    // ─── Analyse complète — thread-safe ──────────────────────
    [[nodiscard]] VisualDecision analyze() const;

    // ─── Accès rapide decision courante ──────────────────────
    [[nodiscard]] MarketDecision decision()  const noexcept;
    [[nodiscard]] std::string    reasoning() const;

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
        const IndiSet& m15, const IndiSet& h1,
        MarketStructure structure,
        TradingSession  session,
        double spreadRatio) const noexcept;

    // ── Stockage barres (FIFO) ────────────────────────────────
    static constexpr int MAX_M1  = 200;
    static constexpr int MAX_M5  = 200;
    static constexpr int MAX_M15 = 100;
    static constexpr int MAX_H1  = 100;

    std::deque<Bar> m1Bars_, m5Bars_, m15Bars_, h1Bars_;

    // Barres en construction
    Bar curM1_, curM5_, curM15_, curH1_;
    std::chrono::system_clock::time_point
        m1Start_, m5Start_, m15Start_, h1Start_;

    // Spread EWMA (pour spreadRatio)
    double spreadEwma_ = 0.0;

    // Cache décision courante (thread-safe)
    mutable std::mutex   mu_;
    mutable VisualDecision lastDecision_;

    int tickCount_ = 0;

    // ── Couche DeepSeek LLM (async, non-bloquant) ─────────────
    mutable DeepSeekVision deepseek_;
};

} // namespace top4ea
