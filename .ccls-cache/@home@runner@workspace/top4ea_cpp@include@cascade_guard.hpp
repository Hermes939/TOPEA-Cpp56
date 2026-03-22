#pragma once
// ============================================================
//  TOP4EA — cascade_guard.hpp
//  Garde professionnel contre les faux déclenchements
//
//  Remplace le cooldown temporel naïf par 3 conditions
//  basées sur la réalité du marché :
//
//  [1] DÉPLACEMENT DE PRIX
//      Le prix doit s'être écarté d'au moins priceDispatchPct%
//      du GridGap effectif depuis le dernier déclenchement.
//      → Empêche les re-déclenchements sur le même niveau.
//
//  [2] SPREAD NORMAL
//      Le spread courant ne doit pas dépasser
//      spreadMaxMult × spread_moyen_EWMA.
//      → Bloque les entrées pendant news/spikes/manipulation.
//
//  [3] MOMENTUM CONFIRMÉ
//      Le prix doit avoir bougé dans la direction du
//      déclenchement sur au moins momentumTicks ticks consécutifs
//      avant d'autoriser le recovery suivant.
//      → Filtre les whipsaws et faux cassements.
//
//  Les 3 conditions doivent être TOUTES vraies simultanément.
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <sstream>

namespace top4ea {

// ─── Résultat d'une vérification cascade ────────────────────
struct CascadeCheckResult {
    bool   allowed          = false;
    bool   priceOk          = false;
    bool   spreadOk         = false;
    bool   momentumOk       = false;
    double priceDisplacement= 0.0;   // déplacement réel en points
    double requiredDisplacement=0.0; // déplacement requis en points
    double currentSpread    = 0.0;
    double avgSpread        = 0.0;
    int    momentumCount    = 0;     // ticks consécutifs dans la direction
    int    momentumRequired = 0;

    [[nodiscard]] std::string explain() const {
        std::ostringstream oss;
        oss << "CascadeGuard ["
            << (allowed ? "OK" : "BLOQUÉ") << "]"
            << " | Prix: " << (priceOk ? "✓" : "✗")
            << " (" << priceDisplacement << "/" << requiredDisplacement << " pts)"
            << " | Spread: " << (spreadOk ? "✓" : "✗")
            << " (" << currentSpread << "/" << avgSpread*2 << ")"
            << " | Momentum: " << (momentumOk ? "✓" : "✗")
            << " (" << momentumCount << "/" << momentumRequired << " ticks)";
        return oss.str();
    }
};

// ============================================================
//  CascadeGuard — gardien professionnel
// ============================================================
class CascadeGuard {
public:
    struct Params {
        double priceDispatchPct  = 0.50; // 50% du GridGap requis comme déplacement min
        double spreadMaxMult     = 2.5;  // spread courant ≤ 2.5× spread moyen
        int    momentumTicks     = 3;    // N ticks consécutifs dans la direction
        double spreadEwmaAlpha   = 0.05; // lissage lent pour spread de référence
    };

    explicit CascadeGuard(Params p = {}) : params_(p), spreadEwma_(p.spreadEwmaAlpha) {}

    // ─── Appel à chaque tick ─────────────────────────────────
    void onTick(const Tick& tick) noexcept {
        std::lock_guard lk(mu_);

        // Mise à jour spread EWMA
        spreadEwma_.update(tick.spread);

        // Mise à jour momentum
        updateMomentum(tick);

        prev_ = tick;
    }

    // ─── Vérification avant d'autoriser un nouveau recovery ──
    // direction : +1 = BUY déclenchement (prix monte)
    //             -1 = SELL déclenchement (prix descend)
    // effectiveGridGap : GridGap courant en points
    // pointSize : taille d'un point
    [[nodiscard]] CascadeCheckResult check(
        int    direction,
        double lastTriggerPrice,
        double currentBid,
        double effectiveGridGap,
        double pointSize,
        double currentSpread) const noexcept
    {
        std::lock_guard lk(mu_);
        CascadeCheckResult r;

        // ── Condition 1 : déplacement de prix ────────────────
        double requiredPts = effectiveGridGap * params_.priceDispatchPct;
        double displacement= 0.0;
        if (lastTriggerPrice > 0.0) {
            displacement = std::abs(currentBid - lastTriggerPrice) / pointSize;
        } else {
            displacement = requiredPts; // premier déclenchement → toujours autorisé
        }
        r.requiredDisplacement = requiredPts;
        r.priceDisplacement    = displacement;
        r.priceOk              = (displacement >= requiredPts);

        // ── Condition 2 : spread normal ───────────────────────
        double avgSpread = spreadEwma_.initialized()
                           ? spreadEwma_.value()
                           : currentSpread;
        r.currentSpread = currentSpread / pointSize;
        r.avgSpread     = avgSpread / pointSize;
        r.spreadOk = (avgSpread <= 0.0)
                     || (currentSpread <= avgSpread * params_.spreadMaxMult);

        // ── Condition 3 : momentum confirmé ──────────────────
        int dir = direction; // +1 ou -1
        r.momentumRequired = params_.momentumTicks;
        r.momentumCount    = getMomentumCount(dir);
        r.momentumOk       = (r.momentumCount >= params_.momentumTicks);

        // ── Résultat global ───────────────────────────────────
        r.allowed = r.priceOk && r.spreadOk && r.momentumOk;
        return r;
    }

    // Réinitialise après fermeture basket ou reset
    void reset() noexcept {
        std::lock_guard lk(mu_);
        momentumBuf_.clear();
        lastTriggerPrice_ = 0.0;
    }

    void setLastTriggerPrice(double price) noexcept {
        std::lock_guard lk(mu_);
        lastTriggerPrice_ = price;
    }

    [[nodiscard]] double lastTriggerPrice() const noexcept {
        std::lock_guard lk(mu_);
        return lastTriggerPrice_;
    }

    [[nodiscard]] double avgSpread() const noexcept {
        return spreadEwma_.value();
    }

private:
    void updateMomentum(const Tick& tick) noexcept {
        if (prev_.bid == 0.0) return;
        double delta = tick.bid - prev_.bid;
        if      (delta > 0.0) momentumBuf_.push_back(+1);
        else if (delta < 0.0) momentumBuf_.push_back(-1);
        else                  momentumBuf_.push_back( 0);

        // Garder seulement les N derniers ticks
        int maxBuf = params_.momentumTicks * 4;
        while ((int)momentumBuf_.size() > maxBuf)
            momentumBuf_.pop_front();
    }

    // Compte les ticks consécutifs dans la direction donnée
    // en partant de la fin du buffer
    [[nodiscard]] int getMomentumCount(int dir) const noexcept {
        int count = 0;
        for (int i = (int)momentumBuf_.size() - 1; i >= 0; --i) {
            if (momentumBuf_[i] == dir) ++count;
            else                        break;
        }
        return count;
    }

    Params             params_;
    math::EWMA         spreadEwma_;
    std::deque<int>    momentumBuf_;   // +1 / 0 / -1 par tick
    Tick               prev_;
    double             lastTriggerPrice_ = 0.0;
    mutable std::mutex mu_;
};

} // namespace top4ea
