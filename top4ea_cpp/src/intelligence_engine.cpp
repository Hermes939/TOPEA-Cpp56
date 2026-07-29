// ============================================================
//  TOP4EA — intelligence_engine.cpp
//  Raisonnement institutionnel profond — Architecture 80/20
//
//  80% MATHÉMATIQUES GÉOMÉTRIQUES PURES :
//  [1] Régime OU × Volatilité × Momentum
//  [2] Delta institutionnel multi-niveau (portfolio + CVD flux)
//  [3] Imbalance géométrique (FVG + OFI institutionnel)
//  [4] Asymétrie géométrique pure (trail adaptatif)
//  [5] Score de risque composite (4 axes quantitatifs)
//
//  20% INDICATEURS + AI :
//  [6] ATR dynamique (régime volatilité)
//  [7] Momentum EWMA fast/slow
//  [8] Prédiction P&L velocity
//  [9] AutoML PyTorch (calibration externe des paramètres θ)
//
//  AUTO-APPRENTISSAGE :
//  - ErrorMemory : enregistrement structuré des erreurs
//  - MathCalibrator : Adam + AutoML JSON → calibration θ
//  - recordCycleClosed() : alimenté par TOP4EA_Engine
// ============================================================
#include "../include/intelligence_engine.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Construction
// ────────────────────────────────────────────────────────────
IntelligenceEngine::IntelligenceEngine(Config cfg)
    : cfg_(std::move(cfg))
    , ouEst_(cfg_.ouWindowSize > 0 ? cfg_.ouWindowSize : 200,
             cfg_.ouMinTheta   > 0 ? cfg_.ouMinTheta   : 0.05)
    , volReg_(100)
    , geoImbalance_(cfg_.point, cfg_.atrPeriod)
    , calibrator_(cfg_)
{}

IntelligenceEngine::~IntelligenceEngine() {
    ouEst_  = math::OUEstimator();
    volReg_ = math::VolatilityRegime();
}

// ────────────────────────────────────────────────────────────
//  onTick — alimentation indicateurs + modules institutionnels
// ────────────────────────────────────────────────────────────
void IntelligenceEngine::onTick(const Tick& tick) noexcept {
    double mid = (tick.bid + tick.ask) * 0.5;
    if (mid <= 0.0) return;

    // OU Estimator
    ouEst_.push(mid);

    // Momentum fast/slow
    if (lastMid_ > 0.0) {
        double ret = (mid - lastMid_) / lastMid_ * 1e4;
        momentumFast_.update(ret);
        momentumSlow_.update(ret);
    }
    lastMid_ = mid;

    // Modules institutionnels (80%)
    geoImbalance_.pushTick(tick);
    instDelta_.pushTick(tick);
}

// ────────────────────────────────────────────────────────────
//  onBar — alimentation FVG depuis barres OHLC
// ────────────────────────────────────────────────────────────
void IntelligenceEngine::onBar(const OHLCBar& bar) noexcept {
    geoImbalance_.pushBar(bar);
}

// ────────────────────────────────────────────────────────────
//  classifyRegime — 5 états de marché (OU × ATR × Momentum)
// ────────────────────────────────────────────────────────────
IntelligenceEngine::Regime IntelligenceEngine::classifyRegime(
    double atrPoints,
    double rho1,
    double momentumFast,
    double momentumSlow) const noexcept
{
    VolRegime vr   = volReg_.classify(atrPoints);
    bool meanRev   = (rho1 < -0.15);
    bool trending  = (rho1 >  0.10);
    double momDiv  = momentumFast - momentumSlow;

    if (trending && momDiv > 0.3)  return Regime::TrendUp;
    if (trending && momDiv < -0.3) return Regime::TrendDown;
    if (!meanRev && !trending)     return Regime::Transitional;
    if (meanRev && vr == VolRegime::Low)  return Regime::RangeQuiet;
    if (meanRev && vr == VolRegime::High) return Regime::RangeVolatile;
    if (meanRev)                   return Regime::RangeQuiet;

    return Regime::Transitional;
}

