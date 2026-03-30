#pragma once
// ============================================================
//  TOP4EA — geometric_imbalance.hpp
//  Détection d'imbalance géométrique institutionnelle
//
//  THÉORIE INSTITUTIONNELLE (80% Math Pur) :
//  ─────────────────────────────────────────────────────────
//  Les institutions placent leurs ordres en blocs massifs
//  qui laissent des "empreintes géométriques" dans le prix :
//
//  1. FAIR VALUE GAP (FVG) — Déséquilibre de prix
//     Lacune entre high[i-2] et low[i] ou low[i-2] et high[i]
//     créée par un déplacement impulsif d'un gros acteur.
//     → Zone où le prix REVIENT pour combler le gap (55-75% du temps)
//     → Ancre naturelle pour la grille en mode mean-reversion
//
//  2. ORDER FLOW IMBALANCE (OFI) — Déséquilibre de flux
//     Mesure la pression acheteuse vs vendeuse tick par tick :
//       OFI = Σ(ask_delta > 0 ? vol : 0) - Σ(bid_delta < 0 ? vol : 0)
//     En l'absence de volume (MT4) : proxy bid/ask movement
//     OFI positif → pression acheteuse institutionnelle
//     OFI négatif → pression vendeuse institutionnelle
//
//  3. DISPLACEMENT — Déplacement directionnel fort
//     Bougie avec un corps ≥ 3×ATR moyen
//     → Signal de liquidation ou d'injection institutionnelle
//     → Le prochain niveau de grille doit anticiper le retour
//
//  4. BREAKER BLOCK — Zone de retournement géométrique
//     Après un FVG, si le prix revient et DÉPASSE le FVG,
//     la zone devient un "breaker" (résistance → support ou inverse)
//     → Le gap est recalibré géométriquement
//
//  UTILISATION DANS LA GRILLE :
//  ─────────────────────────────────────────────────────────
//  - Si FVG bullish détecté en dessous du prix → SS ancré sur FVG
//  - Si FVG bearish détecté au dessus du prix  → BS ancré sur FVG
//  - OFI fort → extension asymétrique du côté dominant
//  - Displacement → gel de trail + attente du retour géométrique
// ============================================================
#include "types.hpp"
#include "math_core.hpp"
#include <deque>
#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <optional>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  OHLC Bar — barre de données de marché
// ────────────────────────────────────────────────────────────
struct OHLCBar {
    double open   = 0.0;
    double high   = 0.0;
    double low    = 0.0;
    double close  = 0.0;
    double spread = 0.0;
    long long timestamp = 0;

    [[nodiscard]] double body()    const noexcept { return std::abs(close - open); }
    [[nodiscard]] double range()   const noexcept { return high - low; }
    [[nodiscard]] bool   bullish() const noexcept { return close >= open; }
    [[nodiscard]] bool   bearish() const noexcept { return close < open; }
    [[nodiscard]] double mid()     const noexcept { return (high + low) * 0.5; }
};

// ────────────────────────────────────────────────────────────
//  Zone d'imbalance (FVG / Breaker Block)
// ────────────────────────────────────────────────────────────
struct ImbalanceZone {
    enum class Type : uint8_t {
        FVG_BULLISH   = 0,   // gap haussier — zone de support
        FVG_BEARISH   = 1,   // gap baissier — zone de résistance
        BREAKER_BULL  = 2,   // breaker haussier (ancien résistance → support)
        BREAKER_BEAR  = 3,   // breaker baissier (ancien support → résistance)
        DISPLACEMENT  = 4    // déplacement impulsif
    };

    Type   type        = Type::FVG_BULLISH;
    double priceHigh   = 0.0;   // borne haute de la zone
    double priceLow    = 0.0;   // borne basse de la zone
    double midpoint    = 0.0;   // point médian (ancre grille)
    double strength    = 0.0;   // force ∈ [0,1] (taille gap / ATR moyen)
    int    age         = 0;     // nombre de barres depuis création
    bool   tested      = false; // prix est-il entré dans la zone ?
    bool   mitigated   = false; // zone totalement comblée (invalidée)

