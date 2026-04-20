#pragma once
// ============================================================
//  TOP4EA — break_even_manager.hpp
//  Module Break-Even Orders — BE-BS et BE-SS
//
//  Principe :
//    BE-BS (Buy Stop côté SELL) :
//      Placé au prix moyen pondéré de toutes les positions Sell
//      + buffer (commissions + marge de sécurité).
//      Quand le prix remonte jusqu'ici → les Sells sont à l'équilibre
//      → le moteur ferme le côté Sell au marché (zéro perte).
//
//    BE-SS (Sell Stop côté BUY) :
//      Placé au prix moyen pondéré de toutes les positions Buy
//      - buffer.
//      Quand le prix descend jusqu'ici → les Buys sont à l'équilibre
//      → le moteur ferme le côté Buy au marché (zéro perte).
//
//  Architecture modulaire :
//    - Indépendant du moteur de recovery
//    - Mis à jour à chaque changement de composition du panier
//    - Détection de crossing au niveau tick (non soumise à la gate HTTP)
//    - Ordres BE placés sur le broker (visuel + fallback si déconnexion)
// ============================================================
#include "types.hpp"
#include <vector>
#include <cmath>
#include <string>

namespace top4ea {

// ─── Résultat du calcul break-even pour un côté ─────────────
struct BECalc {
    bool   valid        = false;
    double avgEntry     = 0.0;   // prix moyen pondéré du côté
    double totalLots    = 0.0;   // lots cumulés du côté
    double commCostPts  = 0.0;   // coût commissions+swaps en points
    double level        = 0.0;   // niveau à placer (avgEntry ± buffer)
};

// ============================================================
//  BreakEvenManager
// ============================================================
class BreakEvenManager {
public:
    // ─── Slot d'ordre BE ─────────────────────────────────────
    struct BESlot {
        int    ticket    = -1;
        double level     = 0.0;    // niveau placé sur broker
        double avgEntry  = 0.0;    // prix moyen au moment du calcul
        double totalLots = 0.0;
        bool   active    = false;  // slot en service
    };

    explicit BreakEvenManager(const Config& cfg);

    // ─── Recalcul des niveaux ────────────────────────────────
    // Analyse les positions marché courantes et recalcule avgEntry
    // + niveau BE pour chaque côté.
    // Retourne vrai si au moins un niveau a bougé au-delà du seuil.
    bool recompute(const std::vector<OrderInfo>& orders) noexcept;

    // ─── Calcul statique ─────────────────────────────────────
    // Peut être utilisé en dehors du cycle normal (ex: avant placement)
    [[nodiscard]] BECalc calcSide(const std::vector<OrderInfo>& orders,
                                  int side) const noexcept;
    // side = 0 : positions Buy  → BE-SS en dessous
    // side = 1 : positions Sell → BE-BS au dessus

    // ─── Détection de crossing (appelée à chaque tick) ───────
    // Retourne 0 = rien | 1 = BE-SS franchi | 2 = BE-BS franchi
    // (priorité au côté le plus proche du prix)
    [[nodiscard]] int checkTrigger(const Tick& tick) const noexcept;

    // ─── Gestion des tickets broker ─────────────────────────
    void setSSTicket(int ticket, double level) noexcept;
    void setBSTicket(int ticket, double level) noexcept;

    // ─── Accesseurs ──────────────────────────────────────────
    [[nodiscard]] const BESlot& ssBE() const noexcept { return ssBE_; }
    [[nodiscard]] const BESlot& bsBE() const noexcept { return bsBE_; }

    [[nodiscard]] double ssLevel() const noexcept { return ssBE_.level; }
    [[nodiscard]] double bsLevel() const noexcept { return bsBE_.level; }

    [[nodiscard]] bool ssActive() const noexcept { return ssBE_.active; }
    [[nodiscard]] bool bsActive() const noexcept { return bsBE_.active; }

    // ─── Reset ───────────────────────────────────────────────
    void resetSS() noexcept;
    void resetBS() noexcept;
    void reset()   noexcept;

    // ─── Rapport dashboard ───────────────────────────────────
    [[nodiscard]] std::string report() const noexcept;

private:
    [[nodiscard]] bool levelChanged(double oldLevel,
                                    double newLevel) const noexcept;

    Config  cfg_;
    BESlot  ssBE_;   // BE-SS → protège positions Buy
    BESlot  bsBE_;   // BE-BS → protège positions Sell

    // Seuil de changement pour éviter les mises à jour inutiles
    // (= beBufferPts / 4 points en prix)
    double  changeTol_ = 0.0;
};

} // namespace top4ea