// ────────────────────────────────────────────────────────────
//  computeGapFactors — facteurs régime (contribution 25%)
// ────────────────────────────────────────────────────────────
std::pair<double,double> IntelligenceEngine::computeGapFactors(
    Regime r,
    double netDelta,
    double totalLots,
    int    ssR,
    int    bsR,
    double pnlRatio) const noexcept
{
    double ss = 1.0, bs = 1.0;

    switch (r) {
        case Regime::RangeQuiet:    ss = 0.85; bs = 0.85; break;
        case Regime::RangeVolatile: ss = 1.25; bs = 1.25; break;
        case Regime::TrendUp:       ss = 1.45; bs = 0.80; break;
        case Regime::TrendDown:     ss = 0.80; bs = 1.45; break;
        default:                    ss = 1.05; bs = 1.05; break;
    }

    // Expansion recovery
    if (ssR > 0) ss *= (1.0 + ssR * cfg_.gapExpansionFactor);
    if (bsR > 0) bs *= (1.0 + bsR * cfg_.gapExpansionFactor);

    // Correction delta asymétrique
    if (totalLots > 1e-6) {
        double pressure = netDelta / totalLots;
        double asym     = cfg_.trailAsymmetryFactor;
        if (pressure > 0.0) {
            ss *= (1.0 + pressure * asym);
            bs *= (1.0 - pressure * asym * 0.5);
        } else if (pressure < 0.0) {
            bs *= (1.0 - pressure * asym);
            ss *= (1.0 + pressure * asym * 0.5);
        }
    }

    // Resserrement progressif du trail quand le P&L approche du target
    // Au-delà de 50% du target, le trail commence à se resserrer pour
    // verrouiller les gains. À 100% du target → facteur réduit de 25%.
    if (pnlRatio > 0.5) {
        double tightening = 1.0 - 0.25 * ((pnlRatio - 0.5) / 0.5);
        tightening = std::clamp(tightening, 0.75, 1.0);
        ss *= tightening;
        bs *= tightening;
    }

    ss = std::clamp(ss, 0.50, 3.0);
    bs = std::clamp(bs, 0.50, 3.0);
    return {ss, bs};
}

// ────────────────────────────────────────────────────────────
//  fuseGapFactors — fusion 80/20 des facteurs gap
//
//  PONDÉRATION INSTITUTIONNELLE :
//  35% régime OU/ATR
//  30% delta institutionnel (CVD + portfolio)
//  15% imbalance géométrique (FVG + OFI)
//  20% facteurs indicateurs (ATR expansion + recovery depth)
// ────────────────────────────────────────────────────────────
std::pair<double,double> IntelligenceEngine::fuseGapFactors(
    std::pair<double,double> regimeFact,
    std::pair<double,double> deltaFact,
    double                   imbalanceMult,
    double                   pnlRatio) const noexcept
{
    // Pondération institutionnelle
    static constexpr double W_REGIME   = 0.35;
    static constexpr double W_DELTA    = 0.30;
    static constexpr double W_IMBAL    = 0.15;
    static constexpr double W_NEUTRAL  = 0.20;  // composante neutre (1.0)

    double ssF = W_REGIME  * regimeFact.first
               + W_DELTA   * deltaFact.first
               + W_IMBAL   * imbalanceMult
               + W_NEUTRAL * 1.0;

    double bsF = W_REGIME  * regimeFact.second
               + W_DELTA   * deltaFact.second
               + W_IMBAL   * imbalanceMult
               + W_NEUTRAL * 1.0;

    // Correction finale selon ratio P&L : resserrement additionnel
    // au-delà de 70% du target pour sécuriser les gains (verrou final).
    if (pnlRatio > 0.70) {
        double finalTight = 1.0 - 0.15 * ((pnlRatio - 0.70) / 0.30);
        finalTight = std::clamp(finalTight, 0.85, 1.0);
        ssF *= finalTight;
        bsF *= finalTight;
    }

    // Plafond institutionnel : cap multiplicateur combiné
    // Évite que la multiplication cumulative (régime × delta × OFI × P&L)
    // produise des valeurs extrêmes (ex: 0.85 × 1.30 × 1.30 × 0.90 ≈ 1.29 — ok)
    // mais protège contre les cas pathologiques.
    static constexpr double MAX_TRAIL_FACTOR = 3.0;
    static constexpr double MIN_TRAIL_FACTOR = 0.50;
    ssF = std::clamp(ssF, MIN_TRAIL_FACTOR, MAX_TRAIL_FACTOR);
    bsF = std::clamp(bsF, MIN_TRAIL_FACTOR, MAX_TRAIL_FACTOR);
    return {ssF, bsF};
}

