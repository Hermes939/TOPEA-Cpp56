#pragma once
// ============================================================
//  TOP4EA — grid_engine.hpp
//  Moteur de grille & trail stop — contrôle absolu des niveaux
//  Chaque déplacement est mathématiquement justifié (ATR)
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace top4ea {

// ─── Résultat de placement/déplacement d'un stop ────────────
struct StopPlacement {
    bool   needsCreate = false;   // doit être créé (absent)
    bool   needsModify = false;   // doit être déplacé
    double targetPrice = 0.0;     // prix calculé
    double currentPrice= 0.0;     // prix actuel (si existant)
    double delta       = 0.0;     // écart cible vs actuel
};

// ============================================================
//  GridEngine — calcul et gestion des niveaux SS/BS
// ============================================================
class GridEngine {
public:
    explicit GridEngine(const Config& cfg);

    // ─── Mise à jour du marché ───────────────────────────────
    void onTick(const Tick& tick) noexcept;

    // Alimentation ATR (depuis tick synthétique ou barre H1)
    void pushOHLC(double o, double h, double l, double c) noexcept;

    // ─── Calcul des niveaux ──────────────────────────────────
    // Calcule le niveau cible SELLSTOP — ancrage géométrique pur
    // anchorPrice    : origine de la grille (gridOrigin_), jamais le prix courant
    // recovDepth     : profondeur recovery SS → SS placé à anchorPrice - (depth+1)*GridGap
    // gapFactor      : facteur asymétrique (1.0 = neutre, >1 = stop plus éloigné)
    // distOverridePts: si > 0, remplace trailFixedDistPts (mode NATURAL asymétrique)
    [[nodiscard]] StopPlacement computeSellStop(
        int    currentTicket,
        double currentLevel,
        double anchorPrice,
        int    recovDepth      = 0,
        double gapFactor       = 1.0,
        int    distOverridePts = -1) const noexcept;

    // Calcule le niveau cible BUYSTOP — ancrage géométrique pur
    // anchorPrice    : origine de la grille (gridOrigin_), jamais le prix courant
    // recovDepth     : profondeur recovery BS → BS placé à anchorPrice + (depth+1)*GridGap
    // gapFactor      : facteur asymétrique (1.0 = neutre, >1 = stop plus éloigné)
    // distOverridePts: si > 0, remplace trailFixedDistPts (mode NATURAL asymétrique)
    [[nodiscard]] StopPlacement computeBuyStop(
        int    currentTicket,
        double currentLevel,
        double anchorPrice,
        int    recovDepth      = 0,
        double gapFactor       = 1.0,
        int    distOverridePts = -1) const noexcept;

    // Mode dual trail côté SELL — NATURAL ou AGGRESSIVE
    // ss_profit_pips : profit flottant des Sell en points (> 0 = en profit)
    [[nodiscard]] TrailMode getTrailMode(double ss_profit_pips) const noexcept;

    // Mode dual trail côté BUY (miroir SS AGGRESSIVE)
    // bs_profit_pips : profit flottant des Buy en points (> 0 = en profit)
    [[nodiscard]] TrailMode getBuyTrailMode(double bs_profit_pips) const noexcept;

    // Demi-gap courant (ATR ou fixe) en points
    [[nodiscard]] double halfGap() const noexcept;

    // GridGap effectif
    [[nodiscard]] int effectiveGridGap() const noexcept;

    // Régime volatilité courant
    [[nodiscard]] VolRegime regime() const noexcept {
        return regime_.load(std::memory_order_relaxed);
    }

    // ATR courant en points
    [[nodiscard]] double atrPoints() const noexcept;

    // ─── Accesseurs tick ────────────────────────────────────
    [[nodiscard]] double bid() const noexcept { return lastBid_.load(std::memory_order_relaxed); }
    [[nodiscard]] double ask() const noexcept { return lastAsk_.load(std::memory_order_relaxed); }
    [[nodiscard]] double mid() const noexcept {
        return (lastBid_.load(std::memory_order_relaxed)
              + lastAsk_.load(std::memory_order_relaxed)) * 0.5;
    }

    // ─── Validation de prix ─────────────────────────────────
    [[nodiscard]] bool isPriceValid(double price, double minDist,
                                    OrderType stopType) const noexcept;

    // Gap OU courant en points (0 si marché non mean-reverting)
    [[nodiscard]] int ouGridGap() const noexcept;

    // Distance minimum broker (MODE_STOPLEVEL × Point)
    [[nodiscard]] double minDistPts() const noexcept;

private:
    [[nodiscard]] double pointSize()  const noexcept;

    Config         cfg_;
    math::ATR      atr_;
    math::VolatilityRegime volReg_;
    math::SpreadTracker    spreadTrack_;
    math::OUEstimator      ouEst_;       // [A] Estimateur Ornstein-Uhlenbeck

    std::atomic<double>    lastBid_   { 0.0 };
    std::atomic<double>    lastAsk_   { 0.0 };
    double                 prevClose_ = 0.0;
    double                 lastHigh_  = 0.0;
    double                 lastLow_   = 0.0;
    std::atomic<VolRegime> regime_    { VolRegime::Medium };

    mutable std::recursive_mutex mu_;
};

} // namespace top4ea
