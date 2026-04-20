// ============================================================
//  TOP4EA — profit_lock_manager.cpp
//  Module Verrouillage Profit Trailing — V-SS / V-BS
//
//  V-SS : trailing SellStop côté BUY
//    - S'active quand bid > avgBuy + vlActivationPts × point
//    - Niveau ratchet = bid - vlTrailPts × point
//    - Monte avec le bid, ne descend JAMAIS
//    - bid ≤ niveau → déclenche, ferme toutes les Buy
//
//  V-BS : trailing BuyStop côté SELL
//    - S'active quand ask < avgSell - vlActivationPts × point
//    - Niveau ratchet = ask + vlTrailPts × point
//    - Descend avec l'ask, ne remonte JAMAIS
//    - ask ≥ niveau → déclenche, ferme toutes les Sell
// ============================================================
#include "../include/profit_lock_manager.hpp"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Constructor
// ────────────────────────────────────────────────────────────
ProfitLockManager::ProfitLockManager(const Config& cfg)
    : cfg_(cfg)
    , changeTol_((double)cfg.vlTrailPts * cfg.point / 4.0)
{}

// ────────────────────────────────────────────────────────────
//  updateSide — recalcul d'un côté (interne)
//
//  side = 0 → Buy positions  → V-SS (trailing sell stop, monte avec bid)
//  side = 1 → Sell positions → V-BS (trailing buy stop, descend avec ask)
// ────────────────────────────────────────────────────────────
void ProfitLockManager::updateSide(int side, const Tick& tick,
                                    const std::vector<OrderInfo>& orders) noexcept
{
    const OrderType target = (side == 0) ? OrderType::Buy : OrderType::Sell;
    VLSlot& slot = (side == 0) ? ssVL_ : bsVL_;

    double sumLP = 0.0;
    double sumL  = 0.0;

    for (const auto& o : orders) {
        if (o.type != target) continue;
        if (o.lots <= 0.0)    continue;
        sumLP += o.openPrice * o.lots;
        sumL  += o.lots;
    }

    if (sumL < 1e-6) {
        // Aucune position de ce côté → désactiver complètement
        if (slot.active) {
            if (slot.ticket > 0) {
                slot.prevTicket = slot.ticket;
                slot.ticket     = -1;
            }
            slot = {};
        }
        return;
    }

    double avgEntry  = sumLP / sumL;
    double activPts  = (double)cfg_.vlActivationPts * cfg_.point;
    double trailPts  = (double)cfg_.vlTrailPts      * cfg_.point;
    double factor    = std::pow(10.0, cfg_.digits);

    slot.active     = true;
    slot.avgEntry   = avgEntry;
    slot.totalLots  = sumL;

    if (side == 0) {
        // ── V-SS côté BUY ────────────────────────────────────
        bool profitable = (tick.bid > avgEntry + activPts);

        if (!slot.activated && profitable) {
            slot.activated  = true;
            double initLvl  = std::round((tick.bid - trailPts) * factor) / factor;
            slot.level      = initLvl;
            slot.needsPlace = true;

        } else if (slot.activated) {
            double newLvl = std::round((tick.bid - trailPts) * factor) / factor;
            if (newLvl > slot.level + changeTol_) {
                slot.level      = newLvl;
                slot.needsPlace = true;
            }
        }

    } else {
        // ── V-BS côté SELL ───────────────────────────────────
        bool profitable = (tick.ask < avgEntry - activPts);

        if (!slot.activated && profitable) {
            slot.activated  = true;
            double initLvl  = std::round((tick.ask + trailPts) * factor) / factor;
            slot.level      = initLvl;
            slot.needsPlace = true;

        } else if (slot.activated) {
            double newLvl = std::round((tick.ask + trailPts) * factor) / factor;
            if (newLvl < slot.level - changeTol_) {
                slot.level      = newLvl;
                slot.needsPlace = true;
            }
        }
    }
}

// ────────────────────────────────────────────────────────────
//  recompute — recalcule la composition et l'activation
//  Appelé 1×/seconde quand les ordres sont rafraîchis.
// ────────────────────────────────────────────────────────────
bool ProfitLockManager::recompute(const Tick& tick,
                                   const std::vector<OrderInfo>& orders) noexcept
{
    bool wasSS = ssVL_.needsPlace;
    bool wasBS = bsVL_.needsPlace;

    updateSide(0, tick, orders);
    updateSide(1, tick, orders);

    return ssVL_.needsPlace != wasSS
        || bsVL_.needsPlace != wasBS
        || ssVL_.needsPlace
        || bsVL_.needsPlace;
}

