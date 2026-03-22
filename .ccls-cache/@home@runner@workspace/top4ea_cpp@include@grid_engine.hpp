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
    // Calcule le niveau cible SELLSTOP adaptatif
    [[nodiscard]] StopPlacement computeSellStop(
        int    currentTicket,
        double currentLevel,
        double minDist) const noexcept;

    // Calcule le niveau cible BUYSTOP adaptatif
    [[nodiscard]] StopPlacement computeBuyStop(
        int    currentTicket,
        double currentLevel,
        double minDist) const noexcept;

    // Demi-gap courant (ATR ou fixe) en points
    [[nodiscard]] double halfGap() const noexcept;

    // GridGap effectif
    [[nodiscard]] int effectiveGridGap() const noexcept;

    // Régime volatilité courant
    [[nodiscard]] VolRegime regime() const noexcept { return regime_; }

    // ATR courant en points
    [[nodiscard]] double atrPoints() const noexcept;

    // ─── Accesseurs tick ────────────────────────────────────
    [[nodiscard]] double bid() const noexcept { return lastBid_; }
    [[nodiscard]] double ask() const noexcept { return lastAsk_; }
    [[nodiscard]] double mid() const noexcept { return (lastBid_ + lastAsk_) * 0.5; }

    // ─── Validation de prix ─────────────────────────────────
    [[nodiscard]] bool isPriceValid(double price, double minDist,
                                    OrderType stopType) const noexcept;

private:
    [[nodiscard]] double minDistPts() const noexcept;
    [[nodiscard]] double pointSize()  const noexcept;

    Config         cfg_;
    math::ATR      atr_;
    math::VolatilityRegime volReg_;
    math::SpreadTracker    spreadTrack_;

    double     lastBid_   = 0.0;
    double     lastAsk_   = 0.0;
    double     prevClose_ = 0.0;
    double     lastHigh_  = 0.0;
    double     lastLow_   = 0.0;
    VolRegime  regime_    = VolRegime::Medium;

    mutable std::mutex mu_;
};

} // namespace top4ea
