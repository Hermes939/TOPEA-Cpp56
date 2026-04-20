// ============================================================
//  TOP4EA — break_even_manager.cpp
//  Implémentation du module Break-Even Orders (BE-BS / BE-SS)
// ============================================================
#include "../include/break_even_manager.hpp"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Constructor
// ────────────────────────────────────────────────────────────
BreakEvenManager::BreakEvenManager(const Config& cfg)
    : cfg_(cfg)
    , changeTol_((double)cfg.beBufferPts * cfg.point / 4.0)
{}

// ────────────────────────────────────────────────────────────
//  calcSide — calcul pur du break-even d'un côté
//  side = 0 → Buy positions  → BE-SS (sell stop en dessous)
//  side = 1 → Sell positions → BE-BS (buy stop au dessus)
// ────────────────────────────────────────────────────────────
BECalc BreakEvenManager::calcSide(const std::vector<OrderInfo>& orders,
                                   int side) const noexcept
{
    BECalc r;
    const OrderType target = (side == 0) ? OrderType::Buy : OrderType::Sell;

    double sumLP = 0.0;   // Σ(price × lots)
    double sumL  = 0.0;   // Σ(lots)
    double sumC  = 0.0;   // Σ(commission + swap) — valeurs négatives = coûts

    for (const auto& o : orders) {
        if (o.type != target) continue;
        if (o.lots <= 0.0)    continue;
        sumLP += o.openPrice * o.lots;
        sumL  += o.lots;
        sumC  += o.commission + o.swap;
    }

    if (sumL < 1e-6) return r;   // aucune position de ce côté

    r.valid      = true;
    r.avgEntry   = sumLP / sumL;
    r.totalLots  = sumL;

    // ── Coût commissions + swap en points ────────────────────
    // P&L d'une position = (Δprice / point) × lots × tickValue
    // Au break-even total : Σ P&L + sumC = 0
    // Δprice_BE × sumL × tickValue / point = -sumC   (sumC < 0)
    // Δprice_BE = (-sumC) / (sumL × tickValue / point)
    //           = |sumC| × point / (sumL × tickValue)
    double commPts = 0.0;
    if (cfg_.tickValue > 1e-9 && sumL > 1e-6) {
        commPts = std::abs(sumC) / (sumL * cfg_.tickValue) ;
        // commPts est en points bruts (px/point)
        // on le convertit en unités de prix en le gardant tel quel
        // (le level sera avgEntry ± commPts, comme avgEntry est en prix)
    }
    // Buffer de sécurité fixe (beBufferPts) en unités de prix
    double bufferPrice = (double)cfg_.beBufferPts * cfg_.point;

    r.commCostPts = commPts + (double)cfg_.beBufferPts;  // pts totaux

    // ── Niveau final ──────────────────────────────────────────
    // BE-SS (buys, side=0) : sell stop en DESSOUS de avgBuy
    //   → trigger si prix baisse jusqu'ici = les buys couvrent leur coût
    // BE-BS (sells, side=1) : buy stop AU DESSUS de avgSell
    //   → trigger si prix monte jusqu'ici = les sells couvrent leur coût
    if (side == 0) {
        r.level = r.avgEntry - commPts * cfg_.point - bufferPrice;
    } else {
        r.level = r.avgEntry + commPts * cfg_.point + bufferPrice;
    }

    // Normalisation au digit broker
    double factor = std::pow(10.0, cfg_.digits);
    r.level = std::round(r.level * factor) / factor;

    return r;
}

