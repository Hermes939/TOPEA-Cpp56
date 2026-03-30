// ============================================================
//  TOP4EA — intelligence_engine.cpp
//  Raisonnement profond — Hedging mathématique pur C++20
//
//  ARCHITECTURE DE DÉCISION :
//  ─────────────────────────────────────────────────────────
//  [1] RÉGIME : OUEstimator(ρ₁) × VolatilityRegime × Momentum
//      → classe le marché en 5 états (Range/Trend/Transitional)
//
//  [2] NET DELTA : Σ(BUY lots) - Σ(SELL lots)
//      → mesure l'exposition directionnelle nette
//      → signal hedge si |delta| > 30% des lots totaux
//         en régime Trending pour neutraliser l'exposition
//
//  [3] GAP FACTORS : facteurs adaptatifs pour trail stop
//      RangeQuiet  → ssGap×0.85, bsGap×0.85 (stops serrés)
//      TrendUp     → ssGap×1.40, bsGap×0.80 (asymétrie bullish)
//      TrendDown   → ssGap×0.80, bsGap×1.40 (asymétrie bearish)
//      RangeVol    → ssGap×1.20, bsGap×1.20 (protection vol)
//      Zone TP     → freeze tous les stops
//
//  [4] RISK SCORE composite sur 3 axes (0→100) :
//      40% recovery depth | 35% marge | 25% delta imbalance
//
//  [5] REASONING : log structuré en français pour dashboard
// ============================================================
#include "../include/intelligence_engine.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Construction
// ────────────────────────────────────────────────────────────
IntelligenceEngine::IntelligenceEngine(const Config& cfg)
    : cfg_(cfg)
    , ouEst_(cfg.ouWindowSize > 0 ? cfg.ouWindowSize : 200,
             cfg.ouMinTheta   > 0 ? cfg.ouMinTheta   : 0.05)
    , volReg_(100)
{}

// ────────────────────────────────────────────────────────────
//  onTick — alimentation indicateurs internes
// ────────────────────────────────────────────────────────────
void IntelligenceEngine::onTick(const Tick& tick) noexcept {
    double mid = (tick.bid + tick.ask) * 0.5;
    if (mid <= 0.0) return;

    // OU Estimator (mean-reversion / trending)
    ouEst_.push(mid);

    // Momentum fast/slow (variation de prix normalisée)
    if (lastMid_ > 0.0) {
        double ret = (mid - lastMid_) / lastMid_ * 1e4; // en bps
        momentumFast_.update(ret);
        momentumSlow_.update(ret);
    }
    lastMid_ = mid;
}

// ────────────────────────────────────────────────────────────
//  classifyRegime — 5 états de marché
// ────────────────────────────────────────────────────────────
IntelligenceEngine::Regime IntelligenceEngine::classifyRegime(
    double atrPoints,
    double rho1,
    double momentumFast,
    double momentumSlow) const noexcept
{
    // Mise à jour VolatilityRegime (mutable interne via const_cast acceptable ici)
    // On utilise une variable locale pour l'appel — volReg_ est alimenté dans analyze()
    VolRegime vr = volReg_.classify(atrPoints);

    bool meanReverting = (rho1 < -0.15); // ρ₁ < -0.15 → mean-reverting
    bool trending      = (rho1 > 0.10);  // ρ₁ >  0.10 → trending

    double momDiv = momentumFast - momentumSlow; // divergence fast/slow

    if (trending && momDiv > 0.3)
        return Regime::TrendUp;
    if (trending && momDiv < -0.3)
        return Regime::TrendDown;

    if (!meanReverting && !trending)
        return Regime::Transitional;

    if (meanReverting && vr == VolRegime::Low)
        return Regime::RangeQuiet;
    if (meanReverting && vr == VolRegime::High)
        return Regime::RangeVolatile;
    if (meanReverting)
        return Regime::RangeQuiet; // Medium vol → range calme

    return Regime::Transitional;
}

