// ============================================================
//  TOP4EA — market_intelligence.cpp  C++20
//  Assistante Visuelle AI + iniMTA Multi-Timeframe
// ============================================================
#include "../include/market_intelligence.hpp"
#include <iostream>
#include <numeric>
#include <ctime>
#include <cassert>

namespace top4ea {

using SysClock = std::chrono::system_clock;
using SysTP    = std::chrono::time_point<SysClock>;

// ─── Durées des timeframes en secondes ──────────────────────
static constexpr int TF_M1  = 60;
static constexpr int TF_M5  = 300;
static constexpr int TF_M15 = 900;
static constexpr int TF_M30 = 1800;

// ============================================================
//  Helpers statistiques internes
// ============================================================
namespace {

// EMA rapide depuis un vecteur de valeurs
double ema(const std::vector<double>& v, int period) noexcept {
    if (v.empty() || period <= 0) return 0.0;
    double alpha = 2.0 / (period + 1.0);
    double e = v.front();
    for (size_t i = 1; i < v.size(); ++i)
        e = alpha * v[i] + (1.0 - alpha) * e;
    return e;
}

// SMA simple
double sma(const std::vector<double>& v) noexcept {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

// Écart-type populationnelle
double stddev(const std::vector<double>& v, double mean) noexcept {
    if (v.size() < 2) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += (x - mean) * (x - mean);
    return std::sqrt(sum / v.size());
}

} // anon

// ============================================================
//  Initialisation
// ============================================================
void MarketIntelligence::resetPartials() noexcept {
    auto now = SysClock::now();
    curM1_ = curM5_ = curM15_ = curM30_ = Bar{};
    m1Start_ = m5Start_ = m15Start_ = m30Start_ = now;
}

// ============================================================
//  warmUp — réchauffement synthétique (cold-start fix)
//  Pré-popule les déques de barres M5 / M15 / M30 avec des
//  bougies synthétiques réalistes pour que les indicateurs
//  (ATR, RSI, MACD, BB) soient opérationnels dès le 1er tick.
//
//  Modèle : marche aléatoire centrée avec volatilité ≈ atrPrice.
//  Les barres synthétiques sont moins précises que les vraies,
//  mais garantissent des valeurs non-nulles immédiatement.
//  Elles sont progressivement remplacées par des barres réelles.
// ============================================================
void MarketIntelligence::warmUp(double price, double atrPrice, int nBars) noexcept {
    if (price <= 0.0 || atrPrice <= 0.0 || nBars <= 0) return;

    // Pseudo-générateur déterministe (reproductible, sans <random>)
    // LCG léger — suffisant pour les barres synthétiques
    uint64_t seed = 123456789ULL + (uint64_t)(price * 100.0);
    auto rng = [&]() -> double {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)(seed >> 33) / (double)(1ULL << 31) - 1.0; // [-1, +1]
    };

    // Génère nBars bougies cohérentes (chaque close est l'open de la suivante)
    double cur = price;
    auto now   = std::chrono::system_clock::now();

    // Remplir M5, M15, M30 simultanément (même jeu de bougies synthétiques)
    // M30 est la priorité pour activer DeepSeek rapidement
    for (int i = 0; i < nBars; ++i) {
        double drift   = rng() * atrPrice * 0.15;   // légère tendance aléatoire
        double intraVol= atrPrice * (0.4 + std::abs(rng()) * 0.6);

        Bar b;
        b.open  = cur;
        b.close = cur + drift;
        b.high  = std::max(b.open, b.close) + std::abs(rng()) * intraVol * 0.5;
        b.low   = std::min(b.open, b.close) - std::abs(rng()) * intraVol * 0.5;
        b.ticks = 10;  // marqué comme barre avec données
        // Timestamp décroissant (la barre la plus ancienne en premier)
        b.time  = now - std::chrono::minutes((nBars - i) * 30);

        m30Bars_.push_back(b);
        if ((int)m30Bars_.size() > MAX_M30) m30Bars_.pop_front();

        // M15 : 2 barres par barre M30
        for (int j = 0; j < 2; ++j) {
            Bar b15;
            double m = (j == 0) ? b.open : ((b.open + b.close) * 0.5);
            double e = (j == 0) ? ((b.open + b.close) * 0.5) : b.close;
            b15.open  = m;
            b15.close = e;
            b15.high  = std::max(m, e) + std::abs(rng()) * intraVol * 0.25;
            b15.low   = std::min(m, e) - std::abs(rng()) * intraVol * 0.25;
            b15.ticks = 5;
            b15.time  = b.time + std::chrono::minutes(j * 15);
            m15Bars_.push_back(b15);
            if ((int)m15Bars_.size() > MAX_M15) m15Bars_.pop_front();
        }

        // M5 : 6 barres par barre M30
        for (int j = 0; j < 6; ++j) {
            Bar b5;
            double frac   = (double)j / 6.0;
            double fracN  = (double)(j+1) / 6.0;
            b5.open  = b.open  + (b.close - b.open) * frac;
            b5.close = b.open  + (b.close - b.open) * fracN;
            b5.high  = std::max(b5.open, b5.close) + std::abs(rng()) * intraVol * 0.1;
            b5.low   = std::min(b5.open, b5.close) - std::abs(rng()) * intraVol * 0.1;
            b5.ticks = 3;
            b5.time  = b.time + std::chrono::minutes(j * 5);
            m5Bars_.push_back(b5);
            if ((int)m5Bars_.size() > MAX_M5) m5Bars_.pop_front();
        }

        cur = b.close;  // enchaîner les barres
    }

