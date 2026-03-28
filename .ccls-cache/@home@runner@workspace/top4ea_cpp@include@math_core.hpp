#pragma once
// ============================================================
//  TOP4EA — math_core.hpp
//  Noyau mathématique : ATR, EWMA, Kelly, détection régime
//  100% C++20, sans dépendance externe
// ============================================================
#include "types.hpp"
#include <deque>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <span>

namespace top4ea::math {

// ────────────────────────────────────────────────────────────
//  EWMA — Moyenne mobile exponentielle pondérée
//  α ∈ (0,1]  →  valeur = α*x + (1-α)*prev
// ────────────────────────────────────────────────────────────
class EWMA {
public:
    explicit EWMA(double alpha, double init = 0.0)
        : alpha_(alpha), value_(init), initialized_(false) {}

    double update(double x) noexcept {
        if (!initialized_) { value_ = x; initialized_ = true; }
        else                value_ = alpha_ * x + (1.0 - alpha_) * value_;
        return value_;
    }

    [[nodiscard]] double value()       const noexcept { return value_; }
    [[nodiscard]] bool   initialized() const noexcept { return initialized_; }
    void reset(double v = 0.0) noexcept { value_ = v; initialized_ = false; }

private:
    double alpha_;
    double value_;
    bool   initialized_;
};

// ────────────────────────────────────────────────────────────
//  ATR — Average True Range (Wilder, période configurable)
//  Flux d'entrée : high, low, close
// ────────────────────────────────────────────────────────────
class ATR {
public:
    explicit ATR(int period) : period_(period) {}

    double update(double high, double low, double prevClose) noexcept {
        double tr = trueRange(high, low, prevClose);
        trBuf_.push_back(tr);
        if ((int)trBuf_.size() > period_)
            trBuf_.pop_front();

        if ((int)trBuf_.size() < period_) {
            current_ = std::accumulate(trBuf_.begin(), trBuf_.end(), 0.0)
                       / (double)trBuf_.size();
        } else {
            if (!initialized_) {
                current_     = std::accumulate(trBuf_.begin(), trBuf_.end(), 0.0)
                               / (double)period_;
                initialized_ = true;
            } else {
                current_ = (current_ * (period_ - 1) + tr) / period_;
            }
        }
        return current_;
    }

    [[nodiscard]] double value()       const noexcept { return current_; }
    [[nodiscard]] bool   ready()       const noexcept { return initialized_; }
    [[nodiscard]] int    period()      const noexcept { return period_; }

    // Convertit ATR en points (pour GridGap)
    [[nodiscard]] double toPoints(double pointSize) const noexcept {
        return (pointSize > 0.0) ? std::round(current_ / pointSize) : 0.0;
    }

private:
    static double trueRange(double h, double l, double pc) noexcept {
        return std::max({ h - l,
                          std::abs(h - pc),
                          std::abs(l - pc) });
    }

    int           period_;
    double        current_     = 0.0;
    bool          initialized_ = false;
    std::deque<double> trBuf_;
};

// ────────────────────────────────────────────────────────────
//  Détection de régime de volatilité
//  Compare ATR courant aux percentiles historiques
// ────────────────────────────────────────────────────────────
class VolatilityRegime {
public:
    explicit VolatilityRegime(int histSize = 100)
        : histSize_(histSize) {}

    void push(double atr) {
        buf_.push_back(atr);
        if ((int)buf_.size() > histSize_)
            buf_.pop_front();
    }

