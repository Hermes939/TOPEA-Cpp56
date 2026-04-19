// ============================================================
//  TOP4EA — market_intelligence.cpp  C++20
//  Assistante Visuelle AI + iniMTA Multi-Timeframe
// ============================================================
#include "../include/market_intelligence.hpp"
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
static constexpr int TF_H1  = 3600;

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
    curM1_ = curM5_ = curM15_ = curH1_ = Bar{};
    m1Start_ = m5Start_ = m15Start_ = h1Start_ = now;
}

// ============================================================
//  update — appelé à chaque tick
// ============================================================
void MarketIntelligence::update(const Tick& tick, double /*pointSize*/) noexcept {
    if (tick.bid <= 0.0 || tick.ask <= 0.0) return;

    // Spread EWMA
    if (spreadEwma_ <= 0.0) spreadEwma_ = tick.spread;
    else spreadEwma_ = 0.05 * tick.spread + 0.95 * spreadEwma_;

    pushTick(tick);
    closeBars(tick);
    ++tickCount_;

    // Recalcul décision toutes les 10 ticks
    if (tickCount_ % 10 == 0) {
        VisualDecision vd = analyze();
        std::lock_guard lk(mu_);
        lastDecision_ = vd;
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
    update_bar(curH1_);
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
    if (secs(h1Start_,  TF_H1))  push(h1Bars_,  curH1_,  h1Start_,  TF_H1,  MAX_H1);
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
//    4. Multi-TF confirmation (M5 + H1)
//    5. RSI + MACD cross-TF
//    6. BB squeeze
//    7. Décision finale avec score de confiance
// ============================================================
VisualDecision MarketIntelligence::computeDecision(
    const IndiSet& m1, const IndiSet& m5,
    const IndiSet& m15, const IndiSet& h1,
    MarketStructure structure,
    TradingSession  session,
    double spreadRatio) const noexcept
{
    VisualDecision vd;
    vd.structure = structure;
    vd.session   = session;
    vd.m1 = m1; vd.m5 = m5; vd.m15 = m15; vd.h1 = h1;

    std::string reason;
    double conf = 0.0;

    // ── [1] Session ────────────────────────────────────────────
    if (session == TradingSession::CLOSED) {
        vd.decision  = MarketDecision::NO_TRADE;
        vd.reasoning = "Session fermée — aucun trade";
        vd.confidence = 100.0;
        return vd;
    }
    if (session == TradingSession::TOKYO) {
        reason += "[Session Tokyo: liquidité réduite] ";
        conf += 10.0;
    } else if (session == TradingSession::OVERLAP_LN) {
        reason += "[Overlap London-NY: liquidité max] ";
        conf += 20.0;
    }

    // ── [2] Spread anormal — bloque tout ──────────────────────
    if (spreadRatio > 4.0) {
        vd.decision  = MarketDecision::STOP_ORDERS;
        vd.reasoning = "Spread x" + std::to_string((int)spreadRatio)
                     + " normal — news/spike détecté";
        vd.confidence = 95.0;
        return vd;
    }
    if (spreadRatio > 2.5) {
        vd.decision  = MarketDecision::LOCK_WAIT;
        vd.reasoning = "Spread élargi x" + std::to_string((int)spreadRatio)
                     + " — attente normalisation";
        vd.confidence = 80.0;
        return vd;
    }

    // ── [3] Structure pièges → verrouillage ───────────────────
    if (structure == MarketStructure::TRAP_BULL ||
        structure == MarketStructure::TRAP_BEAR) {
        vd.decision  = MarketDecision::LOCK_WAIT;
        vd.reasoning = "Structure piège détectée ("
                     + structureName(structure) + ") — verrouillage";
        vd.confidence = 82.0;
        return vd;
    }

    // ── [4] Oscillation serrée (BB squeeze M5 + M15) ─────────
    if (m5.bbNarrow() && m15.bbNarrow()) {
        vd.decision  = MarketDecision::LOCK_WAIT;
        vd.reasoning = "BB squeeze M5+M15 — marché compressé, attente cassure";
        vd.confidence = 75.0;
        return vd;
    }

    // ── [5] Trend fort → trail agressif ───────────────────────
    bool bullishTrend = (structure == MarketStructure::BULLISH_TREND ||
                         structure == MarketStructure::BREAKOUT_UP);
    bool bearishTrend = (structure == MarketStructure::BEARISH_TREND ||
                         structure == MarketStructure::BREAKOUT_DOWN);

    bool macdBull = m5.macdBullish() && m15.macdBullish();
    bool macdBear = !m5.macdBullish() && !m15.macdBullish();

    bool momentumStrong = std::abs(m5.momentum) > 0.003;   // 0.3%

    if ((bullishTrend || bearishTrend) && momentumStrong) {
        if (macdBull || macdBear) {
            reason += "[Trend " + structureName(structure)
                   + " + MACD confirmé + momentum fort] ";
            conf += 30.0;
            vd.decision = MarketDecision::TRAIL_AGGRESSIVE;
            vd.reasoning = reason;
            vd.confidence = std::min(conf + 50.0, 100.0);
            return vd;
        }
    }

    // ── [6] Zone TP proche ou P&L positif élevé → gel ────────
    bool rsiExtreme = (m5.rsiOverbought() && m15.rsiOverbought()) ||
                      (m5.rsiOversold()   && m15.rsiOversold());
    if (rsiExtreme) {
        reason += "[RSI extrême M5+M15 → zone retournement] ";
        conf += 20.0;
        vd.decision  = MarketDecision::TRAIL_FREEZE;
        vd.reasoning = reason + "Gel des stops — retournement imminent";
        vd.confidence = std::min(conf + 45.0, 100.0);
        return vd;
    }

    // ── [7] Range large → trail naturel, continuer ────────────
    if (structure == MarketStructure::RANGE_WIDE) {
        vd.decision  = MarketDecision::TRAIL_NATURAL;
        vd.reasoning = "Range large — trail naturel, conditions propices au grid";
        vd.confidence = 60.0;
        return vd;
    }

    // ── [8] Observe → structure indéterminée ──────────────────
    vd.decision  = MarketDecision::OBSERVE;
    vd.reasoning = "Structure incertaine (" + structureName(structure)
                 + ") — observation en cours";
    vd.confidence = 40.0;
    return vd;
}

// ============================================================
//  analyze — calcul complet thread-safe
// ============================================================
VisualDecision MarketIntelligence::analyze() const {
    // Snapshots des barres (lecture sous verrou minimal)
    std::deque<Bar> bM1, bM5, bM15, bH1;
    Bar c1, c5, c15, cH;
    double se;
    {
        std::lock_guard lk(mu_);
        bM1 = m1Bars_;  c1  = curM1_;
        bM5 = m5Bars_;  c5  = curM5_;
        bM15 = m15Bars_; c15 = curM15_;
        bH1 = h1Bars_;  cH  = curH1_;
        se   = spreadEwma_;
    }

    // Ajoute la barre partielle courante (si des ticks existent)
    if (c1.ticks  > 0) bM1.push_back(c1);
    if (c5.ticks  > 0) bM5.push_back(c5);
    if (c15.ticks > 0) bM15.push_back(c15);
    if (cH.ticks  > 0) bH1.push_back(cH);

    IndiSet indM1  = computeIndicators(bM1);
    IndiSet indM5  = computeIndicators(bM5);
    IndiSet indM15 = computeIndicators(bM15);
    IndiSet indH1  = computeIndicators(bH1);

    MarketStructure st = analyzeStructure(bM5);
    TradingSession  ses = detectSession();

    // SpreadRatio = spread courant / spread EWMA
    double spreadRatio = (se > 1e-10) ? (se / se) : 1.0; // identité avant données
    // Note : se est l'EWMA, pas le courant — utilise la barre M1 courante
    // Ratio approximé via bbWidth M1 en proxy de volatilité intraday
    if (se > 1e-10 && indM1.atr > 0.0)
        spreadRatio = se / (se * 0.95 + 1e-10);  // toujours ~1 sans données news

    return computeDecision(indM1, indM5, indM15, indH1, st, ses, spreadRatio);
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
