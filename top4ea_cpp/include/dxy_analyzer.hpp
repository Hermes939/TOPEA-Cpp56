#pragma once
// ============================================================
//  TOP4EA — dxy_analyzer.hpp  C++20
//
//  MODULE DXY ANALYZER — Proxy inversé depuis barres XAU
//  ─────────────────────────────────────────────────────────
//  L'or (XAUUSD) est inversement corrélé au Dollar (DXY).
//  En l'absence de feed DXY externe, ce module utilise les
//  barres M15/M30 XAU comme proxy inversé :
//    • XAU EMA20 < EMA50 (XAU baissier)  → DXY BULL (dollar fort)
//    • XAU EMA20 > EMA50 (XAU haussier)  → DXY BEAR (dollar faible)
//    • ADX(14) > 25 sur M15 confirme la tendance DXY
//
//  Exposé dans VisualDecision.dxySignal pour le vote board.
// ============================================================
#include <deque>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdint>

namespace top4ea {

struct Bar;

// ─── Signal directionnel DXY (proxy XAU inversé) ────────────
enum class DXYSignal : uint8_t {
    NEUTRAL = 0,   // pas assez de données ou ADX faible
    BULL    = 1,   // dollar fort → XAU structurellement baissier
    BEAR    = 2,   // dollar faible → XAU structurellement haussier
};

// ─── Résultat complet de l'analyse DXY ──────────────────────
struct DXYResult {
    DXYSignal   signal    = DXYSignal::NEUTRAL;
    double      strength  = 0.0;    // force du signal [0–100]
    double      ema20     = 0.0;    // EMA20 des closes M30
    double      ema50     = 0.0;    // EMA50 des closes M30
    double      adx       = 0.0;    // ADX(14) calculé sur M15
    std::string reason;

    [[nodiscard]] static const char* name(DXYSignal s) noexcept {
        switch (s) {
            case DXYSignal::BULL:    return "BULL";
            case DXYSignal::BEAR:    return "BEAR";
            default:                 return "NEUT";
        }
    }
};

// ============================================================
//  DXYAnalyzer — analyse sans état (fonctions statiques)
//  Appelé depuis MarketIntelligence::analyze() avec les
//  snapshots de barres déjà calculés.
// ============================================================
class DXYAnalyzer {
public:
    // ─── Point d'entrée principal ────────────────────────────
    // barsM30 : barres M30 (EMA20/50 close)
    // barsM15 : barres M15 (ADX14)
    [[nodiscard]] static DXYResult compute(
        const std::deque<Bar>& barsM30,
        const std::deque<Bar>& barsM15) noexcept;

private:
    // ─── EMA rapide ──────────────────────────────────────────
    [[nodiscard]] static double computeEMA(
        const std::deque<Bar>& bars, int period) noexcept;

    // ─── ADX(14) Wilder ──────────────────────────────────────
    [[nodiscard]] static double computeADX(
        const std::deque<Bar>& bars, int period = 14) noexcept;
};

} // namespace top4ea