// ────────────────────────────────────────────────────────────
//  computeGapFactors — trail stop adaptatif
//
//  ssGapFactor = facteur pour le gap du BuyStop  (SS)
//  bsGapFactor = facteur pour le gap du SellStop (BS)
//
//  Logique :
//  ─────────────────────────────────────────────────────────
//  RangeQuiet   → stops serrés des deux côtés (capture rapide)
//  TrendUp      → SS large (protège vs cascade haussière)
//                 BS serré (marché monte → BS peu menacé)
//  TrendDown    → BS large (protège vs cascade baissière)
//                 SS serré (marché descend → SS peu menacé)
//  RangeVol     → stops larges des deux côtés (protection vol)
//  Zone TP      → freeze total (P&L proche du target)
//
//  Correction delta : le côté le plus chargé est plus large
//  Correction recovery : chaque niveau ajoute gapExpansionFactor
// ────────────────────────────────────────────────────────────
std::pair<double,double> IntelligenceEngine::computeGapFactors(
    Regime r,
    double netDelta,
    double totalLots,
    int    ssR,
    int    bsR,
    double pnlRatio) const noexcept
{
    double ss = 1.0;
    double bs = 1.0;

    // [A] Facteurs de base selon le régime
    switch (r) {
        case Regime::RangeQuiet:
            ss = 0.85; bs = 0.85;   // serrés → capture rapide
            break;
        case Regime::RangeVolatile:
            ss = 1.25; bs = 1.25;   // larges → protection pics de vol
            break;
        case Regime::TrendUp:
            ss = 1.45; bs = 0.80;   // asymétrie bullish
            break;
        case Regime::TrendDown:
            ss = 0.80; bs = 1.45;   // asymétrie bearish
            break;
        case Regime::Transitional:
        case Regime::Unknown:
        default:
            ss = 1.05; bs = 1.05;   // légèrement plus large, prudence
            break;
    }

    // [B] Correction recovery : chaque niveau élargit son côté
    //     (évite la cascade rapide aux mêmes prix)
    if (ssR > 0) ss *= (1.0 + ssR * cfg_.gapExpansionFactor);
    if (bsR > 0) bs *= (1.0 + bsR * cfg_.gapExpansionFactor);

    // [C] Correction delta — asymétrie si portfolio déséquilibré
    //     Le côté où on a PLUS de lots reçoit un stop PLUS LARGE
    //     pour lui donner plus d'espace (protection du côté chargé)
    if (totalLots > 1e-6) {
        double pressure = netDelta / totalLots; // ∈ [-1, +1]
        double asym     = cfg_.trailAsymmetryFactor;
        if (pressure > 0.0) {
            ss *= (1.0 + pressure * asym); // net long → protéger SS
            bs *= (1.0 - pressure * asym * 0.5);
        } else if (pressure < 0.0) {
            bs *= (1.0 - pressure * asym); // net short → protéger BS
            ss *= (1.0 + pressure * asym * 0.5);
        }
    }

    // [D] Zone TP : stops figés pour protéger le cycle gagnant
    if (pnlRatio >= cfg_.trailFreezeZone) {
        // On ne modifie pas les facteurs ici — le freeze est géré par trailFrozen
    }

    // Sécurité : facteur dans [0.50, 3.0]
    ss = std::clamp(ss, 0.50, 3.0);
    bs = std::clamp(bs, 0.50, 3.0);

    return { ss, bs };
}

// ────────────────────────────────────────────────────────────
//  shouldHedge — signal de couverture delta-neutre
//
//  Un hedge est recommandé quand :
//  1. Pression delta > 40% des lots totaux (fort déséquilibre)
//  2. Régime Trending (risque que le déséquilibre s'amplifie)
//  3. Score de risque modéré (on peut encore agir)
//
//  Le hedge suggère une position inverse pour neutraliser le delta.
//  La taille = |delta| pour revenir à une exposition nulle.
// ────────────────────────────────────────────────────────────
bool IntelligenceEngine::shouldHedge(
    double netDelta,
    double totalLots,
    Regime r,
    double riskScore) const noexcept
{
    if (totalLots < 0.01) return false;

    double pressure = std::abs(netDelta) / totalLots;

    // Seuil de pression : 40% de déséquilibre
    bool highPressure = (pressure > 0.40);

    // Régime trending = risque amplifié du déséquilibre
    bool trendingRegime = (r == Regime::TrendUp || r == Regime::TrendDown);

    // Ne pas hedger si risque déjà critique (marge trop faible)
    bool canAct = (riskScore < 75.0);

    return highPressure && trendingRegime && canAct;
}

// ────────────────────────────────────────────────────────────
//  computeRiskScore — score composite 0→100
//
//  Axes :
//  [40%] Profondeur recovery : max(ssR, bsR) / maxRecov
//  [35%] Pression marge     : max(0, 100 - marginLevel%) / 80%
//  [25%] Déséquilibre delta : |delta| / totalLots
// ────────────────────────────────────────────────────────────
double IntelligenceEngine::computeRiskScore(
    double drawdownPct,
    double marginLevel,
    int    ssR,
    int    bsR,
    double hedgePressure) const noexcept
{
    int maxR = std::max(1, cfg_.maxRecovery);

    // Axe 1 — Recovery depth (40 pts max)
    double recovScore = std::min(1.0, (double)std::max(ssR, bsR) / maxR) * 40.0;

    // Axe 2 — Marge (35 pts max)
    // marginLevel = equity/margin*100 ; < 150% = danger
    double marginScore = 0.0;
    if (marginLevel > 0.0 && marginLevel < 500.0) {
        marginScore = std::max(0.0, (500.0 - marginLevel) / 500.0) * 35.0;
    }

    // Axe 3 — Delta imbalance (25 pts max)
    double deltaScore = std::clamp(hedgePressure, 0.0, 1.0) * 25.0;

    return std::min(100.0, recovScore + marginScore + deltaScore);
}