// ────────────────────────────────────────────────────────────
//  applyTick — ratchet pur à chaque tick
//  Ultra-rapide : pas de boucle sur les ordres.
// ────────────────────────────────────────────────────────────
bool ProfitLockManager::applyTick(const Tick& tick) noexcept
{
    bool moved = false;
    double factor   = std::pow(10.0, cfg_.digits);
    double trailPts = (double)cfg_.vlTrailPts * cfg_.point;

    // ── V-SS ratchet (monte seulement) ───────────────────────
    if (ssVL_.activated && ssVL_.active) {
        double newLvl = std::round((tick.bid - trailPts) * factor) / factor;
        if (newLvl > ssVL_.level + changeTol_) {
            ssVL_.level      = newLvl;
            ssVL_.needsPlace = true;
            moved            = true;
        }
    }

    // ── V-BS ratchet (descend seulement) ─────────────────────
    if (bsVL_.activated && bsVL_.active) {
        double newLvl = std::round((tick.ask + trailPts) * factor) / factor;
        if (newLvl < bsVL_.level - changeTol_) {
            bsVL_.level      = newLvl;
            bsVL_.needsPlace = true;
            moved            = true;
        }
    }

    return moved;
}

// ────────────────────────────────────────────────────────────
//  checkTrigger — détection crossing tick-level (sans gate HTTP)
//  Priorité au côté le plus proche du mid si les deux déclenchent.
// ────────────────────────────────────────────────────────────
int ProfitLockManager::checkTrigger(const Tick& tick) const noexcept
{
    // V-SS : bid ≤ niveau → les Buy sont à l'objectif de profit
    bool ssFired = ssVL_.activated && ssVL_.level > 0.0
                   && tick.bid <= ssVL_.level;

    // V-BS : ask ≥ niveau → les Sell sont à l'objectif de profit
    bool bsFired = bsVL_.activated && bsVL_.level > 0.0
                   && tick.ask >= bsVL_.level;

    if (!ssFired && !bsFired) return 0;
    if (ssFired  && !bsFired) return 1;
    if (!ssFired && bsFired)  return 2;

    // Les deux déclenchés simultanément : prioriser le plus proche du mid
    double mid = (tick.bid + tick.ask) * 0.5;
    double dSS = std::abs(mid - ssVL_.level);
    double dBS = std::abs(mid - bsVL_.level);
    return (dSS <= dBS) ? 1 : 2;
}

// ────────────────────────────────────────────────────────────
//  Gestion des tickets
// ────────────────────────────────────────────────────────────
void ProfitLockManager::setSSTicket(int ticket, double level) noexcept {
    ssVL_.ticket     = ticket;
    ssVL_.level      = level;
    ssVL_.needsPlace = false;
    ssVL_.prevTicket = -1;
}

void ProfitLockManager::setBSTicket(int ticket, double level) noexcept {
    bsVL_.ticket     = ticket;
    bsVL_.level      = level;
    bsVL_.needsPlace = false;
    bsVL_.prevTicket = -1;
}

// ────────────────────────────────────────────────────────────
//  Reset
// ────────────────────────────────────────────────────────────
void ProfitLockManager::resetSS() noexcept { ssVL_ = {}; }
void ProfitLockManager::resetBS() noexcept { bsVL_ = {}; }
void ProfitLockManager::reset()   noexcept { ssVL_ = {}; bsVL_ = {}; }

// ────────────────────────────────────────────────────────────
//  Rapport dashboard
// ────────────────────────────────────────────────────────────
std::string ProfitLockManager::report() const noexcept {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (bsVL_.activated && bsVL_.level > 0.0)
        oss << "V-BS@" << std::setprecision(5) << bsVL_.level
            << "(avgSell=" << bsVL_.avgEntry
            << " L=" << std::setprecision(2) << bsVL_.totalLots << ")  ";
    else if (bsVL_.active)
        oss << "V-BS:en_attente_activation  ";
    else
        oss << "V-BS:inactif  ";

    if (ssVL_.activated && ssVL_.level > 0.0)
        oss << "V-SS@" << std::setprecision(5) << ssVL_.level
            << "(avgBuy=" << ssVL_.avgEntry
            << " L=" << std::setprecision(2) << ssVL_.totalLots << ")";
    else if (ssVL_.active)
        oss << "V-SS:en_attente_activation";
    else
        oss << "V-SS:inactif";
    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  levelChanged — seuil de changement significatif
// ────────────────────────────────────────────────────────────
bool ProfitLockManager::levelChanged(double oldLevel,
                                      double newLevel) const noexcept
{
    return std::abs(newLevel - oldLevel) >= changeTol_;
}

} // namespace top4ea