// ────────────────────────────────────────────────────────────
//  shouldHedge — signal delta-neutre
// ────────────────────────────────────────────────────────────
bool IntelligenceEngine::shouldHedge(
    double netDelta,
    double totalLots,
    Regime r,
    double riskScore) const noexcept
{
    if (totalLots < 0.01) return false;
    double pressure      = std::abs(netDelta) / totalLots;
    bool highPressure    = (pressure > 0.40);
    bool trendingRegime  = (r == Regime::TrendUp || r == Regime::TrendDown);
    bool canAct          = (riskScore < 75.0);
    return highPressure && trendingRegime && canAct;
}

// ────────────────────────────────────────────────────────────
//  computeRiskScore — score composite 0→100 (4 axes)
// ────────────────────────────────────────────────────────────
double IntelligenceEngine::computeRiskScore(
    double drawdownPct,
    double marginLevel,
    int    ssR,
    int    bsR,
    double hedgePressure) const noexcept
{
    int maxR = std::max(1, cfg_.maxRecovery);

    // [35%] Profondeur recovery
    double recovScore = std::min(1.0, (double)std::max(ssR, bsR) / maxR) * 35.0;

    // [30%] Pression marge
    double marginScore = 0.0;
    if (marginLevel > 0.0 && marginLevel < 500.0)
        marginScore = std::max(0.0, (500.0 - marginLevel) / 500.0) * 30.0;

    // [20%] Delta imbalance
    double deltaScore = std::clamp(hedgePressure, 0.0, 1.0) * 20.0;

    // [15%] Drawdown
    double ddScore = std::min(1.0, drawdownPct / 20.0) * 15.0;

    return std::min(100.0, recovScore + marginScore + deltaScore + ddScore);
}

