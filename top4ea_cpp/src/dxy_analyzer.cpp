// ============================================================
//  TOP4EA — dxy_analyzer.cpp  C++20
//  Proxy DXY depuis barres XAU (corrélation inverse)
// ============================================================
#include "../include/dxy_analyzer.hpp"
#include "../include/market_intelligence.hpp"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace top4ea {

// ─── EMA depuis les closes d'une deque de barres ─────────────
double DXYAnalyzer::computeEMA(const std::deque<Bar>& bars, int period) noexcept {
    if ((int)bars.size() < period || period <= 0) return 0.0;
    double alpha = 2.0 / (period + 1.0);
    // Initialise sur la SMA des N premières barres
    double e = 0.0;
    int start = (int)bars.size() - period;
    for (int i = start; i < start + period; ++i)
        e += bars[i].close;
    e /= period;
    // Puis applique EMA sur le reste
    for (int i = start + period; i < (int)bars.size(); ++i)
        e = alpha * bars[i].close + (1.0 - alpha) * e;
    return e;
}

// ─── ADX(14) Wilder — calcul complet depuis barres ────────────
// Nécessite au minimum 2 × period barres pour une stabilisation correcte
double DXYAnalyzer::computeADX(const std::deque<Bar>& bars, int period) noexcept {
    int n = (int)bars.size();
    if (n < period * 2 + 2) return 20.0;  // valeur neutre par défaut

    // Calcul TR, +DM, -DM sur chaque barre
    std::vector<double> trVec, pdmVec, ndmVec;
    trVec.reserve(n - 1);
    pdmVec.reserve(n - 1);
    ndmVec.reserve(n - 1);

    for (int i = 1; i < n; ++i) {
        const Bar& cur  = bars[i];
        const Bar& prev = bars[i - 1];
        double tr  = std::max({ cur.high - cur.low,
                                std::abs(cur.high - prev.close),
                                std::abs(cur.low  - prev.close) });
        double upMove   = cur.high - prev.high;
        double downMove = prev.low  - cur.low;
        double pdm = (upMove > downMove && upMove > 0.0) ? upMove : 0.0;
        double ndm = (downMove > upMove && downMove > 0.0) ? downMove : 0.0;
        trVec.push_back(tr);
        pdmVec.push_back(pdm);
        ndmVec.push_back(ndm);
    }

    int m = (int)trVec.size();
    if (m < period + 1) return 20.0;

    // Wilder smoothing : première valeur = SMA(period)
    double trSmooth = 0.0, pdmSmooth = 0.0, ndmSmooth = 0.0;
    for (int i = 0; i < period; ++i) {
        trSmooth  += trVec[i];
        pdmSmooth += pdmVec[i];
        ndmSmooth += ndmVec[i];
    }

    // DX series pour ADX
    std::vector<double> dxVec;
    for (int i = period; i < m; ++i) {
        // Wilder smoothing
        trSmooth  = trSmooth  - (trSmooth  / period) + trVec[i];
        pdmSmooth = pdmSmooth - (pdmSmooth / period) + pdmVec[i];
        ndmSmooth = ndmSmooth - (ndmSmooth / period) + ndmVec[i];

        double pdi = (trSmooth > 1e-10) ? 100.0 * pdmSmooth / trSmooth : 0.0;
        double ndi = (trSmooth > 1e-10) ? 100.0 * ndmSmooth / trSmooth : 0.0;
        double sum = pdi + ndi;
        double dx  = (sum > 1e-10) ? 100.0 * std::abs(pdi - ndi) / sum : 0.0;
        dxVec.push_back(dx);
    }

    if ((int)dxVec.size() < period) return 20.0;

    // ADX = SMA(DX, period) puis Wilder smooth
    double adx = 0.0;
    for (int i = 0; i < period; ++i) adx += dxVec[i];
    adx /= period;
    for (int i = period; i < (int)dxVec.size(); ++i)
        adx = (adx * (period - 1) + dxVec[i]) / period;

    return adx;
}

// ─── Analyse principale ───────────────────────────────────────
DXYResult DXYAnalyzer::compute(
    const std::deque<Bar>& barsM30,
    const std::deque<Bar>& barsM15) noexcept
{
    DXYResult result;
    result.signal   = DXYSignal::NEUTRAL;
    result.strength = 0.0;

    // Minimum de données requis
    if ((int)barsM30.size() < 50) {
        result.reason = "insuf barres M30";
        return result;
    }

    // EMA20 / EMA50 des closes M30 (proxy XAU = inverse DXY)
    result.ema20 = computeEMA(barsM30, 20);
    result.ema50 = computeEMA(barsM30, 50);

    // ADX(14) sur M15 pour confirmer la tendance DXY
    if ((int)barsM15.size() >= 30)
        result.adx = computeADX(barsM15, 14);
    else
        result.adx = 20.0;  // neutre si pas assez de données

    if (result.ema20 <= 0.0 || result.ema50 <= 0.0) {
        result.reason = "EMA non calculée";
        return result;
    }

    // Direction XAU pour inférer DXY (corrélation inverse)
    // XAU EMA20 < EMA50 → XAU baissier → DXY BULL (dollar fort)
    // XAU EMA20 > EMA50 → XAU haussier → DXY BEAR (dollar faible)
    double emaDiff = result.ema20 - result.ema50;
    double spread  = std::abs(emaDiff);
    double relDiff = (result.ema50 > 0.0) ? spread / result.ema50 * 100.0 : 0.0;

    // Force proportionnelle à l'écart relatif EMA (clampé 0–100)
    // + bonus ADX : si ADX > 25 → tendance confirmée
    double baseStrength = std::min(relDiff * 50.0, 60.0);
    double adxBonus     = 0.0;
    if (result.adx > 35.0)      adxBonus = 40.0;
    else if (result.adx > 25.0) adxBonus = 25.0;
    else if (result.adx > 18.0) adxBonus = 10.0;

    result.strength = std::min(baseStrength + adxBonus, 100.0);

    // Seuil minimal pour affirmer une direction : ADX > 18 ET écart EMA significatif
    bool trendConfirmed = (result.adx > 18.0 && relDiff > 0.01);

    if (!trendConfirmed) {
        result.signal  = DXYSignal::NEUTRAL;
        result.strength = std::min(result.strength, 30.0);
        result.reason  = "DXY NEUTRE (adx=" + std::to_string((int)result.adx)
                       + " emaDiff=" + std::to_string((int)(relDiff * 100)) + "bp)";
        return result;
    }

    // Inversion XAU → DXY
    if (emaDiff < 0.0) {
        // XAU EMA20 < EMA50 : XAU baissier → DXY BULL
        result.signal = DXYSignal::BULL;
        result.reason = "DXY:BULL (XAU EMA20<EMA50 adx="
                      + std::to_string((int)result.adx) + " str="
                      + std::to_string((int)result.strength) + "%)";
    } else {
        // XAU EMA20 > EMA50 : XAU haussier → DXY BEAR
        result.signal = DXYSignal::BEAR;
        result.reason = "DXY:BEAR (XAU EMA20>EMA50 adx="
                      + std::to_string((int)result.adx) + " str="
                      + std::to_string((int)result.strength) + "%)";
    }

    return result;
}

} // namespace top4ea
