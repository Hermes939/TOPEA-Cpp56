#pragma once
// ============================================================
//  TOP4EA — cascade_guard.hpp
//  Garde professionnel pour HEDGING GRID + RECOVERY MARTINGALE
//
//  ⚠ Ce système NE prédit PAS la direction du marché.
//  Il place des stops des DEUX côtés simultanément.
//  Le garde protège contre les dégradations de qualité
//  du marché, PAS contre une direction particulière.
//
//  3 CONDITIONS basées sur la qualité du mouvement :
//
//  [1] DÉPLACEMENT MINIMUM DEPUIS LE DERNIER TRIGGER
//      Le prix doit s'être écarté d'au moins X% du GridGap
//      depuis le niveau du dernier trigger.
//      → Empêche les déclenchements en chaîne sur des micro-
//        oscillations autour d'un même niveau de prix.
//
//  [2] SPREAD NORMAL
//      Le spread ne doit pas dépasser N× le spread moyen EWMA.
//      → Bloque les entrées pendant news/spikes/manipulation
//        où le spread s'élargit artificiellement.
//
//  [3] ANTI-WHIPSAW : PAS DE RETOUR SUR LE NIVEAU
//      Après un trigger SS à prix P, si le marché remonte
//      au-dessus de P → c'était un faux cassement (whipsaw).
//      Même logique côté BS.
//      → Condition purement mécanique, sans prédiction de
//        direction. Le mouvement doit "tenir" son niveau.
//
//  Les 3 conditions doivent être TOUTES vraies.
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <sstream>
#include <cmath>

namespace top4ea {

// ─── Résultat détaillé d'une vérification ───────────────────
struct CascadeCheckResult {
    bool   allowed              = false;
    bool   priceDispatchOk      = false;  // condition 1
    bool   spreadOk             = false;  // condition 2
    bool   noWhipsawOk          = false;  // condition 3
    double priceDisplacement    = 0.0;    // pts depuis dernier trigger
    double requiredDisplacement = 0.0;    // pts requis
    double currentSpreadPts     = 0.0;    // spread courant en points
    double avgSpreadPts         = 0.0;    // spread moyen EWMA en points
    double triggerPrice         = 0.0;    // prix du dernier trigger
    double currentPrice         = 0.0;    // prix courant
    int    side                 = 0;      // 0=BUY / 1=SELL

    [[nodiscard]] std::string explain() const {
        std::ostringstream oss;
        oss << "CascadeGuard [" << (allowed ? "OK" : "BLOQUÉ") << "]"
            << " | [1]Déplacement: " << (priceDispatchOk ? "✓" : "✗")
            << " (" << priceDisplacement << "/" << requiredDisplacement << "pts)"
            << " | [2]Spread: " << (spreadOk ? "✓" : "✗")
            << " (" << currentSpreadPts << "/" << avgSpreadPts*2.5 << "pts)"
            << " | [3]Whipsaw: " << (noWhipsawOk ? "✓" : "✗")
            << " (prix=" << currentPrice << " trigger=" << triggerPrice << ")";
        return oss.str();
    }
};

// ─── Paramètres du CascadeGuard (défini avant la classe) ────
struct CascadeGuardParams {
    double priceDispatchPct  = 0.50;  // 50% du GridGap requis comme déplacement min
    int    priceDispatchPips = 15;    // déplacement fixe en pips (0 = utilise pct)
    //   XAUUSDm digits=2 : 1 pip = 10 points → 15 pips = 150 points
    //   Forex   digits=5 : 1 pip = 10 points → 15 pips = 150 points
    double spreadMaxMult    = 2.5;    // spread max = 2.5× spread moyen EWMA
    double spreadEwmaAlpha  = 0.05;   // lissage très lent pour spread de référence
};

// ============================================================
//  CascadeGuard
// ============================================================
class CascadeGuard {
public:
    using Params = CascadeGuardParams;

    explicit CascadeGuard(Params p = Params{})
        : params_(p), spreadEwma_(p.spreadEwmaAlpha) {}