// ────────────────────────────────────────────────────────────
//  buildReasoning — texte structuré complet
// ────────────────────────────────────────────────────────────
std::string IntelligenceEngine::buildReasoning(const Analysis& a,
                                                double pnlRatio) const
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    // [80%] Régime géométrique
    oss << "[80%] RÉGIME: " << regimeName(a.regime)
        << "  ρ₁=" << std::setprecision(3) << a.rho1
        << "  MomF=" << a.momentumFast
        << "  MomS=" << a.momentumSlow << "\n";

    // [80%] Delta institutionnel
    oss << "[80%] DELTA INST: " << deltaSignalName(a.deltaSignal)
        << "  CVD=" << std::setprecision(3) << a.cumulativeDelta
        << "  netΔP=" << std::setprecision(2)
        << (a.netDelta >= 0 ? "+" : "") << a.netDelta << "lots"
        << "  press=" << (a.hedgePressure * 100.0) << "%";
    if (a.needsHedge) {
        oss << "  ⚠ HEDGE: "
            << (a.hedgeType == 0 ? "BUY " : "SELL ")
            << a.hedgeLot << "lots";
    }
    if (a.deltaExhaustion) oss << "  ⏸ ÉPUISEMENT";
    oss << "\n";

    // [80%] Imbalance FVG + OFI
    oss << "[80%] IMBALANCE: score=" << std::setprecision(1) << a.imbalanceScore
        << "  OFI=" << std::setprecision(3) << a.ofiNormalized;
    if (a.ssAnchorFVG > 0) oss << "  FVG_SS@" << std::setprecision(2) << a.ssAnchorFVG;
    if (a.bsAnchorFVG > 0) oss << "  FVG_BS@" << a.bsAnchorFVG;
    if (a.displacementActive) oss << "  ⚡ DISPLACEMENT";
    oss << "\n";

    // [80%] Trail asymétrique
    oss << "[80%] TRAIL: ss×" << std::setprecision(3) << a.ssGapFactor
        << " bs×" << a.bsGapFactor
        << "  asym×" << a.asymmetryMult;
    if (a.trailFrozen) oss << "  ❄ FIGÉ (P&L " << (pnlRatio * 100.0) << "%)";
    oss << "\n";

    // [80%] Risque
    oss << "[80%] RISQUE: " << std::setprecision(1) << a.riskScore << "/100"
        << "  DD=" << a.drawdownPct << "%";
    if (a.marginAlert) oss << "  ⚠ MARGE CRITIQUE";
    if (a.cascadeRisk) oss << "  ⚠ RISQUE CASCADE";
    oss << "\n";

    // [20%] Velocity + prédiction
    oss << "[20%] VELOCITY: " << std::setprecision(3) << a.pnlVelocity << "$/tick"
        << "  closeProb=" << std::setprecision(1) << a.closeProbPct << "%\n";

    // Décision finale
    oss << "→ DÉCISION: ";
    if (a.riskScore >= 80)
        oss << "DÉFENSIF — réduction exposition";
    else if (a.deltaExhaustion)
        oss << "ATTENTE — épuisement institutionnel → pivot imminent";
    else if (a.deltaSignal == DeltaSignal::DivergenceBullish)
        oss << "ACCUMULATION — protéger BS, élargir SS";
    else if (a.deltaSignal == DeltaSignal::DivergenceBearish)
        oss << "DISTRIBUTION — protéger SS, élargir BS";
    else if (a.displacementActive)
        oss << "DISPLACEMENT — gel trail, attendre retour géométrique";
    else if (a.riskScore >= 50)
        oss << "PRUDENCE — trail élargi";
    else if (a.regime == Regime::RangeQuiet)
        oss << "OFFENSIF — range calme, capture rapide";
    else if (a.regime == Regime::TrendUp)
        oss << "BULLISH — asymétrie SS élargi, BS serré";
    else if (a.regime == Regime::TrendDown)
        oss << "BEARISH — asymétrie BS élargi, SS serré";
    else
        oss << "NEUTRE — stops standard";

    return oss.str();
}

