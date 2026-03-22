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

} // namespace top4ea::math