    std::cout << "[WarmUp] " << nBars << " bougies synthétiques injectées"
              << " | prix=" << price << " | ATR≈" << atrPrice
              << " | M5=" << m5Bars_.size()
              << " M15=" << m15Bars_.size()
              << " M30=" << m30Bars_.size() << "\n";
}

// ============================================================
//  update — appelé à chaque tick
// ============================================================
void MarketIntelligence::update(const Tick& tick, double /*pointSize*/,
                                 double basketPnl, int openOrders,
                                 double bsLots, double ssLots,
                                 bool inRecovery) noexcept {
    if (tick.bid <= 0.0 || tick.ask <= 0.0) return;

    // Spread courant + EWMA
    curSpread_ = tick.spread;
    if (spreadEwma_ <= 0.0) spreadEwma_ = tick.spread;
    else spreadEwma_ = 0.05 * tick.spread + 0.95 * spreadEwma_;

    // Cache contexte panier (thread-safe : écrit avant pushTick/closeBars)
    basketPnlCache_  = basketPnl;
    openOrdersCache_ = openOrders;
    bsLotsCache_     = bsLots;
    ssLotsCache_     = ssLots;
    inRecoveryCache_ = inRecovery;

    pushTick(tick);
    closeBars(tick);
    ++tickCount_;

    // Recalcul décision toutes les 10 ticks
    if (tickCount_ % 10 == 0) {
        VisualDecision vd = analyze();
        std::lock_guard lk(mu_);
        lastDecision_ = vd;
    }

    // ── Appel DeepSeek LLM — toutes les 5 ticks (non-bloquant) ──
    // Fréquence augmentée (15→5) pour que DeepSeek ne s'endorme jamais.
    // En mode recovery actif, appel encore plus fréquent (toujours si tick%3==1).
    // forceWake() est appelé toutes les 20 ticks pour bypass si signal périmé.
    // Garde : requiert au moins 5 barres M30 (réduit de 26 → accélère le démarrage)
    bool m30Ready = ((int)m30Bars_.size() >= 5);
    int dsTickMod = inRecovery ? 3 : 5;  // plus réactif en recovery
    if (deepseek_.hasApiKey() && m30Ready &&
        ((tickCount_ % dsTickMod == 1) || (tickCount_ % 20 == 0))) {
        // Snapshot indicateurs pour le prompt
        std::deque<Bar> bM5, bM30;
        double se;
        {
            std::lock_guard lk(mu_);
            bM5  = m5Bars_;
            bM30 = m30Bars_;
            se   = spreadEwma_;
        }
        Bar c5, cM30;
        { std::lock_guard lk(mu_); c5 = curM5_; cM30 = curM30_; }
        if (c5.ticks  > 0) bM5.push_back(c5);
        if (cM30.ticks > 0) bM30.push_back(cM30);

        IndiSet im5  = computeIndicators(bM5);
        IndiSet im30 = computeIndicators(bM30);
        TradingSession ses = detectSession();
        MarketStructure st = analyzeStructure(bM5);

        DSMarketContext ctx;
        ctx.session      = sessionName(ses);
        ctx.structure    = structureName(st);
        ctx.bid          = tick.bid;
        ctx.ask          = tick.ask;
        ctx.spreadRatio  = (se > 1e-10 && curSpread_ > 0.0)
                           ? curSpread_ / se : 1.0;
        ctx.m5Atr       = im5.atr;
        ctx.m5Rsi       = im5.rsi;
        ctx.m5Macd      = im5.macdLine;
        ctx.m5MacdHist  = im5.macdHist;
        ctx.m5BbWidth   = im5.bbWidth * 100.0;
        ctx.m5Momentum  = im5.momentum;
        ctx.m30Atr      = im30.atr;
        ctx.m30Rsi      = im30.rsi;
        ctx.m30Macd     = im30.macdLine;
        ctx.m30MacdHist = im30.macdHist;
        ctx.basketPnl   = basketPnl;
        ctx.openOrders  = openOrders;
        ctx.bsLots      = bsLots;
        ctx.ssLots      = ssLots;
        ctx.netDelta    = bsLots - ssLots;
        ctx.inRecovery  = inRecovery;

        // Réveil forcé si signal trop vieux (prioritaire sur requestAnalysis normal)
        // forceWake bypasse le throttle et relance immédiatement si périmé
        try {
            deepseek_.forceWake(ctx);      // réveil si signal > 30s
            deepseek_.requestAnalysis(ctx); // appel normal (throttlé à 5s)
        }
        catch (...) { /* thread non-créé — pas critique */ }
    }
}

// ============================================================
//  pushTick — intègre un tick dans les barres en cours
// ============================================================
void MarketIntelligence::pushTick(const Tick& tick) noexcept {
    double mid = (tick.bid + tick.ask) * 0.5;

    auto update_bar = [mid](Bar& bar) noexcept {
        if (bar.ticks == 0) {
            bar.open = bar.high = bar.low = bar.close = mid;
        } else {
            if (mid > bar.high) bar.high = mid;
            if (mid < bar.low)  bar.low  = mid;
            bar.close = mid;
        }
        ++bar.ticks;
    };

    update_bar(curM1_);
    update_bar(curM5_);
    update_bar(curM15_);
    update_bar(curM30_);
}

// ============================================================
//  closeBars — clôture les barres dont le temps est écoulé
// ============================================================
void MarketIntelligence::closeBars(const Tick& /*tick*/) noexcept {
    auto now   = SysClock::now();
    auto secs  = [&](const SysTP& start, int tf) {
        return std::chrono::duration_cast<std::chrono::seconds>(
            now - start).count() >= tf;
    };

    auto push = [](std::deque<Bar>& q, Bar& cur, SysTP& start,
                   int tf, int max) noexcept {
        if (cur.ticks > 0) {
            cur.time = start;
            q.push_back(cur);
            if ((int)q.size() > max) q.pop_front();
        }
        cur = Bar{};
        start = std::chrono::system_clock::now();
        (void)tf;
    };

    if (secs(m1Start_,  TF_M1))  push(m1Bars_,  curM1_,  m1Start_,  TF_M1,  MAX_M1);
    if (secs(m5Start_,  TF_M5))  push(m5Bars_,  curM5_,  m5Start_,  TF_M5,  MAX_M5);
    if (secs(m15Start_, TF_M15)) push(m15Bars_, curM15_, m15Start_, TF_M15, MAX_M15);
    if (secs(m30Start_, TF_M30)) push(m30Bars_, curM30_, m30Start_, TF_M30, MAX_M30);
}

// ============================================================
//  computeATR(14)
// ============================================================
double MarketIntelligence::computeATR(
    const std::deque<Bar>& bars, int period) const noexcept
{
    int n = (int)bars.size();
    if (n < 2) return 0.0;
    int start = std::max(0, n - period * 3);
    double atr = 0.0;
    double alpha = 2.0 / (period + 1.0);
    bool first = true;
    for (int i = start + 1; i < n; ++i) {
        double tr = bars[i].range();
        double tr2 = std::abs(bars[i].high - bars[i-1].close);
        double tr3 = std::abs(bars[i].low  - bars[i-1].close);
        double trueRange = std::max({tr, tr2, tr3});
        if (first) { atr = trueRange; first = false; }
        else        atr = alpha * trueRange + (1.0 - alpha) * atr;
    }
    return atr;
}

// ============================================================
//  computeRSI(14)
// ============================================================
double MarketIntelligence::computeRSI(
    const std::deque<Bar>& bars, int period) const noexcept
{
    int n = (int)bars.size();
    if (n < period + 1) return 50.0;

    int start = std::max(0, n - period * 3);
    double avgGain = 0.0, avgLoss = 0.0;
    bool seeded = false;

    for (int i = start + 1; i < n; ++i) {
        double diff = bars[i].close - bars[i-1].close;
        double gain = diff > 0 ? diff : 0.0;
        double loss = diff < 0 ? -diff : 0.0;
        if (!seeded) {
            avgGain = gain; avgLoss = loss; seeded = true;
        } else {
            avgGain = (avgGain * (period - 1) + gain) / period;
            avgLoss = (avgLoss * (period - 1) + loss) / period;
        }
    }
    if (avgLoss < 1e-12) return 100.0;
    double rs = avgGain / avgLoss;
    return 100.0 - 100.0 / (1.0 + rs);
}

// ============================================================
//  computeMACD(12,26,9)
// ============================================================
std::pair<double,double> MarketIntelligence::computeMACD(
    const std::deque<Bar>& bars) const noexcept
{
    int n = (int)bars.size();
    if (n < 26) return {0.0, 0.0};

    std::vector<double> closes;
    closes.reserve(n);
    for (const auto& b : bars) closes.push_back(b.close);

    // EMA12 et EMA26 sur les dernières 60 barres max
    int wnd = std::min(n, 60);
    std::vector<double> recent(closes.end() - wnd, closes.end());

    double ema12 = ema(recent, 12);
    double ema26 = ema(recent, 26);
    double macdLine = ema12 - ema26;

    // Signal = EMA9 de la MACD line (approximé sur 27 points)
    int sigWnd = std::min(n - 26, 27);
    if (sigWnd < 2) return {macdLine, macdLine};

    std::vector<double> macdHist;
    macdHist.reserve(sigWnd);
    for (int i = n - sigWnd; i < n; ++i) {
        std::vector<double> sub(closes.begin() + std::max(0, i - 59),
                                closes.begin() + i + 1);
        double e12 = ema(sub, 12);
        double e26 = ema(sub, 26);
        macdHist.push_back(e12 - e26);
    }
    double signal = ema(macdHist, 9);
    return {macdLine, signal};
}

// ============================================================
//  computeBB(20, 2σ)
// ============================================================
std::tuple<double,double,double> MarketIntelligence::computeBB(
    const std::deque<Bar>& bars, int period) const noexcept
{
    int n = (int)bars.size();
    if (n < period) return {0.0, 0.0, 0.0};

    std::vector<double> closes;
    closes.reserve(period);
    for (int i = n - period; i < n; ++i)
        closes.push_back(bars[i].close);

    double mid = sma(closes);
    double sd  = stddev(closes, mid);
    return {mid + 2.0 * sd, mid, mid - 2.0 * sd};
}

// ============================================================
//  computeIndicators — assemble IndiSet pour un TF
// ============================================================
IndiSet MarketIntelligence::computeIndicators(
    const std::deque<Bar>& bars) const noexcept
{
    IndiSet s;
    s.barCount = (int)bars.size();
    if (s.barCount < 2) return s;

    s.atr = computeATR(bars, 14);
    s.rsi = computeRSI(bars, 14);

    auto [ml, sig] = computeMACD(bars);
    s.macdLine = ml; s.macdSignal = sig;
    s.macdHist = ml - sig;

    auto [bbu, bbm, bbl] = computeBB(bars, 20);
    s.bbUpper = bbu; s.bbMid = bbm; s.bbLower = bbl;
    s.bbWidth = (bbm > 0.0) ? (bbu - bbl) / bbm : 0.0;

    // Momentum(10) : (close - close[-10]) / close[-10]
    int n = s.barCount;
    if (n >= 11) {
        double c0  = bars[n-1].close;
        double c10 = bars[n-11].close;
        s.momentum = (c10 > 0.0) ? (c0 - c10) / c10 : 0.0;
    }

    return s;
}

// ============================================================
//  analyzeStructure — HH/HL/LH/LL + breakout + piège
// ============================================================
MarketStructure MarketIntelligence::analyzeStructure(
    const std::deque<Bar>& bars) const noexcept
{
    int n = (int)bars.size();
    if (n < 6) return MarketStructure::UNKNOWN;

    // Derniers 6 pivots high/low
    auto& b = bars;

    // Swings simples : comparer par groupes de 2
    double h1 = b[n-1].high, h2 = b[n-3].high, h3 = b[n-5].high;
    double l1 = b[n-1].low,  l2 = b[n-3].low,  l3 = b[n-5].low;

    bool hhhl = (h1 > h2 && h2 > h3) && (l1 > l2 && l2 > l3);
    bool lhll = (h1 < h2 && h2 < h3) && (l1 < l2 && l2 < l3);

    // ATR du dernier groupe pour distinguer range serré
    double atr = computeATR(bars, 14);
    double rangeBar = (n > 0) ? bars[n-1].range() : 0.0;

    if (hhhl) return MarketStructure::BULLISH_TREND;
    if (lhll) return MarketStructure::BEARISH_TREND;

    // Range / oscillation
    double avgRange = 0.0;
    int window = std::min(n, 10);
    for (int i = n - window; i < n; ++i) avgRange += b[i].range();
    avgRange /= window;

    if (atr > 0.0 && avgRange < 0.5 * atr) return MarketStructure::RANGE_TIGHT;

    // Piège — fausse cassure : barre précédente très grande + retournement
    if (n >= 3) {
        bool bullTrap = (b[n-2].range() > 2.0 * atr) &&
                        (b[n-1].close < b[n-2].open);
        bool bearTrap = (b[n-2].range() > 2.0 * atr) &&
                        (b[n-1].close > b[n-2].open);
        if (bullTrap) return MarketStructure::TRAP_BULL;
        if (bearTrap) return MarketStructure::TRAP_BEAR;
    }

    // Breakout
    if (n >= 20) {
        double highest = 0.0, lowest = 1e9;
        for (int i = n-20; i < n-1; ++i) {
            if (b[i].high > highest) highest = b[i].high;
            if (b[i].low  < lowest)  lowest  = b[i].low;
        }
        if (b[n-1].close > highest) return MarketStructure::BREAKOUT_UP;
        if (b[n-1].close < lowest)  return MarketStructure::BREAKOUT_DOWN;
    }

    return MarketStructure::RANGE_WIDE;
    (void)rangeBar;
}

// ============================================================
//  computeDecision — Visual AI core
//
//  Raisonnement institutionnel :
//    1. Session filter (liquidité)
//    2. Spread filter (news/spike)
//    3. Structure (trend / range / trap)
//    4. Multi-TF confirmation M5 + M15 + M30 (3 TF requis)
//    5. RSI + MACD cross-TF
//    6. BB squeeze
//    7. Décision trailing (Aggressive / Natural / Freeze)
//    8. ── AUDIT RECOVERY MATHÉMATIQUE ──────────────────────────
//       Score 0–100 sur 5 facteurs indépendants :
//         F1 (+25) Spread normal (ratio ≤ 1.5)
//         F2 (+20) Structure non-piège
//         F3 (+25) Momentum suffisant (marché en mouvement)
//         F4 (+15) M30 non compressé (BB non-squeeze)
//         F5 (+15) RSI M5+M30 non extrême (pas en retournement)
//       seuil : allowRecovery = (score ≥ 55)
//       Surcharge basket : si openOrders ≥ 4 → seuil rehaussé à 70
// ============================================================
VisualDecision MarketIntelligence::computeDecision(
    const IndiSet& m1, const IndiSet& m5,
    const IndiSet& m15, const IndiSet& m30,
    MarketStructure structure,
    TradingSession  session,
    double spreadRatio,
    double basketPnl,
    int    openOrders) const noexcept
{
    VisualDecision vd;
    vd.structure = structure;
    vd.session   = session;
    vd.m1 = m1; vd.m5 = m5; vd.m15 = m15; vd.m30 = m30;

    std::string reason;
    double conf = 0.0;

    // ── [1] Session ────────────────────────────────────────────
    if (session == TradingSession::CLOSED) {
        vd.decision  = MarketDecision::NO_TRADE;
        vd.reasoning = "Session fermée — aucun trade";
        vd.confidence = 100.0;
        vd.allowRecovery = false;
        vd.recoveryScore = 0.0;
        vd.recoveryReason = "Session fermée";
        return vd;
    }
    if (session == TradingSession::TOKYO) {
        reason += "[Session Tokyo: liquidité réduite] ";
        conf += 10.0;
    } else if (session == TradingSession::OVERLAP_LN) {
        reason += "[Overlap London-NY: liquidité max] ";
        conf += 20.0;
    }

    // ── [2] Spread anormal — bloque tout + bloque recovery ────
    if (spreadRatio > 4.0) {
        vd.decision  = MarketDecision::STOP_ORDERS;
        vd.reasoning = "Spread x" + std::to_string((int)spreadRatio)
                     + " normal — news/spike détecté";
        vd.confidence = 95.0;
        vd.allowRecovery = false;
        vd.recoveryScore = 0.0;
        vd.recoveryReason = "Spread critique x" + std::to_string((int)spreadRatio);
        return vd;
    }
    if (spreadRatio > 2.5) {
        vd.decision  = MarketDecision::LOCK_WAIT;
        vd.reasoning = "Spread élargi x" + std::to_string((int)spreadRatio)
                     + " — attente normalisation";
        vd.confidence = 80.0;
        vd.allowRecovery = false;
        vd.recoveryScore = 0.0;
        vd.recoveryReason = "Spread élargi x" + std::to_string((int)spreadRatio);
        return vd;
    }

    // ── [3] Structure pièges → verrouillage ───────────────────
    bool isTrap = (structure == MarketStructure::TRAP_BULL ||
                   structure == MarketStructure::TRAP_BEAR);
    if (isTrap) {
        vd.decision  = MarketDecision::LOCK_WAIT;
        vd.reasoning = "Structure piège détectée ("
                     + structureName(structure) + ") — verrouillage";
        vd.confidence = 82.0;
        vd.allowRecovery = false;
        vd.recoveryScore = 5.0;
        vd.recoveryReason = "Piège " + structureName(structure) + " — recovery bloqué";
        return vd;
    }

    // ── [4] Oscillation serrée (BB squeeze M5 + M15) ─────────
    if (m5.bbNarrow() && m15.bbNarrow()) {
        vd.decision  = MarketDecision::LOCK_WAIT;
        vd.reasoning = "BB squeeze M5+M15 — marché compressé, attente cassure";
        vd.confidence = 75.0;
        // Recovery possible seulement si M30 montre une direction
        bool m30trend = !m30.bbNarrow() && (std::abs(m30.momentum) > 0.001);
        vd.allowRecovery = m30trend;
        vd.recoveryScore = m30trend ? 45.0 : 10.0;
        vd.recoveryReason = m30trend
            ? "BB squeeze M5/M15 mais M30 directionnel — recovery conditionnel"
            : "BB squeeze total M5+M15+M30 — recovery bloqué";
        return vd;
    }

    // ── [5] Indicateurs multi-TF ───────────────────────────────
    bool bullishTrend = (structure == MarketStructure::BULLISH_TREND ||
                         structure == MarketStructure::BREAKOUT_UP);
    bool bearishTrend = (structure == MarketStructure::BEARISH_TREND ||
                         structure == MarketStructure::BREAKOUT_DOWN);

    // MACD cross-TF — 3 timeframes doivent confirmer (vs 2 avant)
    bool macdBull3TF = m5.macdBullish() && m15.macdBullish() && m30.macdBullish();
    bool macdBear3TF = !m5.macdBullish() && !m15.macdBullish() && !m30.macdBullish();

    // RSI extremes sur 2 TF minimum
    bool rsiExtreme = (m5.rsiOverbought() && (m15.rsiOverbought() || m30.rsiOverbought())) ||
                      (m5.rsiOversold()   && (m15.rsiOversold()   || m30.rsiOversold()));

    // ── [6] RSI extrême → gel des stops (seulement si profit à protéger)
    // Seuil 85/15 : évite les faux gels en tendance forte normale
    // Condition profit : geler sans P&L positif ne protège rien et
    //                    empêche le trail de suivre le marché inutilement
    if (rsiExtreme && basketPnl > 1.0) {
        reason += "[RSI extrême M5+M15/M30 + profit → gel protecteur] ";
        conf += 20.0;
        vd.decision  = MarketDecision::TRAIL_FREEZE;
        vd.reasoning = reason + "Gel des stops — retournement imminent";
        vd.confidence = std::min(conf + 45.0, 100.0);
        vd.allowRecovery = false;
        vd.recoveryScore = 8.0;
        vd.recoveryReason = "RSI extrême — retournement probable, recovery bloqué";
        return vd;
    }

    // ── [7] Trend fort 3-TF → trail AGRESSIF ─────────────────
    // Critères renforcés : structure + momentum M5 ET M30 + MACD 3 TF
    // Bonus basket : si perte flottante élevée → abaisser le seuil momentum
    //   (on veut locker les profits de recovery plus agressivement)
    double momentumThresh = (basketPnl < -20.0 && openOrders >= 2) ? 0.0015 : 0.002;
    bool momentumAggressive = std::abs(m5.momentum) > momentumThresh
                           && std::abs(m30.momentum) > 0.001;

    if ((bullishTrend || bearishTrend) && momentumAggressive) {
        if (macdBull3TF || macdBear3TF) {
            reason += "[Trend " + structureName(structure)
                   + " confirmé M5+M15+M30 + momentum fort] ";
            if (basketPnl < -10.0)
                reason += "[basket en perte → trail agressif prioritaire] ";
            conf += 35.0;
            vd.decision   = MarketDecision::TRAIL_AGGRESSIVE;
            vd.reasoning  = reason;
            vd.confidence = std::min(conf + 50.0, 100.0);
            // Recovery autorisé en trend fort si spread ok
            vd.recoveryScore  = 80.0;
            vd.allowRecovery  = true;
            vd.recoveryReason = "Trend 3-TF confirmé — recovery autorisé";
            return vd;
        }
    }

    // ── [8] Range large → trail naturel ───────────────────────
    if (structure == MarketStructure::RANGE_WIDE) {
        vd.decision  = MarketDecision::TRAIL_NATURAL;
        vd.reasoning = "Range large — trail naturel, conditions propices au grid";
        vd.confidence = 60.0;
    } else {
        // ── [9] Structure incertaine → observation ─────────────
        vd.decision  = MarketDecision::OBSERVE;
        vd.reasoning = "Structure incertaine (" + structureName(structure)
                     + ") — observation en cours";
        vd.confidence = 40.0;
    }

    // ─────────────────────────────────────────────────────────
    //  AUDIT RECOVERY MATHÉMATIQUE (commun à RANGE + OBSERVE)
    //  Calcul du recoveryScore sur 5 facteurs indépendants
    // ─────────────────────────────────────────────────────────
    double rScore = 0.0;
    std::string rReason;

    // F1 (+25) Spread normal
    if      (spreadRatio <= 1.2) { rScore += 25.0; rReason += "F1:✓spread "; }
    else if (spreadRatio <= 1.8) { rScore += 12.0; rReason += "F1:~spread "; }
    else                         {                  rReason += "F1:✗spread "; }

    // F2 (+20) Structure non-piège
    if (!isTrap) {
        if (structure == MarketStructure::RANGE_WIDE ||
            bullishTrend || bearishTrend) {
            rScore += 20.0; rReason += "F2:✓struct ";
        } else {
            rScore += 10.0; rReason += "F2:~struct ";
        }
    } else {
        rReason += "F2:✗piège ";
    }

    // F3 (+25) Momentum suffisant (marché en mouvement directionnnel)
    double mom = std::max(std::abs(m5.momentum), std::abs(m30.momentum));
    if      (mom > 0.003) { rScore += 25.0; rReason += "F3:✓mom++ "; }
    else if (mom > 0.001) { rScore += 15.0; rReason += "F3:~mom   "; }
    else                  {                  rReason += "F3:✗mom   "; }

    // F4 (+15) M30 non compressé (BB width > 0.3%)
    if (!m30.bbNarrow()) { rScore += 15.0; rReason += "F4:✓M30BB "; }
    else                 {                  rReason += "F4:✗M30BB "; }

    // F5 (+15) RSI M5+M30 non extrême
    bool rsiOk = !m5.rsiOverbought() && !m5.rsiOversold() &&
                 !m30.rsiOverbought() && !m30.rsiOversold();
    if (rsiOk) { rScore += 15.0; rReason += "F5:✓RSI  "; }
    else       {                  rReason += "F5:✗RSI  "; }

    // Seuil adaptatif : si panier profondément chargé (≥4 ordres ouverts),
    // rehausser le seuil pour éviter de sur-empiler les positions recovery
    double threshold = (openOrders >= 4) ? 70.0 : 55.0;
    if (openOrders >= 4) rReason += "[seuil↑70 — panier chargé] ";

    vd.recoveryScore  = rScore;
    vd.allowRecovery  = (rScore >= threshold);
    vd.recoveryReason = rReason
        + (vd.allowRecovery ? "→ RECOVERY OK" : "→ BLOQUÉ");

    return vd;
}

// ============================================================
//  analyze — calcul complet thread-safe
// ============================================================
VisualDecision MarketIntelligence::analyze() const {
    // Snapshots des barres (lecture sous verrou minimal)
    std::deque<Bar> bM1, bM5, bM15, bM30;
    Bar c1, c5, c15, cM30;
    double se;
    {
        std::lock_guard lk(mu_);
        bM1  = m1Bars_;   c1   = curM1_;
        bM5  = m5Bars_;   c5   = curM5_;
        bM15 = m15Bars_;  c15  = curM15_;
        bM30 = m30Bars_;  cM30 = curM30_;
        se   = spreadEwma_;
    }

    // Ajoute la barre partielle courante (si des ticks existent)
    if (c1.ticks   > 0) bM1.push_back(c1);
    if (c5.ticks   > 0) bM5.push_back(c5);
    if (c15.ticks  > 0) bM15.push_back(c15);
    if (cM30.ticks > 0) bM30.push_back(cM30);

    IndiSet indM1  = computeIndicators(bM1);
    IndiSet indM5  = computeIndicators(bM5);
    IndiSet indM15 = computeIndicators(bM15);
    IndiSet indM30 = computeIndicators(bM30);

    MarketStructure st = analyzeStructure(bM5);
    TradingSession  ses = detectSession();

    // SpreadRatio = spread courant / spread EWMA
    // Un ratio > 2 indique un spike (news, manque de liquidité)
    double curSpreadSnap;
    { std::lock_guard lk(mu_); curSpreadSnap = curSpread_; }
    double spreadRatio = (se > 1e-10 && curSpreadSnap > 0.0)
                       ? curSpreadSnap / se
                       : 1.0;

    double bPnl; int nOrd;
    double bsLotsCached, ssLotsCached; bool inRecoveryCached;
    {
        std::lock_guard lk(mu_);
        bPnl            = basketPnlCache_;
        nOrd            = openOrdersCache_;
        bsLotsCached    = bsLotsCache_;
        ssLotsCached    = ssLotsCache_;
        inRecoveryCached = inRecoveryCache_;
    }

    VisualDecision vd = computeDecision(indM1, indM5, indM15, indM30,
                                        st, ses, spreadRatio, bPnl, nOrd);

    // ── Couche DeepSeek LLM — enrichissement ACTIF de la décision ──
    DeepSeekSignal ds = deepseek_.getLastSignal();
    vd.dsSignal = ds;

    if (ds.fresh && ds.signal != DSSignal::UNKNOWN) {
        bool localBullish = (vd.decision == MarketDecision::TRAIL_AGGRESSIVE &&
                             (st == MarketStructure::BULLISH_TREND ||
                              st == MarketStructure::BREAKOUT_UP));
        bool localBearish = (vd.decision == MarketDecision::TRAIL_AGGRESSIVE &&
                             (st == MarketStructure::BEARISH_TREND ||
                              st == MarketStructure::BREAKOUT_DOWN));

        // ── [A] Accord DS + local → boost confiance ────────────
        if ((localBullish && ds.signal == DSSignal::BUY_BIAS) ||
            (localBearish && ds.signal == DSSignal::SELL_BIAS)) {
            double boost = ds.effectiveConfidence * 0.15;
            vd.confidence = std::min(vd.confidence + boost, 100.0);
            vd.reasoning += " | DS:" + ds.signalName()
                          + "(" + std::to_string((int)ds.confidence) + "%)";
        }

        // ── [B] DS oriente une décision neutre (TRAIL_NATURAL ou OBSERVE) ──
        // FIX PRINCIPAL : DS était ignoré dans les cas les plus fréquents.
        // Si DS est confiant (≥60% effectif), il prend la main et oriente activement.
        bool neutralDecision = (vd.decision == MarketDecision::TRAIL_NATURAL ||
                                vd.decision == MarketDecision::OBSERVE);
        if (neutralDecision && ds.effectiveConfidence >= 60.0) {
            if (ds.signal == DSSignal::BUY_BIAS || ds.signal == DSSignal::SELL_BIAS) {
                std::string dir = (ds.signal == DSSignal::BUY_BIAS) ? "BULL" : "BEAR";
                vd.decision   = MarketDecision::TRAIL_AGGRESSIVE;
                vd.reasoning  = "[DS-ORIENT " + dir + " "
                               + std::to_string((int)ds.confidence) + "%] " + ds.reason;
                vd.confidence = ds.effectiveConfidence;
                vd.dsOverride = true;
                // DS oriente → débloquer recovery dans sa direction
                if (ds.signal == DSSignal::BUY_BIAS) {
                    vd.allowBSRecovery = true;
                    vd.bsBlockReason.clear();
                } else {
                    vd.allowSSRecovery = true;
                    vd.ssBlockReason.clear();
                }
            } else if (ds.signal == DSSignal::WAIT || ds.signal == DSSignal::NEUTRAL) {
                vd.reasoning += " | DS:WAIT";
            }
        }

        // ── [C] LOCK DS fort → surcharge (seuil abaissé 85→70) ─
        if (ds.signal == DSSignal::LOCK && ds.effectiveConfidence >= 70.0) {
            if (vd.decision != MarketDecision::NO_TRADE &&
                vd.decision != MarketDecision::STOP_ORDERS) {
                vd.decision   = MarketDecision::LOCK_WAIT;
                vd.reasoning  = "[DS-LOCK " + std::to_string((int)ds.confidence)
                               + "%] " + ds.reason;
                vd.confidence = ds.effectiveConfidence;
                vd.dsOverride = true;
            }
        }

        // ── [D] Désaccord majeur → gel préventif (seuil abaissé 90→70) ─
        if (ds.effectiveConfidence >= 70.0) {
            bool currentlyBull = (vd.decision == MarketDecision::TRAIL_AGGRESSIVE &&
                                  (st == MarketStructure::BULLISH_TREND ||
                                   st == MarketStructure::BREAKOUT_UP));
            bool currentlyBear = (vd.decision == MarketDecision::TRAIL_AGGRESSIVE &&
                                  (st == MarketStructure::BEARISH_TREND ||
                                   st == MarketStructure::BREAKOUT_DOWN));
            if (currentlyBull && ds.signal == DSSignal::SELL_BIAS) {
                vd.decision   = MarketDecision::TRAIL_FREEZE;
                vd.reasoning  = "[DS-DÉSACCORD " + std::to_string((int)ds.confidence)
                               + "%] " + ds.reason;
                vd.confidence = (vd.confidence + ds.effectiveConfidence) * 0.5;
                vd.dsOverride = true;
            } else if (currentlyBear && ds.signal == DSSignal::BUY_BIAS) {
                vd.decision   = MarketDecision::TRAIL_FREEZE;
                vd.reasoning  = "[DS-DÉSACCORD " + std::to_string((int)ds.confidence)
                               + "%] " + ds.reason;
                vd.confidence = (vd.confidence + ds.effectiveConfidence) * 0.5;
                vd.dsOverride = true;
            }
        }

        // ── [E] Biais DS sur les facteurs de trail ──────────────
        // Proportionnel à la confidence effective (entre 60% et 100%) : max ±20%
        if (ds.effectiveConfidence >= 60.0) {
            double intensity  = std::min((ds.effectiveConfidence - 60.0) / 40.0, 1.0); // 0..1
            double biasFactor = 1.0 + intensity * 0.20;  // 1.0 → 1.20
            if (ds.signal == DSSignal::SELL_BIAS) {
                // Marché baissier → SS (Sell Stop) plus serré, BS plus large
                vd.dsTrailSSBias = 1.0 / biasFactor;  // SS resserré (déclenche plus tôt)
                vd.dsTrailBSBias = biasFactor;          // BS élargi (protection buy)
            } else if (ds.signal == DSSignal::BUY_BIAS) {
                // Marché haussier → BS (Buy Stop) plus serré, SS plus large
                vd.dsTrailBSBias = 1.0 / biasFactor;  // BS resserré
                vd.dsTrailSSBias = biasFactor;          // SS élargi
            }
        }
    }

    // ── Bloc directionnel recovery (CRITIQUE) ────────────────
    // Calcul des flags allowBSRecovery / allowSSRecovery
    // en combinant : balance de lots, signal DeepSeek, structure, RSI.
    //
    // Règle centrale : ne pas ajouter d'ordres recovery dans la direction
    // qui va à l'encontre de la tendance ET qui est déjà surexposée.
    //
    // FIX : DS ≥ 70% peut contrebalancer la structure seule et débloquer
    // la recovery dans SA direction (évite le blocage mutuel).
    // ──────────────────────────────────────────────────────────
    {
        DeepSeekSignal ds2 = deepseek_.getLastSignal();
        double netDelta = bsLotsCached - ssLotsCached;

        // DS contrebalance la structure quand sa confiance effective est forte (≥70%)
        // → La structure seule ne suffit plus à bloquer dans la direction DS
        bool dsCounterBearStruct = (ds2.fresh && ds2.signal == DSSignal::BUY_BIAS
                                    && ds2.effectiveConfidence >= 70.0);
        bool dsCounterBullStruct = (ds2.fresh && ds2.signal == DSSignal::SELL_BIAS
                                    && ds2.effectiveConfidence >= 70.0);

        // ── Critères qui bloquent BS recovery (BUY Stop) ─────
        // BS = position haussière → bloquer si marché baissier
        // Si DS dit BUY_BIAS ≥ 70% effectif, la structure BEARISH ne bloque plus BS
        bool bsBearStruct    = (st == MarketStructure::BEARISH_TREND)
                               && !dsCounterBearStruct;
        bool bsRsiOverbought = (indM5.rsi >= 75.0 && indM30.rsi >= 65.0);
        bool bsDsConfirm     = (ds2.fresh && ds2.signal == DSSignal::SELL_BIAS
                                && ds2.effectiveConfidence >= 55.0);
        bool bsOverexposed   = (netDelta > 0.10);

        // En mode normal (hors recovery), critère 1 suffit (prudence maximale)
        // En recovery, on exige 2 signaux pour éviter de bloquer inutilement
        int bsThreshold = inRecoveryCached ? 2 : 1;

        int bsBearScore = (int)bsBearStruct + (int)bsRsiOverbought
                        + (int)bsDsConfirm + (int)bsOverexposed;
        if (bsBearScore >= bsThreshold) {
            vd.allowBSRecovery = false;
            vd.bsBlockReason   = "[BS bloqué:";
            if (bsBearStruct)    vd.bsBlockReason += " struct.Bear";
            if (bsRsiOverbought) vd.bsBlockReason += " RSI↑overbought";
            if (bsDsConfirm)     vd.bsBlockReason += " DS:SELL("
                                 + std::to_string((int)ds2.confidence) + "%)";
            if (bsOverexposed)   vd.bsBlockReason += " Δ=+"
                                 + std::to_string((int)(netDelta * 100)) + "clots";
            vd.bsBlockReason += "]";
        }
        // DS BUY_BIAS fort → forcer l'autorisation BS même si bloqué ci-dessus
        if (dsCounterBearStruct && !vd.allowBSRecovery) {
            // DS contredit la structure, mais d'autres critères bloquent encore
            // → autoriser uniquement si DS est fort et pas en RSI extrême
            if (!bsRsiOverbought) {
                vd.allowBSRecovery = true;
                vd.bsBlockReason   = "[BS:DS-OVERRIDE BUY("
                                   + std::to_string((int)ds2.confidence) + "%)]";
            }
        }

        // ── Critères qui bloquent SS recovery (SELL Stop) ────
        // SS = position baissière → bloquer si marché haussier
        // Si DS dit SELL_BIAS ≥ 70% effectif, la structure BULLISH ne bloque plus SS
        bool ssBullStruct   = (st == MarketStructure::BULLISH_TREND)
                              && !dsCounterBullStruct;
        bool ssRsiOversold  = (indM5.rsi <= 25.0 && indM30.rsi <= 35.0);
        bool ssDsConfirm    = (ds2.fresh && ds2.signal == DSSignal::BUY_BIAS
                               && ds2.effectiveConfidence >= 55.0);
        bool ssOverexposed  = (netDelta < -0.10);

        int ssThreshold = inRecoveryCached ? 2 : 1;
        int ssBullScore = (int)ssBullStruct + (int)ssRsiOversold
                        + (int)ssDsConfirm + (int)ssOverexposed;
        if (ssBullScore >= ssThreshold) {
            vd.allowSSRecovery = false;
            vd.ssBlockReason   = "[SS bloqué:";
            if (ssBullStruct)   vd.ssBlockReason += " struct.Bull";
            if (ssRsiOversold)  vd.ssBlockReason += " RSI↓oversold";
            if (ssDsConfirm)    vd.ssBlockReason += " DS:BUY("
                                + std::to_string((int)ds2.confidence) + "%)";
            if (ssOverexposed)  vd.ssBlockReason += " Δ="
                                + std::to_string((int)(netDelta * 100)) + "clots";
            vd.ssBlockReason += "]";
        }
        // DS SELL_BIAS fort → forcer l'autorisation SS même si bloqué ci-dessus
        if (dsCounterBullStruct && !vd.allowSSRecovery) {
            if (!ssRsiOversold) {
                vd.allowSSRecovery = true;
                vd.ssBlockReason   = "[SS:DS-OVERRIDE SELL("
                                   + std::to_string((int)ds2.confidence) + "%)]";
            }
        }

        // allowRecovery = true seulement si AU MOINS une direction est libre
        if (!vd.allowBSRecovery && !vd.allowSSRecovery) {
            vd.allowRecovery = false;
            vd.recoveryReason += " | Tous les sens bloqués directionnellement";
        }
    }

    return vd;
}

// ============================================================
//  Getters rapides
// ============================================================
MarketDecision MarketIntelligence::decision() const noexcept {
    std::lock_guard lk(mu_);
    return lastDecision_.decision;
}

std::string MarketIntelligence::reasoning() const {
    std::lock_guard lk(mu_);
    return lastDecision_.reasoning;
}

// ============================================================
//  detectSession — UTC heure courante
// ============================================================
TradingSession MarketIntelligence::detectSession() noexcept {
    auto now  = SysClock::now();
    std::time_t t = SysClock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    int h = utc.tm_hour;
    int wd = utc.tm_wday; // 0=Sun 6=Sat

    if (wd == 0 && h < 22) return TradingSession::CLOSED;
    if (wd == 6 && h >= 21) return TradingSession::CLOSED;
    if (wd == 6) return TradingSession::CLOSED;

    // Overlap London-NY : 13-17 UTC
    if (h >= 13 && h < 17) return TradingSession::OVERLAP_LN;
    // London : 08-17 UTC
    if (h >= 8  && h < 17) return TradingSession::LONDON;
    // New York : 13-22 UTC
    if (h >= 17 && h < 22) return TradingSession::NEW_YORK;
    // Tokyo : 00-09 UTC
    if (h < 9)              return TradingSession::TOKYO;

    return TradingSession::TOKYO;
}

// ============================================================
//  Convertisseurs texte
// ============================================================
std::string MarketIntelligence::decisionName(MarketDecision d) noexcept {
    switch (d) {
        case MarketDecision::CONTINUE:         return "CONTINUE";
        case MarketDecision::TRAIL_AGGRESSIVE: return "TRAIL_AGR";
        case MarketDecision::TRAIL_NATURAL:    return "TRAIL_NAT";
        case MarketDecision::TRAIL_FREEZE:     return "TRAIL_FRZ";
        case MarketDecision::ADD_ORDER:        return "ADD_ORDER";
        case MarketDecision::REMOVE_TRAIL:     return "REM_TRAIL";
        case MarketDecision::NO_TRADE:         return "NO_TRADE";
        case MarketDecision::OBSERVE:          return "OBSERVE  ";
        case MarketDecision::LOCK_WAIT:        return "LOCK_WAIT";
        case MarketDecision::STOP_ORDERS:      return "STOP_ORDR";
    }
    return "?";
}

std::string MarketIntelligence::structureName(MarketStructure s) noexcept {
    switch (s) {
        case MarketStructure::UNKNOWN:       return "UNKN  ";
        case MarketStructure::BULLISH_TREND: return "BULL↑ ";
        case MarketStructure::BEARISH_TREND: return "BEAR↓ ";
        case MarketStructure::RANGE_TIGHT:   return "RNG_T ";
        case MarketStructure::RANGE_WIDE:    return "RNG_W ";
        case MarketStructure::BREAKOUT_UP:   return "BKUP↑ ";
        case MarketStructure::BREAKOUT_DOWN: return "BKDN↓ ";
        case MarketStructure::TRAP_BULL:     return "TRAP_B";
        case MarketStructure::TRAP_BEAR:     return "TRAP_R";
    }
    return "?";
}

std::string MarketIntelligence::sessionName(TradingSession s) noexcept {
    switch (s) {
        case TradingSession::CLOSED:     return "CLOSED";
        case TradingSession::TOKYO:      return "TOKYO ";
        case TradingSession::LONDON:     return "LONDON";
        case TradingSession::NEW_YORK:   return "NY    ";
        case TradingSession::OVERLAP_LN: return "LN+NY ";
    }
    return "?";
}

} // namespace top4ea