// ────────────────────────────────────────────────────────────
//  analyze — point d'entrée principal (80/20 intégré)
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

    double mid = (tick.bid + tick.ask) * 0.5;

    // ── Audit ATR : vérification cohérence source unique ──────
    // L'ATR externe (atrPoints) vient du GridEngine ; l'ATR interne
    // est calculé depuis les barres OHLC dans GeometricImbalance.
    // Si les deux divergent de plus de 60%, log un avertissement.
    {
        double internalATR = geoImbalance_.atrValue();
        if (internalATR > 1e-9 && atrPoints > 1e-9) {
            double divergence = std::abs(atrPoints - internalATR) / internalATR;
            if (divergence > 0.60) {
                std::ostringstream warn;
                warn << std::fixed << std::setprecision(4);
                warn << "[AUDIT-ATR] Divergence ATR détectée: externe="
                     << atrPoints << " interne(geo)=" << internalATR
                     << " écart=" << (divergence * 100.0) << "% — vérifier source";
                // Limiter les logs répétitifs (log toutes les 500 appels)
                static int atrWarnCount = 0;
                if (++atrWarnCount % 500 == 1) {
                    std::cout << warn.str() << "\n";
                }
            }
        } else if (internalATR < 1e-9 && atrPoints > 1e-9) {
            // ATR interne à zéro = barres OHLC pas encore reçues
            static bool atrNotReadyLogged = false;
            if (!atrNotReadyLogged) {
                std::cout << "[AUDIT-ATR] ATR interne=0.0 — en attente de barres OHLC"
                          << " (utilisation ATR externe=" << atrPoints << ")\n";
                atrNotReadyLogged = true;
            }
        }
    }

    // ── Source ATR canonique — consolidation source unique ────
    // geoImbalance_.atrValue() est LA source de référence (barres OHLC).
    // L'ATR externe (atrPoints) sert uniquement de fallback à froid.
    // Tout le reste de analyze() utilise exclusivement effectiveATR.
    double effectiveATR = (geoImbalance_.atrValue() > 1e-9)
                        ? geoImbalance_.atrValue()
                        : atrPoints;
    a.atrPoints = effectiveATR;

    // ── [80%/1] Régime OU × ATR × Momentum ───────────────────
    ouEst_.push(mid);
    volReg_.push(effectiveATR);

    double rho1 = 0.0;
    if (ouEst_.ready()) {
        double ouGap = ouEst_.optimalGapPrice();
        rho1 = (ouGap > 0.0) ? -0.25 : 0.15;
    }

    double momF = momentumFast_.value();
    double momS = momentumSlow_.value();
    a.rho1         = rho1;
    a.momentumFast = momF;
    a.momentumSlow = momS;
    a.volRegime    = volReg_.classify(effectiveATR);
    a.regime       = classifyRegime(effectiveATR, rho1, momF, momS);

    // ── [80%/2] Delta institutionnel multi-niveau ─────────────
    double buyLots = 0.0, sellLots = 0.0;
    int    buyN = 0, sellN = 0;
    for (const auto& o : orders) {
        if (o.type == OrderType::Buy)  { buyLots  += o.lots; ++buyN;  }
        if (o.type == OrderType::Sell) { sellLots += o.lots; ++sellN; }
    }

    // Mise à jour du tracker delta institutionnel
    instDelta_.updatePortfolio(buyLots, sellLots, buyN, sellN);
    DeltaSnapshot deltaSnap = instDelta_.analyze();

    a.buyLots         = buyLots;
    a.sellLots        = sellLots;
    a.buyOrders       = buyN;
    a.sellOrders      = sellN;
    a.totalLots       = buyLots + sellLots;
    a.netDelta        = buyLots - sellLots;   // delta portefeuille (Niveau 1)
    a.hedgePressure   = (a.totalLots > 1e-6)
                      ? std::abs(a.netDelta) / a.totalLots
                      : 0.0;

    // Delta institutionnel flux (Niveaux 2-4)
    a.flowDeltaNet    = deltaSnap.flowDeltaNet;
    a.cumulativeDelta = deltaSnap.cumulativeDelta;
    a.deltaSignal     = deltaSnap.primarySignal;
    a.deltaExhaustion = deltaSnap.exhaustion;
    a.deltaReasoning  = deltaSnap.reasoning;

    // ── [80%/3] Imbalance géométrique (FVG + OFI) ────────────
    ImbalanceAnalysis imbAnalysis = geoImbalance_.analyze(mid);
    a.imbalanceScore     = imbAnalysis.imbalanceScore;
    a.ofiNormalized      = imbAnalysis.ofi.normalizedOFI;
    a.ssAnchorFVG        = imbAnalysis.ssAnchorPrice;
    a.bsAnchorFVG        = imbAnalysis.bsAnchorPrice;
    a.displacementActive = imbAnalysis.displacementDetected;
    a.asymmetryMult      = imbAnalysis.asymmetryMultiplier;
    a.imbalanceReasoning = imbAnalysis.summary;

    // ── [80%/4] Score de risque composite ─────────────────────
    double drawdownUSD = (account.balance > 0.0)
                       ? std::max(0.0, account.balance - account.equity)
                       : 0.0;
    double drawdownPct = (account.balance > 0.0)
                       ? drawdownUSD / account.balance * 100.0
                       : 0.0;

    a.drawdownUSD = drawdownUSD;
    a.drawdownPct = drawdownPct;
    a.marginAlert = (account.marginLevel > 0.0 && account.marginLevel < 150.0);
    a.cascadeRisk = (std::max(ssRecovery, bsRecovery) >= cfg_.maxRecovery - 3);
    a.riskScore   = computeRiskScore(drawdownPct, account.marginLevel,
                                      ssRecovery, bsRecovery, a.hedgePressure);

    // ── [80%/5] Signal de hedge delta-neutre ─────────────────
    a.needsHedge = shouldHedge(a.netDelta, a.totalLots, a.regime, a.riskScore);
    if (a.needsHedge) {
        a.hedgeType = (a.netDelta > 0) ? 1 : 0;
        a.hedgeLot  = std::round(std::abs(a.netDelta) * 100.0) / 100.0;
        a.hedgeLot  = std::max(a.hedgeLot, cfg_.baseLot);
    }

    // ── [80%/5b] Trail stop asymétrique — fusion 80/20 ───────
    double pnlRatio = (std::abs(basketTarget) > 1e-6)
                    ? basketPnL / basketTarget
                    : 0.0;

    // 35% régime ATR/OU
    auto regimeFact = computeGapFactors(a.regime, a.netDelta, a.totalLots,
                                         ssRecovery, bsRecovery, pnlRatio);

    // 30% delta institutionnel
    auto deltaFact  = std::make_pair(deltaSnap.suggestedSSFactor,
                                      deltaSnap.suggestedBSFactor);

    // 15% imbalance OFI
    double imbalMult = imbAnalysis.asymmetryMultiplier;

    // Fusion pondérée 80/20
    auto [ssF, bsF] = fuseGapFactors(regimeFact, deltaFact, imbalMult, pnlRatio);

    // Geler si displacement actif ou zone TP atteinte
    bool freezeByDisplace = (imbAnalysis.displacementDetected &&
                              imbAnalysis.displacementMagnitude > 3.0);
    bool freezeByTP       = (pnlRatio >= cfg_.trailFreezeZone && basketPnL > 0.0);
    bool freezeByExhaust  = (deltaSnap.suggestTrailFreeze);

    a.ssGapFactor  = ssF;
    a.bsGapFactor  = bsF;
    a.trailFrozen  = freezeByTP || freezeByDisplace || freezeByExhaust;
    a.trailTightness = (ssF + bsF) * 0.5;

    // ── [20%] Vélocité P&L et prédiction ─────────────────────
    double vel = pnlVel_.update(basketPnL);
    a.pnlVelocity = vel;

    if (basketTarget > 1e-6 && basketPnL >= basketTarget) {
        // Target déjà atteint ou dépassé → probabilité 100%
        a.estimatedTicksTarget = 0;
        a.closeProbPct         = 100.0;
    } else if (vel > 0.0 && basketTarget > basketPnL) {
        double ticksEst = (basketTarget - basketPnL) / vel;
        a.estimatedTicksTarget = (int)std::clamp(ticksEst, 1.0, 9999.0);
        a.closeProbPct = std::max(0.0, 100.0 * std::exp(-ticksEst / 10.0));
    } else {
        a.estimatedTicksTarget = 9999;
        a.closeProbPct         = 0.0;
    }

    // ── Paramètres calibrés courants ──────────────────────────
    a.calibratedParams = calibrator_.currentParams();

    // ── Raisonnement structuré ────────────────────────────────
    a.reasoning = buildReasoning(a, pnlRatio);

    // ── Mise à jour état interne ──────────────────────────────
    { std::lock_guard lk(mu_); lastRegime_ = a.regime; }

    return a;
}