    [[nodiscard]] VolRegime classify(double currentAtr) const noexcept {
        if (buf_.size() < 10) return VolRegime::Medium;
        std::vector<double> sorted(buf_.begin(), buf_.end());
        std::sort(sorted.begin(), sorted.end());
        double p33 = sorted[(int)(sorted.size() * 0.33)];
        double p66 = sorted[(int)(sorted.size() * 0.66)];
        if (currentAtr <= p33) return VolRegime::Low;
        if (currentAtr >= p66) return VolRegime::High;
        return VolRegime::Medium;
    }

private:
    int                histSize_;
    std::deque<double> buf_;
};

// ────────────────────────────────────────────────────────────
//  GridGap dynamique basé ATR
//  gap = clamp(ATR_points * atrMult, gapMin, gapMax)
// ────────────────────────────────────────────────────────────
[[nodiscard]] inline int dynamicGridGap(double atrPoints,
                                        double atrMult,
                                        int    gapMin,
                                        int    gapMax) noexcept {
    return (int)std::clamp(atrPoints * atrMult,
                           (double)gapMin,
                           (double)gapMax);
}

// ────────────────────────────────────────────────────────────
//  Lot de récupération avec fraction Kelly plafonnée
//  lot_n = baseLot * mult^n
//  Kelly cap : lot_n <= baseLot * (1 + kellyFraction * n)
// ────────────────────────────────────────────────────────────
[[nodiscard]] inline double recovLot(double baseLot,
                                     double multiplier,
                                     int    count,
                                     double kellyMaxFraction = 0.25) noexcept {
    double geometric = baseLot;
    for (int i = 0; i < count; ++i)
        geometric *= multiplier;

    double kellyCap = baseLot * (1.0 + kellyMaxFraction * count);
    double lot      = std::min(geometric, kellyCap);
    return std::max(std::round(lot * 100.0) / 100.0, 0.01);
}

// ────────────────────────────────────────────────────────────
//  Vélocité P&L — EWMA sur les variations de profit
//  Permet une fermeture prédictive : si on approche rapidement
//  du target, on ferme avant le retournement
// ────────────────────────────────────────────────────────────
class PnLVelocity {
public:
    explicit PnLVelocity(double alpha = 0.3) : ewma_(alpha) {}

    // Appel à chaque tick — renvoie la vitesse lissée ($/tick)
    double update(double currentPnL) noexcept {
        double delta = currentPnL - prevPnL_;
        prevPnL_     = currentPnL;
        return ewma_.update(delta);
    }

    // Renvoie vrai si la trajectoire atteindra le target
    // dans l'horizon donné (en nombre de ticks)
    [[nodiscard]] bool willReachTarget(double target,
                                       double currentPnL,
                                       int    horizonTicks = 5) const noexcept {
        double v     = ewma_.value();
        double proj  = currentPnL + v * horizonTicks;
        return (v > 0.0 && proj >= target);
    }

    [[nodiscard]] double velocity() const noexcept { return ewma_.value(); }
    void reset() noexcept { prevPnL_ = 0.0; ewma_.reset(); }

private:
    EWMA   ewma_;
    double prevPnL_ = 0.0;
};

// ────────────────────────────────────────────────────────────
//  Ajustement de prix anti-slippage
//  Ajuste le prix de fermeture selon le spread moyen historique
// ────────────────────────────────────────────────────────────
class SpreadTracker {
public:
    explicit SpreadTracker(double alpha = 0.1) : ewma_(alpha) {}

    void push(double spread) noexcept { ewma_.update(spread); }

    // Prix ajusté pour fermeture BUY = Bid - marge ; SELL = Ask + marge
    [[nodiscard]] double adjustedClosePrice(OrderType t, double bid, double ask) const noexcept {
        double avgSpread = ewma_.initialized() ? ewma_.value() : (ask - bid);
        double margin    = avgSpread * 0.5;
        return (t == OrderType::Buy) ? (bid - margin) : (ask + margin);
    }