    // ─── Appel à chaque tick — alimente le spread EWMA ──────
    void onTick(const Tick& tick, double pointSize) noexcept {
        std::lock_guard lk(mu_);
        spreadEwma_.update(tick.spread);
        lastBid_   = tick.bid;
        lastSpread_= tick.spread;
        pointSize_ = pointSize;
    }

    // ─── Vérification avant un nouveau recovery ──────────────
    //
    // side          : 0 = BuyStop déclenché  (prix a monté → SS side)
    //                 1 = SellStop déclenché (prix a descendu → BS side)
    // triggerPrice  : niveau du stop qui vient de se déclencher
    // gridGap       : GridGap effectif en points
    //
    // Aucune prédiction de direction. Uniquement qualité du marché.
    [[nodiscard]] CascadeCheckResult check(
        int    side,
        double triggerPrice,
        double gridGap) const noexcept
    {
        std::lock_guard lk(mu_);
        CascadeCheckResult r;
        r.side         = side;
        r.triggerPrice = triggerPrice;
        r.currentPrice = lastBid_;

        // ── [1] Déplacement depuis le dernier trigger ─────────
        // Mode pips fixe (prioritaire) : 1 pip = 10 points (tous symboles)
        // Mode % GridGap (fallback)    : gridGap × priceDispatchPct
        double requiredPts = (params_.priceDispatchPips > 0)
            ? params_.priceDispatchPips * 10.0
            : gridGap * params_.priceDispatchPct;
        double displacement = (lastTriggerPrice_ > 0.0 && pointSize_ > 0.0)
            ? std::abs(lastBid_ - lastTriggerPrice_) / pointSize_
            : requiredPts; // premier trigger → toujours ok
        r.requiredDisplacement = requiredPts;
        r.priceDisplacement    = displacement;
        r.priceDispatchOk      = (displacement >= requiredPts);

        // ── [2] Spread normal ─────────────────────────────────
        double avgSpread = spreadEwma_.initialized()
                           ? spreadEwma_.value()
                           : lastSpread_;
        double pts = (pointSize_ > 0.0) ? 1.0 / pointSize_ : 1.0;
        r.currentSpreadPts = lastSpread_ * pts;
        r.avgSpreadPts     = avgSpread   * pts;
        r.spreadOk = (avgSpread <= 0.0)
                     || (lastSpread_ <= avgSpread * params_.spreadMaxMult);

        // ── [3] Anti-whipsaw : le prix tient son niveau ───────
        // Un stop SS (vente, déclenché quand prix descend) doit
        // rester EN DESSOUS du niveau de trigger → prix < triggerPrice
        // Un stop BS (achat, déclenché quand prix monte) doit
        // rester AU DESSUS du niveau de trigger → prix > triggerPrice
        // Si le prix est revenu de l'autre côté = whipsaw = bloquer.
        if (triggerPrice <= 0.0) {
            r.noWhipsawOk = true; // pas de référence → premier trigger ok
        } else if (side == 1) {
            // SellStop : prix doit rester < triggerPrice
            // (le marché a bien continué à descendre)
            r.noWhipsawOk = (lastBid_ <= triggerPrice);
        } else {
            // BuyStop : prix doit rester > triggerPrice
            // (le marché a bien continué à monter)
            r.noWhipsawOk = (lastBid_ >= triggerPrice);
        }

        r.allowed = r.priceDispatchOk && r.spreadOk && r.noWhipsawOk;
        return r;
    }

    // ─── Enregistre le prix au moment où un recovery est validé
    void recordTrigger(double price) noexcept {
        std::lock_guard lk(mu_);
        lastTriggerPrice_ = price;
    }

    void reset() noexcept {
        std::lock_guard lk(mu_);
        lastTriggerPrice_ = 0.0;
    }

    [[nodiscard]] double avgSpreadRaw() const noexcept {
        return spreadEwma_.initialized() ? spreadEwma_.value() : lastSpread_;
    }

private:
    Params             params_;
    math::EWMA         spreadEwma_;
    double             lastBid_          = 0.0;
    double             lastSpread_       = 0.0;
    double             pointSize_        = 0.00001;
    double             lastTriggerPrice_ = 0.0;
    mutable std::mutex mu_;
};

} // namespace top4ea