// ────────────────────────────────────────────────────────────
//  recordCycleClosed — auto-apprentissage après fermeture basket
// ────────────────────────────────────────────────────────────
void IntelligenceEngine::recordCycleClosed(
    double pnlResult,
    double targetExpected,
    double drawdownPct,
    double gapUsedPoints,
    double gapOptimalPoints,
    int    recovDepth,
    double lotUsed,
    int    trailModeUsed,
    int    vLockTriggered,
    double vLockPrice,
    double ssProfitAtLockPts,
    int    trailModeBuyUsed,
    int    ssLockTriggered,
    double ssLockPrice,
    double bsProfitAtLockPts
) noexcept {
    // [1] Enregistrer dans le RollingKelly (déjà existant dans RecoveryEngine)
    // → fait dans TOP4EA_Engine::closeBasketAtomic()

    // [2] Construire et enregistrer l'erreur mathématique
    // Récupérer le dernier contexte d'analyse
    IntelligenceEngine::Regime lastR;
    {
        std::lock_guard lk(mu_);
        lastR = lastRegime_;
    }

    auto err = errorMem_.buildCycleError(
        /* atrPoints */       geoImbalance_.atrValue(),
        /* atrMult */         cfg_.atrMultiplier,
        /* gapUsed */         gapUsedPoints,
        /* gapOptimal */      gapOptimalPoints,
        /* rho1 */            ouEst_.ready() ? (ouEst_.optimalGapPrice() > 0 ? -0.25 : 0.15) : 0.0,
        /* regime */          (int)lastR,
        /* netDelta */        0.0,   // sera mis à jour si disponible
        /* hedgePressure */   0.0,
        /* recovDepth */      recovDepth,
        /* lotUsed */         lotUsed,
        /* pnlResult */       pnlResult,
        /* targetExpected */  targetExpected,
        /* drawdownPct */     drawdownPct
    );
    errorMem_.record(std::move(err));

    // [3] Export pour AutoML Python
    DeltaSnapshot ds = instDelta_.analyze();
    calibrator_.exportCycleToAutoML(
        geoImbalance_.atrValue(),
        ouEst_.ready() ? (ouEst_.optimalGapPrice() > 0 ? -0.25 : 0.15) : 0.0,
        (int)lastR,
        ds.portfolioDelta,
        ds.portfolioPressure,
        recovDepth,
        lotUsed,
        pnlResult,
        drawdownPct,
        std::abs(ds.cumulativeDelta) * 100.0,  // OFI score proxy
        geoImbalance_.analyze(lastMid_).imbalanceScore,
        cfg_.atrMultiplier,
        cfg_.kellyMaxFraction,
        cfg_.gapExpansionFactor,
        cfg_.trailAsymmetryFactor,
        // Métriques Dual Trail
        trailModeUsed,
        vLockTriggered,
        vLockPrice,
        ssProfitAtLockPts,
        trailModeBuyUsed,
        ssLockTriggered,
        ssLockPrice,
        bsProfitAtLockPts
    );

    // [4] Calibration périodique (tous les CALIB_EVERY cycles)
    ++cycleCount_;
    if (cycleCount_ % CALIB_EVERY == 0 && errorMem_.readyForCalibration()) {
        auto result = calibrator_.calibrate(errorMem_);
        if (result.gradientSteps > 0) {
            // Appliquer les paramètres calibrés à la config locale
            calibrator_.applyToConfig(cfg_);
        }
    }
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

// ────────────────────────────────────────────────────────────
//  computeDirectionVote — vote board directionnel [-5, +5]
//  Agrège 4 sources indépendantes en un score entier :
//    (1) Delta lots portfolio     : ±2
//    (2) Signal DeepSeek effectif : ±2
//    (3) Structure marché         : ±1
//    (4) Signal DXY proxy         : ±1
//  Seuils décision :
//    score ≤ -2 → SS-only (direction SELL confirmée)
//    score ≥ +2 → BS-only (direction BUY confirmée)
//    [-2, +2]   → grille symétrique standard
// ────────────────────────────────────────────────────────────
DirectionVote computeDirectionVote(
    double          buyLots,
    double          sellLots,
    double          totalLots,
    DSSignal        dsSignal,
    double          dsEffectiveConf,
    MarketStructure structure,
    DXYSignal       dxySignal,
    bool            lotsStale,
    int             gateMinScore,
    int             dsConfHigh,
    int             dsConfLow) noexcept
{
    DirectionVote vote;
    std::string r;

    // ── (1) Delta lots — poids ±2 — protection hedge ─────────
    // Logique de protection : si ≥70% des lots sont côté BUY (buy-heavy),
    // le portefeuille est SUREXPOSÉ aux BUY → on BLOQUE les nouvelles BUY (BS)
    // et on favorise SS (SELL) pour hedger/protéger. Score négatif = SS-only.
    // Inverse pour sell-heavy → score positif = BS-only (hedge SELL).
    // Task #17 : si le cache lots est périmé (coupure flux ticks), on
    // exclut totalement cette composante (poids de repli = 0) plutôt que
    // de voter sur une exposition potentiellement obsolète.
    if (lotsStale) {
        r += "delta=STALE(skip) ";
    } else if (totalLots > 1e-6) {
        double bsPct  = buyLots  / totalLots;  // fraction lots BUY
        double ssPct  = sellLots / totalLots;  // fraction lots SELL
        if (bsPct >= 0.70) {
            // Surexposé BUY → favoriser SELL (hedge), bloquer nouvelles BUY
            vote.score -= 2;
            r += "delta=" + std::to_string((int)(bsPct * 100)) + "%buy-heavy(-2:hedge) ";
        } else if (bsPct >= 0.55) {
            vote.score -= 1;
            r += "delta=" + std::to_string((int)(bsPct * 100)) + "%buy-heavy(-1) ";
        } else if (ssPct >= 0.70) {
            // Surexposé SELL → favoriser BUY (hedge), bloquer nouvelles SELL
            vote.score += 2;
            r += "delta=" + std::to_string((int)(ssPct * 100)) + "%sell-heavy(+2:hedge) ";
        } else if (ssPct >= 0.55) {
            vote.score += 1;
            r += "delta=" + std::to_string((int)(ssPct * 100)) + "%sell-heavy(+1) ";
        } else {
            r += "delta=balanced ";
        }
    } else {
        r += "delta=none ";
    }

    // ── (2) DeepSeek signal — poids ±2 / ±1 (seuils configurables) ────────
    // dsConfHigh → ±2, dsConfLow → ±1
    if (dsSignal == DSSignal::BUY_BIAS && dsEffectiveConf >= (double)dsConfHigh) {
        vote.score += 2;
        r += "DS:BUY(" + std::to_string((int)dsEffectiveConf) + "%)(+2) ";
    } else if (dsSignal == DSSignal::BUY_BIAS && dsEffectiveConf >= (double)dsConfLow) {
        vote.score += 1;
        r += "DS:BUY(" + std::to_string((int)dsEffectiveConf) + "%)(+1) ";
    } else if (dsSignal == DSSignal::SELL_BIAS && dsEffectiveConf >= (double)dsConfHigh) {
        vote.score -= 2;
        r += "DS:SELL(" + std::to_string((int)dsEffectiveConf) + "%)(-2) ";
    } else if (dsSignal == DSSignal::SELL_BIAS && dsEffectiveConf >= (double)dsConfLow) {
        vote.score -= 1;
        r += "DS:SELL(" + std::to_string((int)dsEffectiveConf) + "%)(-1) ";
    } else {
        r += "DS:neut ";
    }

    // ── (3) Structure marché — poids ±1 ──────────────────────
    switch (structure) {
        case MarketStructure::BULLISH_TREND:
        case MarketStructure::BREAKOUT_UP:
            vote.score += 1;
            r += "struct:BULL(+1) ";
            break;
        case MarketStructure::BEARISH_TREND:
        case MarketStructure::BREAKOUT_DOWN:
            vote.score -= 1;
            r += "struct:BEAR(-1) ";
            break;
        default:
            r += "struct:neut ";
            break;
    }

    // ── (4) Signal DXY proxy (corrélation inverse XAU) ────────
    // DXY BULL (dollar fort) → XAU structurellement baissier → -1
    // DXY BEAR (dollar faible) → XAU structurellement haussier → +1
    switch (dxySignal) {
        case DXYSignal::BULL:
            vote.score -= 1;
            r += "DXY:BULL(-1) ";
            break;
        case DXYSignal::BEAR:
            vote.score += 1;
            r += "DXY:BEAR(+1) ";
            break;
        default:
            r += "DXY:neut ";
            break;
    }

    // ── Clamp et décision ─────────────────────────────────────
    if (vote.score < -5) vote.score = -5;
    if (vote.score >  5) vote.score =  5;

    vote.ssOnly = (vote.score <= -gateMinScore);
    vote.bsOnly = (vote.score >= +gateMinScore);
    vote.reason = r + "→score=" + std::to_string(vote.score);

    return vote;
}

} // namespace top4ea