    [[nodiscard]] double avgSpread() const noexcept { return ewma_.value(); }

private:
    EWMA ewma_;
};

// ────────────────────────────────────────────────────────────
//  Normalisation de prix (équivalent NP() MQL4)
// ────────────────────────────────────────────────────────────
[[nodiscard]] inline double normalize(double price, int digits) noexcept {
    double factor = std::pow(10.0, digits);
    return std::round(price * factor) / factor;
}

// ============================================================
//  [1] OUEstimator — Gap optimal Ornstein-Uhlenbeck
//  Avellaneda & Lee (2010) — Statistical Arbitrage in Equity
//
//  Modèle : dX = θ(μ-X)dt + σdW
//  Gap optimal prouvé : gap_ou = σ_ou / √θ
//
//  Estimation depuis les ticks :
//    ρ₁  = autocorrélation lag-1 des returns
//    θ   = -ln|ρ₁|        (vitesse de mean-reversion)
//    σ²  = σ_r² × 2θ / (1 - e^{-2θ})
//    gap = σ_ou / √θ       en unités de prix
//
//  Actif uniquement si ρ₁ < 0 (marché mean-reverting)
//  Sinon : retourne 0 → fallback ATR grid
// ============================================================
class OUEstimator {
public:
    explicit OUEstimator(int window = 200, double minTheta = 0.05)
        : window_(window), minTheta_(minTheta) {}

    void push(double midPrice) noexcept {
        if ((int)buf_.size() >= window_) buf_.pop_front();
        buf_.push_back(midPrice);
    }

    // Gap optimal en unités de prix (diviser par pointSize pour avoir les points)
    // Retourne 0.0 si marché non mean-reverting ou données insuffisantes
    [[nodiscard]] double optimalGapPrice() const noexcept {
        int minSamples = std::max(20, window_ / 5);
        if ((int)buf_.size() < minSamples) return 0.0;

        // Returns r[i] = p[i] - p[i-1]
        std::vector<double> r;
        r.reserve(buf_.size() - 1);
        auto it   = buf_.begin();
        double prev = *it++;
        while (it != buf_.end()) { r.push_back(*it - prev); prev = *it++; }

        int N = (int)r.size();
        if (N < 10) return 0.0;

        double meanR = std::accumulate(r.begin(), r.end(), 0.0) / N;
        double varR  = 0.0;
        for (double v : r) varR += (v - meanR) * (v - meanR);
        varR /= N;
        if (varR <= 0.0) return 0.0;

        // Autocorrélation lag-1
        double cov = 0.0;
        for (int i = 0; i < N - 1; ++i)
            cov += (r[i] - meanR) * (r[i+1] - meanR);
        cov /= (N - 1);
        double rho = cov / varR;

        if (rho >= 0.0) return 0.0;   // trending → pas de OU

        double theta = -std::log(std::abs(rho));
        if (theta < minTheta_) return 0.0;

        double expFactor = 1.0 - std::exp(-2.0 * theta);
        if (expFactor <= 1e-10) return 0.0;

        double sigmaOU = std::sqrt(varR) * std::sqrt(2.0 * theta / expFactor);
        return sigmaOU / std::sqrt(theta);   // gap optimal en unités de prix
    }