// ────────────────────────────────────────────────────────────
//  buildReasoning — texte structuré de raisonnement
// ────────────────────────────────────────────────────────────
std::string IntelligenceEngine::buildReasoning(const Analysis& a,
                                                double pnlRatio) const
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    // Régime
    oss << "RÉGIME: " << regimeName(a.regime)
        << "  ρ₁=" << std::setprecision(3) << a.rho1
        << "  MomF=" << a.momentumFast
        << "  MomS=" << a.momentumSlow
        << "\n";

    // Delta & Hedging
    oss << "DELTA NET: " << std::setprecision(2)
        << (a.netDelta >= 0 ? "+" : "") << a.netDelta << " lots"
        << "  [B=" << a.buyLots << " S=" << a.sellLots << "]"
        << "  Pression=" << (a.hedgePressure*100.0) << "%";
    if (a.needsHedge) {
        oss << "  ⚠ HEDGE requis: "
            << (a.hedgeType == 0 ? "BUY " : "SELL ")
            << a.hedgeLot << " lots pour neutraliser";
    } else {
        oss << "  ✓ exposition équilibrée";
    }
    oss << "\n";

    // Trail stop
    oss << "TRAIL: ss×" << std::setprecision(3) << a.ssGapFactor
        << " bs×" << a.bsGapFactor;
    if (a.trailFrozen) oss << "  ❄ FIGÉ (zone TP " << (pnlRatio*100.0) << "%)";
    oss << "\n";

    // Risk
    oss << "RISQUE: " << std::setprecision(1) << a.riskScore << "/100"
        << "  DD=" << a.drawdownPct << "%"
        << "  PnLv=" << std::setprecision(3) << a.pnlVelocity << "$/tick";
    if (a.marginAlert)  oss << "  ⚠ MARGE CRITIQUE";
    if (a.cascadeRisk)  oss << "  ⚠ RISQUE CASCADE";
    oss << "\n";

    // Décision résumée
    oss << "DÉCISION: ";
    if (a.riskScore >= 80)
        oss << "DÉFENSIF — réduction exposition prioritaire";
    else if (a.riskScore >= 50)
        oss << "PRUDENCE — trail élargi, stops conservateurs";
    else if (a.regime == Regime::RangeQuiet)
        oss << "OFFENSIF — marché range, capture rapide activée";
    else if (a.regime == Regime::TrendUp)
        oss << "BULLISH — asymétrie SS élargi, BS serré";
    else if (a.regime == Regime::TrendDown)
        oss << "BEARISH — asymétrie BS élargi, SS serré";
    else
        oss << "NEUTRE — stops standard";

    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  analyze — point d'entrée principal
