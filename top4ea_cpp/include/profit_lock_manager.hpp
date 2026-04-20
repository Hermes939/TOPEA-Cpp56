#pragma once
// ============================================================
//  TOP4EA — profit_lock_manager.hpp
//  Module Verrouillage Profit Trailing — V-SS / V-BS
//
//  V-SS (Sell Stop Trailing, côté BUY) :
//    S'active quand les Buy sont en profit ≥ vlActivationPts.
//    Niveau = bid - vlTrailPts — suit le bid À LA HAUSSE uniquement.
//    Ratchet strict : le niveau MONTE avec le bid, ne descend JAMAIS.
//    Déclenchement (bid ≤ niveau) → ferme toutes les Buy = profit verrouillé.
//
//  V-BS (Buy Stop Trailing, côté SELL) :
//    S'active quand les Sell sont en profit ≥ vlActivationPts.
//    Niveau = ask + vlTrailPts — suit l'ask À LA BAISSE uniquement.
//    Ratchet strict : le niveau DESCEND avec l'ask, ne remonte JAMAIS.
//    Déclenchement (ask ≥ niveau) → ferme toutes les Sell = profit verrouillé.
//
//  Architecture :
//    - recompute(tick, orders) : recalcule avgEntry + gère l'activation initiale
//    - applyTick(tick)         : applique le ratchet à chaque tick (ultra-rapide)
//    - checkTrigger(tick)      : détecte le déclenchement (aucune gate HTTP)
//    - Les ordres broker V-SS/V-BS sont placés pour visibilité + fallback
// ============================================================
#include "types.hpp"
#include <vector>
#include <cmath>
#include <string>

namespace top4ea {

// ─── Slot d'un ordre Verrouillage ───────────────────────────
struct VLSlot {
    int    ticket     = -1;    // ticket broker de l'ordre stop pending (-1 = aucun)
    int    prevTicket = -1;    // ticket à supprimer avant de placer le nouveau
    double level      = 0.0;   // niveau courant du ratchet (= niveau du stop broker)
    double avgEntry   = 0.0;   // prix moyen pondéré du côté (mis à jour par recompute)
    double totalLots  = 0.0;   // lots cumulés du côté
    bool   active     = false; // des positions de ce côté existent
    bool   activated  = false; // seuil d'activation atteint (profit ≥ vlActivationPts)
    bool   needsPlace = false; // ordre à placer ou modifier sur le broker
};

// ============================================================
//  ProfitLockManager — gestion des verrous trailing V-SS / V-BS
// ============================================================
class ProfitLockManager {
public:
    explicit ProfitLockManager(const Config& cfg);

    // ─── Recalcul composition ────────────────────────────────
    // Appelé quand les ordres changent (1×/seconde via fetchOrders).
    // Recalcule avgEntry, totalLots, et gère l'activation initiale.
    // Retourne true si un ordre broker doit être placé/modifié.
    bool recompute(const Tick& tick,
                   const std::vector<OrderInfo>& orders) noexcept;

    // ─── Ratchet tick-level ──────────────────────────────────
    // Appelé à CHAQUE tick — ultra-rapide (pas de boucle orders).
    // Applique le ratchet : V-SS monte avec bid, V-BS descend avec ask.
    // Retourne true si le niveau a bougé assez pour modifier l'ordre broker.
    bool applyTick(const Tick& tick) noexcept;

    // ─── Détection de déclenchement (chaque tick) ────────────
    // 0 = rien | 1 = V-SS franchi (fermer BUY) | 2 = V-BS franchi (fermer SELL)
    [[nodiscard]] int checkTrigger(const Tick& tick) const noexcept;

    // ─── Gestion tickets broker ──────────────────────────────
    void setSSTicket(int ticket, double level) noexcept;
    void setBSTicket(int ticket, double level) noexcept;
    void clearSSNeedsPlace() noexcept { ssVL_.needsPlace = false; }
    void clearBSNeedsPlace() noexcept { bsVL_.needsPlace = false; }

    // ─── Intégration Dual Trail AGGRESSIVE ───────────────────
    // Remplace le niveau V-BS par le prix de verrou AGGRESSIVE.
    // Appelé depuis top4ea_engine quand vLockActive_ = true.
    // Fait monter la priorité du V-BS broker au niveau de verrou garanti.
    void overrideBSForLock(double lockLevel) noexcept {
        if (!bsVL_.activated || std::abs(bsVL_.level - lockLevel) >= changeTol_) {
            bsVL_.level      = lockLevel;
            bsVL_.activated  = true;
            bsVL_.active     = true;
            bsVL_.needsPlace = true;
        }
    }

    // ─── Accesseurs ──────────────────────────────────────────
    [[nodiscard]] const VLSlot& ssVL() const noexcept { return ssVL_; }
    [[nodiscard]] const VLSlot& bsVL() const noexcept { return bsVL_; }

    [[nodiscard]] double ssLevel() const noexcept { return ssVL_.level; }
    [[nodiscard]] double bsLevel() const noexcept { return bsVL_.level; }

    [[nodiscard]] bool ssActive() const noexcept { return ssVL_.activated; }
    [[nodiscard]] bool bsActive() const noexcept { return bsVL_.activated; }

    // ─── Reset ───────────────────────────────────────────────
    void resetSS() noexcept;
    void resetBS() noexcept;
    void reset()   noexcept;

    // ─── Rapport dashboard ───────────────────────────────────
    [[nodiscard]] std::string report() const noexcept;

private:
    void updateSide(int side, const Tick& tick,
                    const std::vector<OrderInfo>& orders) noexcept;
    [[nodiscard]] bool levelChanged(double oldLevel,
                                    double newLevel) const noexcept;

    Config  cfg_;
    VLSlot  ssVL_;   // V-SS → trailing stop côté BUY  (monte avec le bid)
    VLSlot  bsVL_;   // V-BS → trailing stop côté SELL (descend avec l'ask)

    double  changeTol_ = 0.0;   // seuil minimum de déplacement pour mise à jour broker
};

} // namespace top4ea