    [[nodiscard]] bool   ready()  const noexcept {
        return (int)buf_.size() >= std::max(20, window_ / 5);
    }
    [[nodiscard]] double rho1()   const noexcept { return lastRho_; }

private:
    int                window_;
    double             minTheta_;
    mutable double     lastRho_ = 0.0;
    std::deque<double> buf_;
};

// ============================================================
//  [2] CPPI — Constant Proportion Portfolio Insurance
//  Black & Perold (1992)
//
//  Protection naturelle contre la ruine :
//    floor   = balance × cppiFloorPct   (équité minimum protégée)
//    cushion = equity - floor
//    lot     = k × cushion / (gap_pts × tickValue)
//
//  Quand equity → floor : cushion → 0 → lot → 0 → arrêt auto
//  Retourne 0.0 si equity < floor (signal d'arrêt)
// ============================================================
[[nodiscard]] inline double cppiLot(
    double equity,
    double balance,
    double cppiFloorPct,
    double cppiMultiplier,
    double baseLot,
    double gapPoints,
    double tickValue
) noexcept {
    double floor   = balance * cppiFloorPct;
    double cushion = equity - floor;
    if (cushion <= 0.0) return 0.0;
    if (gapPoints <= 0.0 || tickValue <= 0.0) return baseLot;

    double raw = (cppiMultiplier * cushion) / (gapPoints * tickValue);
    double lot = std::round(raw * 100.0) / 100.0;
    return std::max(baseLot, lot);
}

// ============================================================
//  [3] SRH — Symmetric Recursive Hedge auto-calibrant
//  Référence : ForexFactory SRH (>500 forks GitHub)
//
//  Principe : le lot de recovery est calculé pour récupérer
//  EXACTEMENT le drawdown total en 1 mouvement de grille.
//
//  lot_srh = drawdown_$ / (gap_pts × tickValue)
//
//  Avantage : zéro multiplicateur arbitraire.
//  Le système est auto-calibrant : plus le drawdown est grand,
//  plus le lot de recovery est grand — proportionnellement.
// ============================================================
[[nodiscard]] inline double srhLot(
    double drawdownUSD,
    double gapPoints,
    double tickValue,
    double baseLot,
    double maxLot = 100.0
) noexcept {
    if (drawdownUSD <= 0.0 || gapPoints <= 0.0 || tickValue <= 0.0)
        return baseLot;
    double raw = drawdownUSD / (gapPoints * tickValue);
    double lot = std::max(baseLot, std::round(raw * 100.0) / 100.0);
    return std::min(lot, maxLot);
}

// ============================================================
//  [4] RollingKelly — Kelly dynamique sur historique glissant
//  Extension de Kelly (1956) / Thorp (1969)
//
//  Recalcule f* sur les N derniers cycles réels fermés :
//    p = wins / N
//    b = avgWin / avgLoss
//    f* = (b*p - q) / b × safetyFactor
//
//  Adapte la taille des lots à la performance RÉELLE du système
//  plutôt qu'à une hypothèse statique de taux de gain.
// ============================================================
class RollingKelly {
public:
    explicit RollingKelly(int historyN = 20, double safetyFactor = 0.25)
        : histN_(historyN), safety_(safetyFactor) {}

    void recordCycle(double realizedPnL) noexcept {
        hist_.push_back(realizedPnL);
        if ((int)hist_.size() > histN_) hist_.pop_front();
    }

    [[nodiscard]] double fraction() const noexcept {
        if ((int)hist_.size() < 5) return safety_;

        int wins = 0; double sumW = 0.0, sumL = 0.0;
        for (double v : hist_) {
            if (v >= 0.0) { ++wins; sumW += v; }
            else           {         sumL += std::abs(v); }
        }
        int N  = (int)hist_.size();
        int ls = N - wins;
        double p = (double)wins / N;
        double q = 1.0 - p;
        if (p <= 1e-9 || q <= 1e-9) return safety_;

        double avgW = (wins > 0) ? sumW / wins : 1e-3;
        double avgL = (ls   > 0) ? sumL / ls   : 1e-3;
        double b    = avgW / avgL;
        double f    = std::max(0.0, (b * p - q) / b);
        return f * safety_;
    }

    [[nodiscard]] bool ready() const noexcept { return (int)hist_.size() >= 5; }
    [[nodiscard]] int  count() const noexcept { return (int)hist_.size(); }

private:
    int                histN_;
    double             safety_;
    std::deque<double> hist_;
};

// ============================================================
//  [5] VABTTarget — Volatility-Adjusted Basket Target
//  Inspiré de Risk Parity (Bridgewater Associates)
//
//  target_vabt = ATR_pts × vabtCoeff × totalLots × tickValue
//
//  Quand la volatilité est haute → target monte (grosses fluctuations)
//  Quand la volatilité est basse → target baisse (gains plus rapides)
//  Résultat : rendement homogène en risque ajusté sur tous régimes.
//  Clamp : [baseTarget × 0.50 ; baseTarget × 3.0]
// ============================================================
[[nodiscard]] inline double vabtTarget(
    double atrPoints,
    double totalLots,
    double tickValue,
    double vabtCoeff,
    double baseTarget
) noexcept {
    if (atrPoints <= 0.0 || totalLots <= 0.0 || tickValue <= 0.0)
        return baseTarget;
    double dyn = atrPoints * vabtCoeff * totalLots * tickValue;
    return std::clamp(dyn, baseTarget * 0.50, baseTarget * 3.0);
}

} // namespace top4ea::math