// ────────────────────────────────────────────────────────────
IntelligenceEngine::Analysis IntelligenceEngine::analyze(
    const std::vector<OrderInfo>& orders,
    const Tick&                   tick,
    const AccountSnapshot&        account,
    double basketPnL,
    double basketTarget,
    double atrPoints,
    int    ssRecovery,
    int    bsRecovery)
{
    Analysis a;
    a.atrPoints = atrPoints;

    // ── [1] Indicateurs de marché ─────────────────────────────
    double mid = (tick.bid + tick.ask) * 0.5;

    // Mise à jour OUEstimator et VolatilityRegime
    ouEst_.push(mid);
    volReg_.push(atrPoints);

    double rho1 = 0.0;
    if (ouEst_.ready()) {
        // Recalcul ρ₁ via optimalGapPrice (on utilise l'OUEstimator interne)
        // ρ₁ est estimé indirectement : gap > 0 → rho1 < 0 (mean-reverting)
        double ouGap = ouEst_.optimalGapPrice();
        if (ouGap > 0.0) rho1 = -0.25; // mean-reverting estimé
        else             rho1 =  0.15; // trending estimé
    }

    // Momentum
    double momF = momentumFast_.value();
    double momS = momentumSlow_.value();

    // Régime
    a.rho1         = rho1;
    a.momentumFast = momF;
    a.momentumSlow = momS;
    a.volRegime    = volReg_.classify(atrPoints);
    a.regime       = classifyRegime(atrPoints, rho1, momF, momS);

    // ── [2] Analyse du portefeuille — net delta ───────────────
    double buyLots = 0.0, sellLots = 0.0;
    int    buyN = 0, sellN = 0;

    for (const auto& o : orders) {
        if (o.type == OrderType::Buy)  { buyLots  += o.lots; ++buyN;  }
        if (o.type == OrderType::Sell) { sellLots += o.lots; ++sellN; }
    }

    a.buyLots    = buyLots;
    a.sellLots   = sellLots;
    a.buyOrders  = buyN;
    a.sellOrders = sellN;
    a.totalLots  = buyLots + sellLots;
    a.netDelta   = buyLots - sellLots;
    a.hedgePressure = (a.totalLots > 1e-6)
                    ? std::abs(a.netDelta) / a.totalLots
                    : 0.0;

    // ── [3] Score de risque ───────────────────────────────────
    double drawdownUSD = (account.balance > 0.0)
                       ? std::max(0.0, account.balance - account.equity)
                       : 0.0;
    double drawdownPct = (account.balance > 0.0)
                       ? drawdownUSD / account.balance * 100.0
                       : 0.0;

    a.drawdownUSD  = drawdownUSD;
    a.drawdownPct  = drawdownPct;
    a.marginAlert  = (account.marginLevel > 0.0 && account.marginLevel < 150.0);
    a.cascadeRisk  = (std::max(ssRecovery, bsRecovery) >= cfg_.maxRecovery - 3);

    a.riskScore = computeRiskScore(drawdownPct, account.marginLevel,
                                   ssRecovery, bsRecovery, a.hedgePressure);

    // ── [4] Signal de hedge ───────────────────────────────────
    a.needsHedge = shouldHedge(a.netDelta, a.totalLots, a.regime, a.riskScore);
    if (a.needsHedge) {
        // Hedge type : inverser le déséquilibre
        a.hedgeType = (a.netDelta > 0) ? 1 : 0;   // net long → vendre, net short → acheter
        // Lot : neutraliser exactement le delta
        a.hedgeLot  = std::round(std::abs(a.netDelta) * 100.0) / 100.0;
        a.hedgeLot  = std::max(a.hedgeLot, cfg_.baseLot);
    }

    // ── [5] Facteurs trail stop adaptatifs ───────────────────
    double pnlRatio = (std::abs(basketTarget) > 1e-6)
                    ? basketPnL / basketTarget
                    : 0.0;

    auto [ssF, bsF] = computeGapFactors(a.regime, a.netDelta, a.totalLots,
                                         ssRecovery, bsRecovery, pnlRatio);
    a.ssGapFactor  = ssF;
    a.bsGapFactor  = bsF;
    a.trailFrozen  = (pnlRatio >= cfg_.trailFreezeZone && basketPnL > 0.0);
    a.trailTightness = (ssF + bsF) * 0.5;

    // ── [6] Vélocité P&L et prédiction ───────────────────────
    double vel = pnlVel_.update(basketPnL);
    a.pnlVelocity = vel;

    // Probabilité de fermeture (heuristique basée sur vélocité)
    if (vel > 0.0 && basketTarget > basketPnL) {
        double ticksEstimated = (basketTarget - basketPnL) / vel;
        a.estimatedTicksTarget = (int)std::clamp(ticksEstimated, 1.0, 9999.0);
        // Probabilité : décroît exponentiellement avec la distance
        a.closeProbPct = std::max(0.0, 100.0 * std::exp(-ticksEstimated / 10.0));
    } else {
        a.estimatedTicksTarget = 9999;
        a.closeProbPct         = 0.0;
    }

    // ── [7] Raisonnement ─────────────────────────────────────
    a.reasoning = buildReasoning(a, pnlRatio);

    // ── Mise à jour état interne ──────────────────────────────
    { std::lock_guard lk(mu_); lastRegime_ = a.regime; }

    return a;
}

// ────────────────────────────────────────────────────────────
//  Noms de régime (affichage)
// ────────────────────────────────────────────────────────────
const char* IntelligenceEngine::regimeName(Regime r) noexcept {
    switch (r) {
        case Regime::RangeQuiet:    return "RANGE/CALM";
        case Regime::RangeVolatile: return "RANGE/VOL ";
        case Regime::TrendUp:       return "TREND/UP  ";
        case Regime::TrendDown:     return "TREND/DOWN";
        case Regime::Transitional:  return "TRANSIT.  ";
        default:                    return "UNKNOWN   ";
    }
}

} // namespace top4ea