    [[nodiscard]] bool contains(double price) const noexcept {
        return price >= priceLow && price <= priceHigh;
    }
    [[nodiscard]] double width() const noexcept { return priceHigh - priceLow; }
    [[nodiscard]] bool valid() const noexcept { return !mitigated && age < 100; }

    [[nodiscard]] bool isBullish() const noexcept {
        return type == Type::FVG_BULLISH || type == Type::BREAKER_BULL;
    }
    [[nodiscard]] bool isBearish() const noexcept {
        return type == Type::FVG_BEARISH || type == Type::BREAKER_BEAR;
    }
};

// ────────────────────────────────────────────────────────────
//  Order Flow Imbalance (OFI) snapshot
// ────────────────────────────────────────────────────────────
struct OFISnapshot {
    double cumulativeOFI     = 0.0;   // OFI cumulatif sur la fenêtre
    double normalizedOFI     = 0.0;   // ∈ [-1, +1]
    double ofiMomentum       = 0.0;   // dérivée de l'OFI (accélération)
    double buyPressure       = 0.0;   // pression acheteuse nette [0,1]
    double sellPressure      = 0.0;   // pression vendeuse nette [0,1]
    bool   dominantBuy       = false; // pression acheteuse dominante
    bool   dominantSell      = false; // pression vendeuse dominante
    double imbalanceRatio    = 1.0;   // max(buy,sell) / min(buy,sell)
};

// ────────────────────────────────────────────────────────────
//  Résultat global de l'analyse d'imbalance
// ────────────────────────────────────────────────────────────
struct ImbalanceAnalysis {
    // Zones actives
    std::vector<ImbalanceZone> activeZones;

    // Zone la plus proche (ancre grille suggérée)
    std::optional<ImbalanceZone> nearestBullish;
    std::optional<ImbalanceZone> nearestBearish;

    // OFI courant
    OFISnapshot ofi;

    // Displacement
    bool   displacementDetected = false;
    double displacementMagnitude = 0.0;  // en multiples d'ATR
    bool   displacementBullish  = false;

    // Signaux grille
    double ssAnchorPrice        = 0.0;  // ancre suggérée SellStop (FVG bullish bas)
    double bsAnchorPrice        = 0.0;  // ancre suggérée BuyStop  (FVG bearish haut)
    double asymmetryMultiplier  = 1.0;  // extension côté imbalance dominant

    // Score d'imbalance globale [0,100]
    double imbalanceScore       = 0.0;
    std::string summary;
};

// ============================================================
//  FVGDetector — Détecteur de Fair Value Gaps
// ============================================================
class FVGDetector {
public:
    static constexpr int MIN_BARS_FOR_FVG = 3;
    static constexpr int MAX_ZONES        = 20;
    static constexpr double MIN_GAP_ATR   = 0.30;  // gap min = 30% ATR

    explicit FVGDetector(double pointSize, int maxZones = MAX_ZONES)
        : pointSize_(pointSize), maxZones_(maxZones) {}

    // Alimentation barre OHLC (appelée à chaque clôture de barre)
    void pushBar(const OHLCBar& bar) noexcept;

    // Vérification si une zone est comblée (mitigation)
    void updateMitigation(double currentPrice) noexcept;

    // Zones actives (non comblées)
    [[nodiscard]] std::vector<ImbalanceZone> activeZones() const noexcept {
        std::lock_guard lk(mu_);
        std::vector<ImbalanceZone> out;
        for (const auto& z : zones_)
            if (z.valid()) out.push_back(z);
        return out;
    }

    // Zone bullish la plus proche en dessous du prix courant
    [[nodiscard]] std::optional<ImbalanceZone> nearestBullish(double price) const noexcept;

    // Zone bearish la plus proche au dessus du prix courant
    [[nodiscard]] std::optional<ImbalanceZone> nearestBearish(double price) const noexcept;

    // ATR moyen des N dernières barres (pour normaliser la force)
    [[nodiscard]] double avgATR() const noexcept { return atr_.value(); }