// ────────────────────────────────────────────────────────────
//  recompute — recalcule les deux slots et détecte les changements
// ────────────────────────────────────────────────────────────
bool BreakEvenManager::recompute(const std::vector<OrderInfo>& orders) noexcept
{
    bool changed = false;

    // ── Côté Buy → BE-SS ─────────────────────────────────────
    auto calcSS = calcSide(orders, 0);
    if (calcSS.valid) {
        if (!ssBE_.active || levelChanged(ssBE_.level, calcSS.level)) {
            ssBE_.avgEntry  = calcSS.avgEntry;
            ssBE_.totalLots = calcSS.totalLots;
            ssBE_.level     = calcSS.level;
            ssBE_.active    = true;
            if (ssBE_.ticket > 0) {
                // Invalider le ticket pour forcer un modify/replace
                ssBE_.ticket = -1;
            }
            changed = true;
        }
    } else {
        // Aucune position Buy → désactiver
        if (ssBE_.active) {
            ssBE_.active = false;
            ssBE_.ticket = -1;
            ssBE_.level  = 0.0;
            changed = true;
        }
    }

    // ── Côté Sell → BE-BS ────────────────────────────────────
    auto calcBS = calcSide(orders, 1);
    if (calcBS.valid) {
        if (!bsBE_.active || levelChanged(bsBE_.level, calcBS.level)) {
            bsBE_.avgEntry  = calcBS.avgEntry;
            bsBE_.totalLots = calcBS.totalLots;
            bsBE_.level     = calcBS.level;
            bsBE_.active    = true;
            if (bsBE_.ticket > 0) {
                bsBE_.ticket = -1;
            }
            changed = true;
        }
    } else {
        if (bsBE_.active) {
            bsBE_.active = false;
            bsBE_.ticket = -1;
            bsBE_.level  = 0.0;
            changed = true;
        }
    }

    return changed;
}

// ────────────────────────────────────────────────────────────
//  checkTrigger — détection tick-level (aucune gate HTTP)
//  Retourne le côté déclenché en priorité au plus proche
// ────────────────────────────────────────────────────────────
int BreakEvenManager::checkTrigger(const Tick& tick) const noexcept
{
    // BE-SS : bid descend sous le niveau (buys à break-even)
    bool ssFired = ssBE_.active && ssBE_.level > 0.0
                   && tick.bid <= ssBE_.level;

    // BE-BS : ask monte au-dessus du niveau (sells à break-even)
    bool bsFired = bsBE_.active && bsBE_.level > 0.0
                   && tick.ask >= bsBE_.level;

    if (!ssFired && !bsFired) return 0;
    if (ssFired  && !bsFired) return 1;
    if (!ssFired && bsFired)  return 2;

    // Les deux déclenchés : prioriser le plus proche du mid
    double mid  = (tick.bid + tick.ask) * 0.5;
    double dSS  = std::abs(mid - ssBE_.level);
    double dBS  = std::abs(mid - bsBE_.level);
    return (dSS <= dBS) ? 1 : 2;
}

// ────────────────────────────────────────────────────────────
//  Gestion des tickets
// ────────────────────────────────────────────────────────────
void BreakEvenManager::setSSTicket(int ticket, double level) noexcept {
    ssBE_.ticket = ticket;
    ssBE_.level  = level;
}

void BreakEvenManager::setBSTicket(int ticket, double level) noexcept {
    bsBE_.ticket = ticket;
    bsBE_.level  = level;
}

// ────────────────────────────────────────────────────────────
//  Reset
// ────────────────────────────────────────────────────────────
void BreakEvenManager::resetSS() noexcept {
    ssBE_ = {};
}
void BreakEvenManager::resetBS() noexcept {
    bsBE_ = {};
}
void BreakEvenManager::reset() noexcept {
    ssBE_ = {};
    bsBE_ = {};
}

// ────────────────────────────────────────────────────────────
//  Rapport dashboard
// ────────────────────────────────────────────────────────────
std::string BreakEvenManager::report() const noexcept {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (bsBE_.active && bsBE_.level > 0.0)
        oss << "BE-BS@" << bsBE_.level
            << "(avg=" << bsBE_.avgEntry << " L=" << bsBE_.totalLots << ")  ";
    else
        oss << "BE-BS:inactif  ";
    if (ssBE_.active && ssBE_.level > 0.0)
        oss << "BE-SS@" << ssBE_.level
            << "(avg=" << ssBE_.avgEntry << " L=" << ssBE_.totalLots << ")";
    else
        oss << "BE-SS:inactif";
    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  levelChanged — seuil minimum pour invalider un tick d'ordre
// ────────────────────────────────────────────────────────────
bool BreakEvenManager::levelChanged(double oldLevel,
                                     double newLevel) const noexcept
{
    return std::abs(newLevel - oldLevel) >= changeTol_;
}

} // namespace top4ea