    void setPointSize(double ps) noexcept { pointSize_ = ps; }

private:
    void detectFVG(const OHLCBar& b0, const OHLCBar& b1, const OHLCBar& b2) noexcept;
    void ageBars() noexcept;
    void pruneOldZones() noexcept;

    double                     pointSize_;
    int                        maxZones_;
    std::deque<OHLCBar>        bars_;      // buffer des 3 dernières barres
    std::deque<ImbalanceZone>  zones_;
    math::ATR                  atr_   { 14 };
    mutable std::mutex         mu_;
};

// ============================================================
//  OFICalculator — Calcul de l'Order Flow Imbalance
// ============================================================
class OFICalculator {
public:
    static constexpr int OFI_WINDOW = 100;  // fenêtre de calcul OFI

    explicit OFICalculator(int window = OFI_WINDOW)
        : window_(window)
        , ofiEwma_(0.10)
        , ofiMomEwma_(0.20)
        , buyPressEwma_(0.10)
        , sellPressEwma_(0.10)
    {}

    // Alimentation tick (bid/ask delta comme proxy OFI)
    // En l'absence de volume réel, on utilise le sens du mouvement
    // et l'amplitude relative pour estimer la pression
    void pushTick(const Tick& tick) noexcept;

    // Snapshot OFI courant
    [[nodiscard]] OFISnapshot snapshot() const noexcept;

    // Pression nette normalisée ∈ [-1, +1]
    [[nodiscard]] double normalizedPressure() const noexcept {
        return ofiEwma_.value();
    }

    void reset() noexcept;

private:
    int                window_;
    double             prevBid_    = 0.0;
    double             prevAsk_    = 0.0;
    double             cumOFI_     = 0.0;
    double             maxOFI_     = 1e-9;  // pour normalisation dynamique
    std::deque<double> ofiHistory_;

    math::EWMA         ofiEwma_;
    math::EWMA         ofiMomEwma_;
    math::EWMA         buyPressEwma_;
    math::EWMA         sellPressEwma_;
};

// ============================================================
//  GeometricImbalance — Analyseur d'imbalance global
// ============================================================
class GeometricImbalance {
public:
    explicit GeometricImbalance(double pointSize, double atrPeriod = 14)
        : fvgDetector_(pointSize)
        , ofiCalc_()
        , atr_((int)atrPeriod)
        , pointSize_(pointSize)
    {}

    // ─── Alimentation données ─────────────────────────────────
    void pushBar (const OHLCBar& bar)  noexcept;
    void pushTick(const Tick& tick)    noexcept;

    // ─── Analyse complète ─────────────────────────────────────
    [[nodiscard]] ImbalanceAnalysis analyze(double currentPrice) const noexcept;

    // ─── Ancres géométriques pour la grille ───────────────────
    //  Retourne 0 si aucune zone valide n'est disponible
    [[nodiscard]] double suggestSSAnchor(double currentPrice) const noexcept;
    [[nodiscard]] double suggestBSAnchor(double currentPrice) const noexcept;

    // ─── Multiplicateur d'asymétrie basé sur l'OFI ────────────
    //  > 1.0 → étendre le côté dominant
    [[nodiscard]] double asymmetryMultiplier() const noexcept;

    // ─── Détection displacement ───────────────────────────────
    [[nodiscard]] bool   displacementActive() const noexcept;
    [[nodiscard]] double displacementMagnitude() const noexcept;

    void setPointSize(double ps) noexcept {
        pointSize_ = ps;
        fvgDetector_.setPointSize(ps);
    }

    [[nodiscard]] double atrValue() const noexcept {
        std::lock_guard lk(mu_);
        return atr_.value();
    }

private:
    bool detectDisplacement(const OHLCBar& bar) const noexcept;

    FVGDetector      fvgDetector_;
    OFICalculator    ofiCalc_;
    math::ATR        atr_;
    double           pointSize_;

    mutable double   lastDisplaceMag_ = 0.0;
    mutable bool     lastDisplaceBull_ = false;
    mutable bool     displacementActive_ = false;

    std::deque<OHLCBar> recentBars_;
    mutable std::mutex  mu_;
};

} // namespace top4ea
