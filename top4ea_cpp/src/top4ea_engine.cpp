// ============================================================
//  TOP4EA — top4ea_engine.cpp
//  Moteur principal C++20 — machine à états centrale
// ============================================================
#include "../include/top4ea_engine.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <thread>
#include <future>
#include <ctime>
#include <cmath>
#include <fstream>
#include <filesystem>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Constructor
// ────────────────────────────────────────────────────────────
TOP4EA_Engine::TOP4EA_Engine(Config cfg)
    : cfg_(std::move(cfg))
    , vlMgr_(cfg_)
{
    bridge_      = std::make_unique<MetaApiBridge>(cfg_);
    grid_        = std::make_unique<GridEngine>(cfg_);
    basketMon_   = std::make_unique<BasketMonitor>(cfg_);
    recovery_    = std::make_unique<RecoveryEngine>(cfg_);
    intel_       = std::make_unique<IntelligenceEngine>(cfg_);
    marketIntel_ = std::make_unique<MarketIntelligence>();

    // Callbacks bridge
    bridge_->onTick   ([this](const Tick& t)            { onTick(t);    });
    bridge_->onOrder  ([this](const OrderInfo& o)       { onOrder(o);   });
    bridge_->onAccount([this](const AccountSnapshot& a) { onAccount(a); });
    bridge_->onError  ([this](int c, const std::string& m){ onError(c,m); });
    bridge_->onConnect([this](bool b)                   { onConnect(b); });
}

TOP4EA_Engine::~TOP4EA_Engine() {
    stop();
    // Destruction explicite dans l'ordre pour éviter toute
    // corruption de tas résiduelle (deque nodes libérés proprement)
    { std::lock_guard lk(ordersMu_); orders_.clear(); }
    intel_.reset();
    recovery_.reset();
    basketMon_.reset();
    grid_.reset();
    bridge_.reset();
}

// ────────────────────────────────────────────────────────────
//  Lifecycle
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::start() {
    running_.store(true);
    log("TOP4EA C++20 démarré — " + cfg_.symbol
        + " | digits(config)=" + std::to_string(cfg_.digits)
        + " | Target=" + std::to_string(cfg_.basketTarget) + "$");

    // ── [1] Auto-détection broker AVANT les threads de polling ─
    // Appel HTTP synchrone brut — ne dépend pas de bridge_->start()
    // Si échoue (weekend/offline) → conserve config fichier (fallback safe)
    detectAndApplyBrokerSpec();

    bridge_->start();

    // Initialisation objectif journalier
    auto acc = bridge_->requestAccount(3000);
    if (acc) {
        std::lock_guard lk(accMu_);
        account_          = *acc;
        dailyStartBalance_= acc->balance;
        log("Compte: balance=" + std::to_string(acc->balance)
            + " " + acc->currency);
    }
    dailyResetTime_     = Clock::now();
    dailyTargetReached_ = false;

    // Thread dashboard (toutes les 500ms)
    dashThread_ = std::jthread([this](std::stop_token st){
        while (!st.stop_requested() && running_.load()) {
            std::cout << dashboard() << "\n";
            std::this_thread::sleep_for(Ms(500));
        }
    });
}

// ============================================================
//  detectAndApplyBrokerSpec
//  Interroge MetaApi /symbols/{sym} avant le démarrage des threads.
//  Applique les specs si détectées, sinon conserve config fichier.
// ============================================================
void TOP4EA_Engine::detectAndApplyBrokerSpec() {
    log("[BrokerSpec] Détection spécifications broker " + cfg_.symbol + " ...");

    SymbolSpec spec = bridge_->requestSymbolSpec(cfg_.symbol, 8000);

    if (spec.detected) {
        // Vérifier si la config fichier surcharge manuellement (digits != 0)
        // digits=0 dans top4ea.conf = auto-détection pure
        // digits>0 = surcharge manuelle (ex: test ou broker exotique)
        bool manualOverride = (cfg_.digits > 0) &&
                              (cfg_.digits != spec.digits);
        if (manualOverride) {
            log("[BrokerSpec] SURCHARGE MANUELLE : config fichier digits="
                + std::to_string(cfg_.digits)
                + " vs broker digits=" + std::to_string(spec.digits)
                + " → config fichier utilisée");
            // On stocke la spec détectée mais on n'applique que les champs
            // non-critiques (volumes, stopLevel) sans changer digits/point
            symbolSpec_           = spec;
            cfg_.tickValue        = spec.tickValue;    // toujours prendre
            cfg_.volumeMin        = spec.volumeMin;
            cfg_.volumeMax        = spec.volumeMax;
            cfg_.volumeStep       = spec.volumeStep;
            cfg_.stopLevelPoints  = spec.stopLevelPoints;
            bridge_->updateConfig(cfg_);               // sync précision JSON
        } else {
            applySymbolSpec(spec);
        }
    } else {
        log("[BrokerSpec] Détection échouée — config fichier conservée"
            " (digits=" + std::to_string(cfg_.digits) + ","
            " point=" + std::to_string(cfg_.point) + ","
            " tickValue=" + std::to_string(cfg_.tickValue) + ")");
    }
}

// ────────────────────────────────────────────────────────────
//  applySymbolSpec
//  Applique la spec broker à cfg_ puis recrée tous les sous-systèmes.
//  Doit être appelé AVANT bridge_->start() (pas de threads actifs).
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::applySymbolSpec(const SymbolSpec& spec) {
    const int oldDigits  = cfg_.digits;
    const double oldPt   = cfg_.point;

    // ── Précision & valeur de tick ───────────────────────────
    cfg_.digits      = spec.digits;
    cfg_.point       = spec.point;
    cfg_.tickValue   = spec.tickValue;

    // ── Volumes & stop level ─────────────────────────────────
    cfg_.volumeMin       = spec.volumeMin;
    cfg_.volumeMax       = spec.volumeMax;
    cfg_.volumeStep      = spec.volumeStep;
    cfg_.stopLevelPoints = spec.stopLevelPoints;
    cfg_.contractSize    = (spec.contractSize > 0.0) ? spec.contractSize
                                                      : cfg_.contractSize;

    // ── Rescale des paramètres grille en points ──────────────
    // Les valeurs dans top4ea.conf (grid_gap=500, atr_grid_min=200…)
    // sont définies pour un broker 2-digit (point=0.01).
    // Pour Exness 3-digit (point=0.001) :
    //   500 pts @ 2d = 500 × 0.01 = 5.00$ de déplacement prix
    //   500 pts @ 3d = 500 × 0.001 = 0.50$ → il faut 5000 pts
    // On rescale pour conserver la même amplitude en $ du prix réel.
    if (oldDigits > 0 && spec.digits != oldDigits) {
        double scale = std::pow(10.0, (double)(spec.digits - oldDigits));
        cfg_.gridGap         = (int)std::round((double)cfg_.gridGap * scale);
        cfg_.atrGridMin      = cfg_.atrGridMin * scale;
        cfg_.atrGridMax      = cfg_.atrGridMax * scale;
        // ── CORRECTION : rescale VL pts en cohérence avec gridGap ──
        // vlActivationPts et vlTrailPts sont définis en "points" référencés
        // au broker 2-digit (point=0.01). Si le broker utilise un nombre de
        // décimales différent, on rescale pour conserver la même amplitude prix.
        cfg_.vlActivationPts = (int)std::round((double)cfg_.vlActivationPts * scale);
        cfg_.vlTrailPts      = (int)std::round((double)cfg_.vlTrailPts * scale);
        // Rescale aussi trail_fixed_dist_pts et cascade_dispatch_pips
        cfg_.trailFixedDistPts = (int)std::round((double)cfg_.trailFixedDistPts * scale);
        log("[BrokerSpec] Rescale grille ×" + std::to_string(scale)
            + " (digits " + std::to_string(oldDigits)
            + "→" + std::to_string(spec.digits)
            + ") gridGap=" + std::to_string(cfg_.gridGap)
            + " atrMin=" + std::to_string(cfg_.atrGridMin)
            + " atrMax=" + std::to_string(cfg_.atrGridMax)
            + " vlAct=" + std::to_string(cfg_.vlActivationPts)
            + " vlTrail=" + std::to_string(cfg_.vlTrailPts));
    }

    // ── Sync bridge JSON (précision prix dans cmdToJson) ─────
    bridge_->updateConfig(cfg_);

    // ── Recréer tous les sous-systèmes avec la config corrigée ─
    rebuildSubsystems();

    symbolSpec_ = spec;

    log("[BrokerSpec] APPLIQUÉ : digits=" + std::to_string(cfg_.digits)
        + " point=" + std::to_string(cfg_.point)
        + " (était " + std::to_string(oldPt) + ")"
        + " tickValue=" + std::to_string(cfg_.tickValue) + "$/lot/pt"
        + " | " + spec.report());
}

// ────────────────────────────────────────────────────────────
//  rebuildSubsystems
//  Recrée grid, basket, recovery, intel avec cfg_ mis à jour.
//  Appelé uniquement avant bridge_->start() — pas de lock requis.
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::rebuildSubsystems() {
    grid_        = std::make_unique<GridEngine>       (cfg_);
    basketMon_   = std::make_unique<BasketMonitor>    (cfg_);
    recovery_    = std::make_unique<RecoveryEngine>   (cfg_);
    intel_       = std::make_unique<IntelligenceEngine>(cfg_);
    marketIntel_ = std::make_unique<MarketIntelligence>();
    vlMgr_       = ProfitLockManager(cfg_);
    log("[BrokerSpec] Sous-systèmes recréés avec config broker détectée");
}

void TOP4EA_Engine::stop() {
    running_.store(false);
    if (dashThread_.joinable()) {
        dashThread_.request_stop();
        dashThread_.join();
    }
    bridge_->stop();
    log("TOP4EA stoppé.");
}

// ────────────────────────────────────────────────────────────
//  Callbacks depuis MT4Bridge
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::onTick(const Tick& tick) {
    {
        std::lock_guard lk(tickMu_);
        lastTick_ = tick;
    }
    grid_->onTick(tick);
    recovery_->onTick(tick);  // alimente le CascadeGuard (spread + momentum)
    intel_->onTick(tick);     // alimente les indicateurs intelligence (OU, momentum)
    processTick(tick);
}

void TOP4EA_Engine::onOrder(const OrderInfo& ord) {
    std::lock_guard lk(ordersMu_);
    // Mettre à jour ou insérer
    auto it = std::find_if(orders_.begin(), orders_.end(),
                           [&](const OrderInfo& o){ return o.ticket == ord.ticket; });
    if (it != orders_.end()) *it = ord;
    else                      orders_.push_back(ord);
}

void TOP4EA_Engine::onAccount(const AccountSnapshot& acc) {
    std::lock_guard lk(accMu_);
    account_ = acc;
}

void TOP4EA_Engine::onError(int code, const std::string& msg) {
    std::cerr << "[MetaApi ERR " << code << "] " << msg << "\n";
}

void TOP4EA_Engine::onConnect(bool connected) {
    if (connected) {
        log("MetaApi connecté/reconnecté");
        // Effacer les tickets SS/BS stale pour éviter tout faux
        // déclenchement géométrique pendant la fenêtre de reconnexion.
        // restoreState() les rétablira proprement depuis le broker.
        ssTicket_.store(-1);  bsTicket_.store(-1);
        ssLevel_.store(0.0);  bsLevel_.store(0.0);
        state_.store(EAState::Init);
    } else {
        log("MetaApi déconnecté — en attente de reconnexion");
    }
}

// ────────────────────────────────────────────────────────────
//  Traitement principal par tick
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::processTick(const Tick& tick) {
    if (!bridge_->isConnected()) return;

    // ── Anti-gel CLOSING ─────────────────────────────────────────
    // Si l'état est CLOSING depuis plus de 30 secondes sans que basketClosing_
    // soit actif, c'est un gel : les ordres ont déjà été fermés côté broker
    // mais le moteur n'a pas reçu confirmation (réseau). Force reset → Init.
    if (state_.load() == EAState::Closing) {
        bool activeClose = basketClosing_.load();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           Clock::now() - closingStart_).count();
        if (!activeClose || elapsed > 30) {
            log("[WARN] État CLOSING gel détecté (elapsed=" + std::to_string(elapsed)
                + "s, basketClosing=" + (activeClose ? "OUI" : "NON")
                + ") — force reset Init + vidage cache");
            ssTicket_.store(-1); bsTicket_.store(-1);
            ssLevel_.store(0.0); bsLevel_.store(0.0);
            recovery_->reset();
            basketMon_->reset();
            basketClosing_.store(false);
            { std::lock_guard lk(ordersMu_); orders_.clear(); }
            {
                Tick t; { std::lock_guard lk(tickMu_); t = lastTick_; }
                gridOrigin_ = (t.bid + t.ask) * 0.5;
            }
            state_.store(EAState::Init);
        }
        return;  // Laisser closeBasketAtomic() finir son travail ou attendre reset
    }

    // ── MaxRecoveryHold — surveillance continue du panier ────────────
    // Positions ouvertes conservées après épuisement de la Martingale.
    // Le basketMon gère la sortie en profit. On surveille ici le seuil
    // d'urgence pour limiter le drawdown si le marché continue contre nous.
    if (state_.load() == EAState::MaxRecoveryHold) {
        // Surveillance basket : fermeture normale si le marché revient en profit
        {
            std::vector<OrderInfo> cached;
            { std::lock_guard lk(ordersMu_); cached = orders_; }
            double pnl      = basketProfit(cached);
            double cyclePnl = cycleRealizedPnL_ + pnl;
            int    totalRec = recovery_->sellCount() + recovery_->buyCount();
            // Tester le target basket (même logique que la section 3 normale)
            if (!cached.empty() && basketMon_->update(cyclePnl, cached, totalRec)) {
                if (!basketClosing_.exchange(true)) {
                    closingStart_ = Clock::now();
                    log(">>> [MaxRecoveryHold] BASKET TARGET atteint après hold — fermeture en profit"
                        " cyclePnL=" + std::to_string(cyclePnl) + "$");
                    closeBasketAtomic();
                    basketClosing_.store(false);
                }
                return;
            }
            // Surveillance du seuil d'urgence (basket_max_loss)
            if (cfg_.basketMaxLoss < 0.0 && pnl <= cfg_.basketMaxLoss) {
                log("[MaxRecoveryHold] SEUIL D'URGENCE atteint : P&L="
                    + std::to_string(pnl) + "$ <= "
                    + std::to_string(cfg_.basketMaxLoss) + "$ — sortie forcée");
                emergencyCloseAll(pnl);
            }
        }
        return;  // Aucun trail, aucun nouvel ordre en mode hold
    }

    // 0. Visual AI — mise à jour iniMTA + décision marché temps réel
    //    Appelé avant toute logique de grille pour nourrir l'orchestrateur
    {
        // Contexte panier pour prompt DeepSeek — lire cache sans fetch réseau
        std::vector<OrderInfo> cached;
        { std::lock_guard lk(ordersMu_); cached = orders_; }
        double bPnl  = basketProfit(cached);
        double bsLots = 0.0, ssLots = 0.0;
        int    nMkt  = 0;
        for (const auto& o : cached) {
            if (o.type == OrderType::Buy)  { bsLots += o.lots; ++nMkt; }
            if (o.type == OrderType::Sell) { ssLots += o.lots; ++nMkt; }
        }
        bool inRecov = (state_.load() == EAState::Recovery);
        marketIntel_->update(tick, cfg_.point, bPnl, nMkt,
                             bsLots, ssLots, inRecov);
        // Reset auto si signal DeepSeek inactif > 5min → force nouvel appel au tick suivant
        marketIntel_->dsResetIfStale(300);
    }
    {
        VisualDecision vd = marketIntel_->analyze();
        orchestrator_.applyDecision(vd);
    }

    // 1. Objectif journalier
    resetDailyIfNeeded();
    // Si daily désactivé (calibrage) : sortir de PAUSE_DAILY automatiquement
    if (cfg_.dailyTargetPct <= 0.0) {
        if (state_.load() == EAState::DailyPause)
            state_.store(EAState::Init);
        dailyTargetReached_ = false;
    } else {
        checkDailyTarget();
        if (dailyTargetReached_) return;
    }

    // 2. Détection géométrique pure — CHAQUE tick, aucune gate temporelle
    //    Le prix franchit un niveau de grille → recovery déclenché instantanément
    //    C'est la seule source de vérité pour les déclenchements.
    detectGeometricTriggers(tick);

    // 3. Vérification basket P&L à CHAQUE tick sur ordres cachés (< 1µs)
    //    ─ Avant la gate 1 seconde pour ne rater aucune opportunité ≤50ms ─
    {
        std::vector<OrderInfo> cached;
        { std::lock_guard lk(ordersMu_); cached = orders_; }
        double pnl      = basketProfit(cached);
        // P&L total du cycle = profit flottant + profit déjà encaissé (VL partiels)
        double cyclePnl = cycleRealizedPnL_ + pnl;
        if (!cached.empty()) {
            // ── TP par vagues ──────────────────────────────────────
            // Exécuté AVANT le target basket global pour capturer les profits intermédiaires
            // Wave1 (50% des positions) → Wave2 (30% du restant) → trail reliquat
            if (cfg_.waveEnabled && !basketClosing_.load()) {
                int ws = waveState_.load();
                if (ws == 0 && cyclePnl >= cfg_.wave1Target) {
                    if (waveState_.compare_exchange_strong(ws, 1)) {
                        log(">>> WAVE1 cyclePnL=" + std::to_string(cyclePnl)
                            + " (réalisé=" + std::to_string(cycleRealizedPnL_)
                            + " flottant=" + std::to_string(pnl)
                            + " target=" + std::to_string(cfg_.wave1Target) + ") — fermeture 50%");
                        closeWavePartial(0);
                    }
                } else if (ws == 1 && cyclePnl >= cfg_.wave2Target) {
                    if (waveState_.compare_exchange_strong(ws, 2)) {
                        log(">>> WAVE2 cyclePnL=" + std::to_string(cyclePnl)
                            + " (réalisé=" + std::to_string(cycleRealizedPnL_)
                            + " flottant=" + std::to_string(pnl)
                            + " target=" + std::to_string(cfg_.wave2Target) + ") — fermeture 30%");
                        closeWavePartial(1);
                    }
                }
            }

            // ── Test target basket global (réalisé + flottant) ────
            int totalRecov = recovery_->sellCount() + recovery_->buyCount();
            if (basketMon_->update(cyclePnl, cached, totalRecov)) {
                if (!basketClosing_.exchange(true)) {
                    closingStart_ = Clock::now();
                    log(">>> BASKET TARGET ATTEINT cyclePnL=" + std::to_string(cyclePnl)
                        + " (réalisé=" + std::to_string(cycleRealizedPnL_)
                        + " flottant=" + std::to_string(pnl) + ") [tick]");
                    closeBasketAtomic();
                    basketClosing_.store(false);
                }
                return;
            }
        }
    }

    // 4. Gate HTTP : polling ordres + gestion grille 1×/seconde max
    //    (limite API broker — ne concerne PAS les déclenchements recovery)
    //    EXCEPTION : si triggerFired_ est levé (trigger géométrique vient
    //    de se produire), on passe la gate immédiatement pour replacer
    //    les nouveaux ordres SS/BS sans délai.
    auto now = Clock::now();
    using Sec = std::chrono::seconds;
    bool immediatePass = triggerFired_.exchange(false);
    if (!immediatePass &&
        std::chrono::duration_cast<Sec>(now - lastAction_).count() == 0) return;
    lastAction_ = now;

    // 5. Récupérer ordres frais (HTTP polling, 1×/seconde max)
    auto orders = fetchOrders();
    { std::lock_guard lk(ordersMu_); orders_ = orders; }

    // 6. Init ou restore
    if (state_.load() == EAState::Init) {
        initOrRestore();
    }

    // 7. Sync fallback — vérifie que les tickets internes correspondent
    //    à des ordres réels côté broker, corrige silencieusement si décalage.
    syncTriggersFallback(orders);

    // 7b. Mise à jour des ordres Verrouillage Profit (V-BS / V-SS)
    //     Recalcule avgEntry + ratchet si la composition du panier a changé.
    //     Place/modifie les ordres stop trailing V sur le broker.
    updateProfitLockOrders(orders, tick);

    // 8. Analyse intelligence profonde (1×/seconde — données fraîches)
    {
        AccountSnapshot acc; { std::lock_guard lk(accMu_); acc = account_; }
        double pnl    = basketProfit(orders);
        double target = effectiveTarget(orders);
        double atrPts = grid_->atrPoints();
        int    ssR    = recovery_->sellCount();
        int    bsR    = recovery_->buyCount();

        auto analysis = intel_->analyze(orders, tick, acc, pnl, target,
                                         atrPts, ssR, bsR);
        { std::lock_guard lk(intelMu_); lastIntel_ = analysis; }

        if (analysis.needsHedge) {
            log("[INTEL] ⚠ HEDGE DELTA: delta=" + std::to_string(analysis.netDelta)
                + " lots | pression=" + std::to_string(analysis.hedgePressure * 100.0)
                + "% | régime=" + IntelligenceEngine::regimeName(analysis.regime));
        }
        if (analysis.riskScore >= 70.0) {
            log("[INTEL] ⚠ RISQUE " + std::to_string((int)analysis.riskScore)
                + "/100 | DD=" + std::to_string(analysis.drawdownPct) + "%"
                + (analysis.marginAlert ? " | MARGE CRITIQUE" : ""));
        }
    }

    // 9. Placement des stops géométriques (utilise gridOrigin_ + lastIntel_)
    trailGrid();
}

// ────────────────────────────────────────────────────────────
//  Init / Restauration
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::initOrRestore() {
    // ── Retry détection broker si échec initial (ex: weekend) ─
    // Quand le marché se rouvre, le premier tick passe ici.
    // Si la spec n'a pas été détectée au démarrage (broker offline),
    // on réessaie maintenant avec une connexion disponible.
    if (!symbolSpec_.detected) {
        log("[BrokerSpec] Retry détection au premier tick de marché...");
        SymbolSpec spec = bridge_->requestSymbolSpec(cfg_.symbol, 5000);
        if (spec.detected) {
            bool manualOverride = (cfg_.digits > 0) && (cfg_.digits != spec.digits);
            if (manualOverride) {
                symbolSpec_          = spec;
                cfg_.tickValue       = spec.tickValue;
                cfg_.volumeMin       = spec.volumeMin;
                cfg_.volumeMax       = spec.volumeMax;
                cfg_.volumeStep      = spec.volumeStep;
                cfg_.stopLevelPoints = spec.stopLevelPoints;
                bridge_->updateConfig(cfg_);
                log("[BrokerSpec] Retry OK — surcharge manuelle digits="
                    + std::to_string(cfg_.digits));
            } else {
                applySymbolSpec(spec);
            }
        } else {
            log("[BrokerSpec] Retry échoué — config fichier conservée");
        }
    }

    auto orders = fetchOrders();
    bool hasMkt = std::any_of(orders.begin(), orders.end(), [](const OrderInfo& o){
        return o.type == OrderType::Buy || o.type == OrderType::Sell; });
    bool hasStop= (ssTicket_.load() > 0 || bsTicket_.load() > 0);

    if (hasMkt || hasStop) {
        restoreState(orders);
    } else {
        // Établir l'ancre géométrique sur le prix mid courant
        Tick t; { std::lock_guard lk(tickMu_); t = lastTick_; }
        gridOrigin_ = (t.bid + t.ask) * 0.5;
        state_.store(EAState::GridActive);
        log("Grille initialisée — origin=" + std::to_string(gridOrigin_));
    }

    // ─── Réchauffement Visual AI (cold-start fix) ─────────────
    // Pré-charge 30 bougies synthétiques dans M5/M15/M30 pour que
    // l'ATR, RSI, MACD et DeepSeek soient opérationnels immédiatement
    // sans attendre 13+ heures de barres réelles.
    if (marketIntel_) {
        Tick t0; { std::lock_guard lk(tickMu_); t0 = lastTick_; }
        double mid = (t0.bid + t0.ask) * 0.5;
        if (mid > 0.0) {
            // ATR estimé = gridGap/2 pips × point ($12 pour gridGap=2400, point=0.01)
            double atrEst = (double)(grid_->effectiveGridGap() / 2) * cfg_.point;
            if (atrEst < 0.5) atrEst = 5.0; // garde minimum ($5 ≈ 500pts)
            marketIntel_->warmUp(mid, atrEst, 30);
            log("[WarmUp] Visual AI réchauffé — prix=" + std::to_string(mid)
                + " ATRest=" + std::to_string(atrEst));
        }
    }
}

void TOP4EA_Engine::restoreState(const std::vector<OrderInfo>& orders) {
    int ssT = -1, bsT = -1;
    double ssLvl = 0, bsLvl = 0;
    int sellCnt = 0, buyCnt = 0;
    double sumPrice = 0.0;
    int    mktCount = 0;

    for (const auto& o : orders) {
        if (o.type == OrderType::SellStop) { ssT = o.ticket; ssLvl = o.openPrice; }
        if (o.type == OrderType::BuyStop)  { bsT = o.ticket; bsLvl = o.openPrice; }
        if (o.type == OrderType::Sell) { ++sellCnt; sumPrice += o.openPrice; ++mktCount; }
        if (o.type == OrderType::Buy)  { ++buyCnt;  sumPrice += o.openPrice; ++mktCount; }
    }
    ssTicket_.store(ssT);  ssLevel_.store(ssLvl);
    bsTicket_.store(bsT);  bsLevel_.store(bsLvl);
    recovery_->restoreFromOrders(sellCnt, buyCnt);

    // Restaurer l'ancre géométrique depuis le prix moyen des ordres ouverts
    // Si aucun ordre : utiliser le prix mid courant
    if (mktCount > 0) {
        gridOrigin_ = sumPrice / mktCount;
    } else {
        Tick t; { std::lock_guard lk(tickMu_); t = lastTick_; }
        gridOrigin_ = (t.bid + t.ask) * 0.5;
    }

    state_.store(EAState::GridActive);
    log("État restauré SS=" + std::to_string(ssLvl)
        + " BS=" + std::to_string(bsLvl)
        + " sellR=" + std::to_string(sellCnt)
        + " buyR=" + std::to_string(buyCnt)
        + " origin=" + std::to_string(gridOrigin_));
}

// ────────────────────────────────────────────────────────────
//  Détection géométrique pure — appelée à CHAQUE tick
//  Principe institutionnel : le prix franchit un niveau de grille
//  défini mathématiquement → recovery déclenché immédiatement.
//  Aucune dépendance au polling HTTP ni à une gate temporelle.
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::detectGeometricTriggers(const Tick& tick) {
    int ss = ssTicket_.load();
    int bs = bsTicket_.load();
    int gap = grid_->effectiveGridGap();

    // ── Lecture décision Visual AI (thread-safe via getter) ───
    VisualDecision vdNow = marketIntel_->getLastDecision();

    // ── [VL] Verrouillage Profit Trailing — priorité maximale ──
    // Le ratchet est appliqué à chaque tick avant la logique recovery.
    // Si un niveau de verrouillage est franchi → fermeture immédiate côté marché.
    if (cfg_.vlEnabled) {
        // Ratchet tick-level : fait monter V-SS et descendre V-BS avec le prix
        if (vlMgr_.applyTick(tick)) {
            // Niveau ratchet a bougé : l'engine mettra à jour les ordres broker
            // lors du prochain cycle fetchOrders (pas d'action HTTP ici)
        }
        int vlTrig = vlMgr_.checkTrigger(tick);
        if (vlTrig == 1) {
            log("[VL] V-SS franchi @ bid=" + std::to_string(tick.bid)
                + " ≤ V-SS=" + std::to_string(vlMgr_.ssLevel())
                + " → fermeture côté BUY (profit verrouillé)");
            vlMgr_.resetSS();
            triggerFired_.store(true);
            closeSideAtVL(0);   // ferme toutes les positions Buy avec profit
            return;
        }
        if (vlTrig == 2) {
            log("[VL] V-BS franchi @ ask=" + std::to_string(tick.ask)
                + " ≥ V-BS=" + std::to_string(vlMgr_.bsLevel())
                + " → fermeture côté SELL (profit verrouillé)");
            vlMgr_.resetBS();
            triggerFired_.store(true);
            closeSideAtVL(1);   // ferme toutes les positions Sell avec profit
            return;
        }
    }

    // ── SellStop : bid franchit ssLevel_ à la baisse ─────────
    // Le broker a déjà exécuté l'ordre → on met à jour l'état interne
    // immédiatement sans attendre le polling HTTP.
    if (ss > 0 && ssLevel_.load() > 0.0 && tick.bid <= ssLevel_.load()) {
        log("[GEO] SS déclenché @ bid=" + std::to_string(tick.bid)
            + " ≤ level=" + std::to_string(ssLevel_.load())
            + " [géométrie pure]");
        double triggeredAt = ssLevel_.load();
        ssTicket_.store(-1);  // empêche tout re-déclenchement sur le tick suivant
        ssLevel_.store(0.0);  // sera recalculé géométriquement par trailGrid()

        // ── BUG FIX MARTINGALE ────────────────────────────────
        // Le broker a TOUJOURS exécuté ce SS (position Sell ouverte).
        // On appelle TOUJOURS onSellStopTriggered pour enregistrer le niveau
        // dans le RecoveryEngine et incrémenter le compteur Martingale.
        // Si CascadeGuard bloque l'incrément automatique → on force l'incrément
        // car la position est réelle et le prochain lot DOIT progresser.
        // Le Visual AI décide seulement si on place un NOUVEAU SS recovery.
        auto res = recovery_->onSellStopTriggered(triggeredAt, gap);
        if (res.action == TriggerResult::Action::Blocked) {
            // CascadeGuard a bloqué : forcer quand même l'incrément
            // (position Sell réelle ouverte → compte doit refléter la réalité)
            recovery_->forceIncrementSell();
            log("[Recovery] SS broker exécuté — Martingale forcée malgré CascadeGuard"
                " count=" + std::to_string(recovery_->sellCount()));
        }

        if (!vdNow.allowRecovery || !vdNow.allowSSRecovery) {
            // Visual AI bloque le nouveau SS recovery (sécurité directionnelle)
            // Le compteur est déjà incrémenté → lot correct au prochain SS
            log("[AUDIT] Nouveau SS recovery BLOQUÉ par Visual AI"
                " score=" + std::to_string((int)vdNow.recoveryScore)
                + " raison=" + vdNow.recoveryReason
                + vdNow.ssBlockReason
                + " | Martingale count=" + std::to_string(recovery_->sellCount()));
        } else {
            if (res.action == TriggerResult::Action::NewRecovery)
                state_.store(EAState::Recovery);
            else if (res.action == TriggerResult::Action::CloseOneSide)
                closeOneSide(0);
        }
        // ── Relance immédiate du cycle ────────────────────────
        // Force trailGrid() au tick suivant (pas d'attente 1 seconde)
        triggerFired_.store(true);
    }

    // ── BuyStop : ask franchit bsLevel_ à la hausse ──────────
    if (bs > 0 && bsLevel_.load() > 0.0 && tick.ask >= bsLevel_.load()) {
        log("[GEO] BS déclenché @ ask=" + std::to_string(tick.ask)
            + " ≥ level=" + std::to_string(bsLevel_.load())
            + " [géométrie pure]");
        double triggeredAt = bsLevel_.load();
        bsTicket_.store(-1);
        bsLevel_.store(0.0);

        // ── BUG FIX MARTINGALE ────────────────────────────────
        // Même logique : le broker a toujours exécuté ce BS.
        // Incrément forcé si CascadeGuard bloque pour préserver la Martingale.
        auto res = recovery_->onBuyStopTriggered(triggeredAt, gap);
        if (res.action == TriggerResult::Action::Blocked) {
            recovery_->forceIncrementBuy();
            log("[Recovery] BS broker exécuté — Martingale forcée malgré CascadeGuard"
                " count=" + std::to_string(recovery_->buyCount()));
        }

        if (!vdNow.allowRecovery || !vdNow.allowBSRecovery) {
            log("[AUDIT] Nouveau BS recovery BLOQUÉ par Visual AI"
                " score=" + std::to_string((int)vdNow.recoveryScore)
                + " raison=" + vdNow.recoveryReason
                + vdNow.bsBlockReason
                + " | Martingale count=" + std::to_string(recovery_->buyCount()));
        } else {
            if (res.action == TriggerResult::Action::NewRecovery)
                state_.store(EAState::Recovery);
            else if (res.action == TriggerResult::Action::CloseOneSide)
                closeOneSide(1);
        }
        // ── Relance immédiate du cycle ────────────────────────
        triggerFired_.store(true);
    }
}

// ────────────────────────────────────────────────────────────
//  Sync fallback — détecte les ordres disparus sans crossing prix
//  (ex: suppression manuelle, expiration) — ne double-fire jamais
//  avec detectGeometricTriggers car ssTicket_ = -1 si déjà traité
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::syncTriggersFallback(const std::vector<OrderInfo>& orders) {
    int ss  = ssTicket_.load();
    int bs  = bsTicket_.load();
    auto now = Clock::now();

    // SS disparu sans crossing géométrique détecté (ex: suppression manuelle)
    if (ss > 0 && !ticketStillPending(ss, orders)) {
        // ── Fenêtre de protection anti-doublon ────────────────
        // Si le SS a été créé il y a moins de kOrderProtectWindowSec secondes,
        // l'API broker peut simplement ne pas encore refléter l'ordre
        // (HTTP 429 / latence réseau). On ignore cette disparition temporaire
        // pour éviter de créer un doublon.
        using Sec = std::chrono::seconds;
        auto sinceCreate = std::chrono::duration_cast<Sec>(
                               now - ssCreatedAt_).count();
        if (sinceCreate < kOrderProtectWindowSec) {
            log("[SYNC-Guard] SS ticket=" + std::to_string(ss)
                + " absent API mais protégé (" + std::to_string(sinceCreate)
                + "s < " + std::to_string(kOrderProtectWindowSec) + "s) — ignoré");
        } else {
            log("[SYNC] SS ticket=" + std::to_string(ss)
                + " disparu sans crossing prix — sync fallback");
            ssTicket_.store(-1);
            ssLevel_.store(0.0);
        }
    }

    // BS disparu sans crossing géométrique détecté
    if (bs > 0 && !ticketStillPending(bs, orders)) {
        using Sec = std::chrono::seconds;
        auto sinceCreate = std::chrono::duration_cast<Sec>(
                               now - bsCreatedAt_).count();
        if (sinceCreate < kOrderProtectWindowSec) {
            log("[SYNC-Guard] BS ticket=" + std::to_string(bs)
                + " absent API mais protégé (" + std::to_string(sinceCreate)
                + "s < " + std::to_string(kOrderProtectWindowSec) + "s) — ignoré");
        } else {
            log("[SYNC] BS ticket=" + std::to_string(bs)
                + " disparu sans crossing prix — sync fallback");
            bsTicket_.store(-1);
            bsLevel_.store(0.0);
        }
    }
}

// ────────────────────────────────────────────────────────────
//  Trail de grille — 4 axes adaptatifs
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::trailGrid() {
    // ── MaxRecoveryHold : aucun nouvel ordre autorisé ────────────
    // processTick() retourne avant d'appeler trailGrid() en mode hold,
    // mais cette garde protège contre les appels directs (géométrique, fallback).
    if (state_.load() == EAState::MaxRecoveryHold) return;

    int    ssR   = recovery_->sellCount();
    int    bsR   = recovery_->buyCount();
    double ssLot = recovery_->sellLot();
    double bsLot = recovery_->buyLot();

    // ── Lire la décision Visual AI + DeepSeek (orchestrateur) ────
    // LOCK_WAIT  : DeepSeek ou VisualAI a détecté une zone de risque élevé
    //              → on gèle le trailing (ne pas modifier les stops existants)
    // TRAIL_FREEZE: RSI extrême ou désaccord fort DeepSeek → gel protecteur
    // STOP_ORDERS : spread critique / news spike → aucun nouvel ordre
    // NO_TRADE    : session fermée → aucun nouvel ordre
    MarketDecision vdDecision = orchestrator_.currentDecision();
    bool vlDecisionStopsNew   = (vdDecision == MarketDecision::STOP_ORDERS ||
                                 vdDecision == MarketDecision::NO_TRADE);
    bool vlDecisionFreeze     = (vdDecision == MarketDecision::LOCK_WAIT ||
                                 vdDecision == MarketDecision::TRAIL_FREEZE);

    if (vlDecisionStopsNew) {
        log("[DS/VisualAI] trailGrid bloqué — décision="
            + MarketIntelligence::decisionName(vdDecision)
            + " (aucun nouvel ordre stop)");
        return;
    }

    // ── Lire la dernière analyse intelligence ─────────────────
    // Les facteurs de gap et le freeze sont pilotés par l'intelligence
    // qui prend en compte : régime marché, delta, recovery, vélocité P&L
    double ssGapFactor = 1.0;
    double bsGapFactor = 1.0;
    bool   trailFrozen = false;
    {
        std::lock_guard lk(intelMu_);
        if (lastIntel_.regime != IntelligenceEngine::Regime::Unknown) {
            ssGapFactor = lastIntel_.ssGapFactor;
            bsGapFactor = lastIntel_.bsGapFactor;
            trailFrozen = lastIntel_.trailFrozen;
        } else {
            // Fallback statique si intelligence pas encore initialisée
            double af = cfg_.trailAsymmetryFactor;
            if (ssR > bsR)      { ssGapFactor = 1.0+af; bsGapFactor = 1.0-af; }
            else if (bsR > ssR) { bsGapFactor = 1.0+af; ssGapFactor = 1.0-af; }
            // Gel zone TP fallback
            std::vector<OrderInfo> ord; { std::lock_guard lk2(ordersMu_); ord = orders_; }
            double pnl  = basketProfit(ord);
            trailFrozen = (pnl >= cfg_.basketTarget * cfg_.trailFreezeZone && pnl > 0.0);
        }
    }

    // Gel Visual AI / DeepSeek surcharge Intel si plus restrictif
    if (vlDecisionFreeze && !trailFrozen) {
        log("[DS/VisualAI] Trail gelé par décision="
            + MarketIntelligence::decisionName(vdDecision)
            + " (LOCK_WAIT ou TRAIL_FREEZE — stops non modifiés)");
        trailFrozen = true;
    }

    // ── Trailing session-based + phases de profit ─────────────
    // Spec : 1 pip = 0.01 prix (XAUUSDm digits=2) = 1 point
    // Lot 0.10 → $0.10/pip → 10 pips = $1.00
    //
    //  Phase 0  (P&L < +$8)  : stops initiaux 800 pips — pas de trailing actif
    //  Phase 1  ($8 – $15)   : trail session-based (TOKYO=450p LONDON=800p NY=1050p)
    //  Phase 2  ($15 – $25)  : serrer à 600 pips
    //  Phase 3  (P&L ≥ $25)  : serrer à 400 pips
    //
    // factor = desired_pips / halfGap  (halfGap = gridGap/2)
    // Le trail freeze Intel reste actif pour protéger la zone TP finale.
    if (!trailFrozen) {
        std::vector<OrderInfo> ordCache;
        { std::lock_guard lk(ordersMu_); ordCache = orders_; }
        double pnl      = basketProfit(ordCache);
        int    halfGap  = grid_->effectiveGridGap() / 2;
        if (halfGap < 1) halfGap = 1;

        static constexpr double TRAIL_ACTIVATE_PNL = 8.0;   // +$8  = +80 pips × 0.10
        static constexpr double TRAIL_TIGHT1_PNL   = 15.0;  // +$15 = +150 pips → 600p
        static constexpr double TRAIL_TIGHT2_PNL   = 25.0;  // +$25 = +250 pips → 400p

        if (pnl >= TRAIL_TIGHT2_PNL) {
            // Phase 3 — ultra-serré 400 pips
            double f = 400.0 / halfGap;
            ssGapFactor = f;
            bsGapFactor = f;
        } else if (pnl >= TRAIL_TIGHT1_PNL) {
            // Phase 2 — serré 600 pips
            double f = 600.0 / halfGap;
            ssGapFactor = f;
            bsGapFactor = f;
        } else if (pnl >= TRAIL_ACTIVATE_PNL) {
            // Phase 1 — session-based
            TradingSession sess = MarketIntelligence::detectSession();
            double pipDist = 800.0;  // défaut LONDON/CLOSED
            switch (sess) {
                case TradingSession::TOKYO:      pipDist = 450.0;  break;
                case TradingSession::LONDON:     pipDist = 800.0;  break;
                case TradingSession::NEW_YORK:   pipDist = 1050.0; break;
                case TradingSession::OVERLAP_LN: pipDist = 1050.0; break; // overlap London-NY = forte volatilité → même distance que NY
                default:                         pipDist = 800.0;  break;
            }
            double f = pipDist / halfGap;
            ssGapFactor = f;
            bsGapFactor = f;
        }
        // Phase 0 : facteurs Intel conservés sans modification (distance initiale)
    }

    // Log transition freeze
    static bool wasFrozen = false;
    if (trailFrozen && !wasFrozen) {
        log("[INTEL/Trail] ❄ FIGÉ — zone TP atteinte, stops conservés");
        wasFrozen = true;
    } else if (!trailFrozen && wasFrozen) {
        log("[INTEL/Trail] ✓ Dégel — trail adaptatif réactivé");
        wasFrozen = false;
    }

    std::string ssComment = (ssR > 0) ? "SS_R" + std::to_string(ssR) : "SS_GRID";
    std::string bsComment = (bsR > 0) ? "BS_R" + std::to_string(bsR) : "BS_GRID";

    ensureSellStop(ssLot, ssComment, ssR, ssGapFactor, trailFrozen);
    ensureBuyStop (bsLot, bsComment, bsR, bsGapFactor, trailFrozen);
}

void TOP4EA_Engine::ensureSellStop(double lot, const std::string& comment,
                                   int recovDepth, double gapFactor,
                                   bool trailFrozen) {
    int ss = ssTicket_.load();

    // ── BUG FIX ANTI-DUPLICATION (MetaApi HTTPS) ─────────────
    // Si ssTicket_=-1 (ticket inconnu : POST timeout silencieux, reconnexion…)
    // mais qu'un SellStop non-BE existe dans le cache broker → restaurer le ticket
    // pour éviter de créer un doublon sur le broker.
    if (ss <= 0) {
        std::vector<OrderInfo> ordCache;
        { std::lock_guard lk2(ordersMu_); ordCache = orders_; }
        for (const auto& o : ordCache) {
            if (o.type == OrderType::SellStop) {
                ssTicket_.store(o.ticket);
                ssLevel_.store(o.openPrice);
                ssCreatedAt_ = Clock::now();   // réinitialiser le garde
                log("[Guard-SS] Anti-doublon : SS existant restauré ticket="
                    + std::to_string(o.ticket)
                    + " @ " + std::to_string(o.openPrice));
                ss = o.ticket;   // recharger pour la suite
                break;
            }
        }
    }

    // Ancrage géométrique pur : passe gridOrigin_ comme référence fixe
    // Le niveau est toujours à origin - (depth+1) × GridGap × factor
    auto pl = grid_->computeSellStop(ss, ssLevel_.load(), gridOrigin_, recovDepth, gapFactor);

    if (pl.needsCreate) {
        double curBid  = grid_->bid();
        double minDist = grid_->minDistPts();  // seuil broker minimum (≈102 pts)

        // ── Garde distance broker ───────────────────────────────
        // Le SS doit respecter le STOPLEVEL minimum du broker.
        // Avec ancrage flottant (bid - step), la distance est toujours
        // ≈1 GridGap >> minDist sauf anomalie de prix.
        if ((curBid - pl.targetPrice) < minDist) {
            log("[Guard-SS] trop proche bid " + std::to_string(curBid)
                + " | target=" + std::to_string(pl.targetPrice)
                + " | dist=" + std::to_string((curBid - pl.targetPrice) / cfg_.point)
                + "pts < " + std::to_string(minDist / cfg_.point) + "pts min — rejeté");
            return;
        }

        // ── CascadeGuard : seul critère institutionnel de placement recovery ──
        // Ne s'applique qu'aux recovery stops (recovDepth > 0).
        // Les SS_GRID initiaux (recovDepth = 0) sont toujours autorisés.
        if (recovDepth > 0) {
            auto chk = recovery_->liveCheck((double)grid_->effectiveGridGap());
            if (!chk.priceDispatchOk) {
                log("[Guard-SS-CG] CascadeGuard bloque création SS recovery"
                    " | dépl=" + std::to_string((int)chk.priceDisplacement)
                    + "/" + std::to_string((int)chk.requiredDisplacement)
                    + "pts — nouveau SS refusé");
                return;
            }
        }

        log("SS absent → création @ " + std::to_string(pl.targetPrice)
            + " [origin=" + std::to_string(gridOrigin_)
            + " recov=" + std::to_string(recovDepth)
            + " gap×" + std::to_string(gapFactor)
            + " dist=" + std::to_string((curBid - pl.targetPrice) / cfg_.point) + "pts]");
        TradeCmd cmd = makeSellStop(lot, pl.targetPrice, comment);
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            ssTicket_.store(res.ticket);
            ssLevel_.store(pl.targetPrice);
            ssCreatedAt_ = Clock::now();   // horodatage anti-doublon
            log("SS créé ticket=" + std::to_string(res.ticket)
                + " @ " + std::to_string(ssLevel_.load()));
            // Enregistrement sous-agent L(sellCount)
            orchestrator_.armSlot(
                recovery_->sellCount(),
                res.ticket, pl.targetPrice, lot,
                bsTicket_.load(), bsLevel_.load(), 0.0);
        }
    } else if (pl.needsModify && ss > 0 && !trailFrozen) {
        // Ratchet haut : target > ssLevel_ garanti par computeSellStop
        double curBid = grid_->bid();
        if (pl.targetPrice >= curBid) {
            log("[Guard-SS] trail: cible " + std::to_string(pl.targetPrice)
                + " >= bid " + std::to_string(curBid) + " — ignoré");
            return;
        }
        TradeCmd cmd;
        cmd.op     = TradeCmd::Op::ModifyOrder;
        cmd.ticket = ss;
        cmd.price  = pl.targetPrice;
        cmd.sl     = 0.0; cmd.tp = 0.0;
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            ssLevel_.store(pl.targetPrice);
            log("SS ↑ ratchet → " + std::to_string(pl.targetPrice)
                + " [+" + std::to_string(pl.delta / cfg_.point) + "pts]");
        }
    }
}

void TOP4EA_Engine::ensureBuyStop(double lot, const std::string& comment,
                                  int recovDepth, double gapFactor,
                                  bool trailFrozen) {
    int bs = bsTicket_.load();

    // ── BUG FIX ANTI-DUPLICATION (MetaApi HTTPS) ─────────────
    // Si bsTicket_=-1 mais qu'un BuyStop non-BE existe dans le cache broker
    // → restaurer le ticket pour éviter de créer un doublon.
    if (bs <= 0) {
        std::vector<OrderInfo> ordCache;
        { std::lock_guard lk2(ordersMu_); ordCache = orders_; }
        for (const auto& o : ordCache) {
            if (o.type == OrderType::BuyStop) {
                bsTicket_.store(o.ticket);
                bsLevel_.store(o.openPrice);
                bsCreatedAt_ = Clock::now();
                log("[Guard-BS] Anti-doublon : BS existant restauré ticket="
                    + std::to_string(o.ticket)
                    + " @ " + std::to_string(o.openPrice));
                bs = o.ticket;
                break;
            }
        }
    }

    // Ancrage géométrique pur : passe gridOrigin_ comme référence fixe
    // Le niveau est toujours à origin + (depth+1) × GridGap × factor
    auto pl = grid_->computeBuyStop(bs, bsLevel_.load(), gridOrigin_, recovDepth, gapFactor);

    if (pl.needsCreate) {
        double curAsk  = grid_->ask();
        double minDist = grid_->minDistPts();  // seuil broker minimum (≈102 pts)

        // ── Garde distance broker ───────────────────────────────
        // Le BS doit respecter le STOPLEVEL minimum du broker.
        // Avec ancrage flottant (ask + step), la distance est toujours
        // ≈1 GridGap >> minDist sauf anomalie de prix.
        if ((pl.targetPrice - curAsk) < minDist) {
            log("[Guard-BS] trop proche ask " + std::to_string(curAsk)
                + " | target=" + std::to_string(pl.targetPrice)
                + " | dist=" + std::to_string((pl.targetPrice - curAsk) / cfg_.point)
                + "pts < " + std::to_string(minDist / cfg_.point) + "pts min — rejeté");
            return;
        }

        // ── CascadeGuard : seul critère institutionnel de placement recovery ──
        // Ne s'applique qu'aux recovery stops (recovDepth > 0).
        // Les BS_GRID initiaux (recovDepth = 0) sont toujours autorisés.
        if (recovDepth > 0) {
            auto chk = recovery_->liveCheck((double)grid_->effectiveGridGap());
            if (!chk.priceDispatchOk) {
                log("[Guard-BS-CG] CascadeGuard bloque création BS recovery"
                    " | dépl=" + std::to_string((int)chk.priceDisplacement)
                    + "/" + std::to_string((int)chk.requiredDisplacement)
                    + "pts — nouveau BS refusé");
                return;
            }
        }

        log("BS absent → création @ " + std::to_string(pl.targetPrice)
            + " [origin=" + std::to_string(gridOrigin_)
            + " recov=" + std::to_string(recovDepth)
            + " gap×" + std::to_string(gapFactor)
            + " dist=" + std::to_string((pl.targetPrice - curAsk) / cfg_.point) + "pts]");
        TradeCmd cmd = makeBuyStop(lot, pl.targetPrice, comment);
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            bsTicket_.store(res.ticket);
            bsLevel_.store(pl.targetPrice);
            bsCreatedAt_ = Clock::now();   // horodatage anti-doublon
            log("BS créé ticket=" + std::to_string(res.ticket)
                + " @ " + std::to_string(bsLevel_.load()));
            // Enregistrement sous-agent L(buyCount)
            orchestrator_.armSlot(
                recovery_->buyCount(),
                ssTicket_.load(), ssLevel_.load(), 0.0,
                res.ticket, pl.targetPrice, lot);
        }
    } else if (pl.needsModify && bs > 0 && !trailFrozen) {
        // Ratchet bas : target < bsLevel_ garanti par computeBuyStop
        double curAsk = grid_->ask();
        if (pl.targetPrice <= curAsk) {
            log("[Guard-BS] trail: cible " + std::to_string(pl.targetPrice)
                + " <= ask " + std::to_string(curAsk) + " — ignoré");
            return;
        }
        TradeCmd cmd;
        cmd.op     = TradeCmd::Op::ModifyOrder;
        cmd.ticket = bs;
        cmd.price  = pl.targetPrice;
        cmd.sl     = 0.0; cmd.tp = 0.0;
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            bsLevel_.store(pl.targetPrice);
            log("BS ↓ ratchet → " + std::to_string(pl.targetPrice)
                + " [-" + std::to_string(pl.delta / cfg_.point) + "pts]");
        }
    }
}

// ────────────────────────────────────────────────────────────
//  Sélection du lot — SRH / CPPI / Rolling Kelly
// ────────────────────────────────────────────────────────────
double TOP4EA_Engine::selectLot(int recovCount,
                                double drawdownUSD,
                                double equity,
                                double balance,
                                double gapPoints) const noexcept
{
    // [C] SRH prioritaire : auto-calibré sur le drawdown réel
    if (cfg_.useSRH) {
        return math::srhLot(drawdownUSD, gapPoints,
                            cfg_.tickValue, cfg_.baseLot, cfg_.srhMaxLot);
    }
    // [B] CPPI : protection du plancher d'equity
    if (cfg_.useCPPI) {
        return math::cppiLot(equity, balance,
                             cfg_.cppiFloorPct, cfg_.cppiMultiplier,
                             cfg_.baseLot, gapPoints, cfg_.tickValue);
    }
    // Défaut : Kelly-capped martingale (Rolling Kelly si activé)
    double kf = recovery_->kellyFraction();
    return math::recovLot(cfg_.baseLot, cfg_.lotMultiplier, recovCount, kf);
}

// ────────────────────────────────────────────────────────────
//  Target effectif — VABT ou fixe
// ────────────────────────────────────────────────────────────
double TOP4EA_Engine::effectiveTarget(
    const std::vector<OrderInfo>& orders) const noexcept
{
    if (!cfg_.useVABT) return cfg_.basketTarget;
    double atrPts = grid_->atrPoints();
    double lots   = totalLots(orders);
    return math::vabtTarget(atrPts, lots, cfg_.tickValue,
                            cfg_.vabtCoeff, cfg_.basketTarget);
}

double TOP4EA_Engine::totalLots(
    const std::vector<OrderInfo>& orders) const noexcept
{
    double sum = 0.0;
    for (const auto& o : orders)
        if (o.type == OrderType::Buy || o.type == OrderType::Sell)
            sum += o.lots;
    return sum;
}

// ────────────────────────────────────────────────────────────
//  Fermetures
// ────────────────────────────────────────────────────────────
// ────────────────────────────────────────────────────────────
//  closeWavePartial — TP progressif par vagues
//  wave=0 : Wave1 → ferme les ~50% de lots les plus petits
//  wave=1 : Wave2 → ferme les ~30% du restant (lots moyens)
//  L'ordre de fermeture : les plus petits lots en premier (risque minimal),
//  en conservant les gros lots pour le trailing final.
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::closeWavePartial(int wave) {
    // Snapshot actuel des ordres
    std::vector<OrderInfo> orders;
    { std::lock_guard lk(ordersMu_); orders = orders_; }

    // Séparer positions marché (Buy/Sell) des stops pendants
    std::vector<OrderInfo> mktOrders;
    mktOrders.reserve(orders.size());
    for (const auto& o : orders) {
        if (o.type == OrderType::Buy || o.type == OrderType::Sell)
            mktOrders.push_back(o);
    }

    if (mktOrders.empty()) {
        log("[Wave" + std::to_string(wave+1) + "] Aucune position marché à fermer");
        return;
    }

    // ── Logique de tri — justification explicite (Bug 3 audit) ──────────────
    //
    // STRATÉGIE ACTUELLE : tri par P&L net ascendant (losers en premier)
    //
    // Raisonnement :
    //   L'objectif de Wave1/Wave2 est de sécuriser un profit partiel TOUT EN
    //   réduisant l'exposition globale. Fermer les losers en premier :
    //   (a) réduit le drawdown flottant immédiatement ;
    //   (b) conserve les winners ouverts pour qu'ils continuent vers le target ;
    //   (c) évite de cristalliser des pertes inutilement si la tendance tourne.
    //
    //   Ex : cyclePnL = +$8, positions = [-$3, -$1, +$6, +$12]
    //     Tri P&L asc → ferme [-$3, -$1] → net restant = +$18 (réduction du risque)
    //     Tri lots asc → ferme les petits lots, indépendamment du P&L → non-optimal
    //
    // Risque marge (hedge strategies) :
    //   Dans un panier strictement hedgé (autant de lots Buy que Sell),
    //   fermer uniquement les losers peut déséquilibrer temporairement la couverture
    //   et augmenter la marge requise. Ce cas est atténué par :
    //   • cfg_.basketMaxLoss → emergencyCloseAll() si marge critique
    //   • basketMon_ → surveillance continue du ratio Buy/Sell
    //   Le risque net reste inférieur à la stratégie "lots croissants" qui ignore
    //   entièrement le P&L flottant.
    //
    // ALTERNATIVE (désactivée) : tri par lots croissants (strategy d'origine)
    //   Avantage : préserve la couverture Buy/Sell en fermant proportionnellement.
    //   Inconvénient : peut fermer des winners avant des losers → suboptimal.
    //   Pour activer : remplacer le comparateur par (a.lots < b.lots).
    //
    std::sort(mktOrders.begin(), mktOrders.end(),
              [](const OrderInfo& a, const OrderInfo& b){
                  return (a.profit + a.swap + a.commission) < (b.profit + b.swap + b.commission);
              });

    int total = (int)mktOrders.size();
    int toClose = 0;
    if (wave == 0) {
        // Wave 1 : fermer les 50% les plus petits (arrondi supérieur)
        toClose = (total + 1) / 2;
    } else {
        // Wave 2 : fermer les 30% du restant (au moins 1 si >0)
        toClose = std::max(1, (total * 3) / 10);
    }

    Tick t; { std::lock_guard lk(tickMu_); t = lastTick_; }
    int closed = 0;

    for (int i = 0; i < toClose && i < (int)mktOrders.size(); ++i) {
        const auto& o = mktOrders[i];
        double price = (o.type == OrderType::Buy) ? t.bid : t.ask;
        TradeCmd cmd = makeClose(o, price);
        auto res     = bridge_->sendCommand(cmd, 2000);
        if (res.success) {
            ++closed;
            log("[Wave" + std::to_string(wave+1) + "] Fermé #"
                + std::to_string(o.ticket) + " " + std::to_string(o.lots) + "L"
                + " @ " + std::to_string(price));
        } else {
            log("[Wave" + std::to_string(wave+1) + "] ECHEC fermeture #"
                + std::to_string(o.ticket) + ": " + res.msg);
        }
    }

    // Rafraîchir le cache d'ordres après fermeture partielle
    auto fresh = fetchOrders();
    { std::lock_guard lk(ordersMu_); orders_ = fresh; }

    log("[Wave" + std::to_string(wave+1) + "] Terminé — " + std::to_string(closed)
        + "/" + std::to_string(toClose) + " positions fermées sur " + std::to_string(total));
}

void TOP4EA_Engine::closeBasketAtomic() {
    state_.store(EAState::Closing);
    auto orders = fetchOrders();

    // ── Capturer métriques cycle AVANT fermeture (pour auto-apprentissage) ─
    double cycleGapPoints   = (double)grid_->effectiveGridGap();
    double cycleGapOptimal  = grid_->atrPoints() * cfg_.atrMultiplier;
    int    cycleSsDepth     = recovery_->sellCount();
    int    cycleBsDepth     = recovery_->buyCount();
    int    cycleRecovDepth  = std::max(cycleSsDepth, cycleBsDepth);
    double cyclePnL         = basketProfit(orders);
    double cycleTarget      = effectiveTarget(orders);
    double cycleLotUsed     = totalLots(orders);
    double cycleDrawdownPct = 0.0;
    { AccountSnapshot acc; { std::lock_guard lk(accMu_); acc = account_; }
      if (acc.balance > 1.0 && acc.equity < acc.balance)
          cycleDrawdownPct = (acc.balance - acc.equity) / acc.balance * 100.0;
    }

    // Injection des fonctions de trading dans BasketCloser
    BasketCloser closer(
        cfg_,
        [this](int ticket, double lots, double price, int slip) -> bool {
            TradeCmd cmd = {};
            cmd.op      = TradeCmd::Op::CloseOrder;
            cmd.ticket  = ticket;
            cmd.lots    = lots;
            cmd.price   = price;
            cmd.slippage= slip;
            auto res    = bridge_->sendCommand(cmd, 2000);
            return res.success;
        },
        [this](int buyT, int sellT) -> bool {
            TradeCmd cmd = {};
            cmd.op      = TradeCmd::Op::CloseBy;
            cmd.ticket  = buyT;
            cmd.ticket2 = sellT;
            auto res    = bridge_->sendCommand(cmd, 2000);
            return res.success;
        },
        [this](int ticket) -> bool {
            TradeCmd cmd = {};
            cmd.op     = TradeCmd::Op::DeletePending;
            cmd.ticket = ticket;
            auto res   = bridge_->sendCommand(cmd, 2000);
            return res.success;
        },
        [this]() -> Tick {
            std::lock_guard lk(tickMu_);
            return lastTick_;
        }
    );

    auto res = closer.execute(orders, ssTicket_.load(), bsTicket_.load());
    totalPairs_.fetch_add(res.pairsClosedBy);
    totalSingles_.fetch_add(res.singlesClosedOk);

    // ── Auto-apprentissage : enregistrement cycle fermé ────────
    //   Toujours après le close (P&L réel), avant le reset
    if (intel_) {
        intel_->recordCycleClosed(
            cyclePnL,
            cycleTarget,
            cycleDrawdownPct,
            cycleGapPoints,
            cycleGapOptimal,
            cycleRecovDepth,
            cycleLotUsed
        );
        // Appliquer calibration à la config locale
        intel_->applyCalibration(cfg_);
    }

    // Reset complet — atomique : tout le reset est réalisé sous le verrou mu_
    // pour qu'aucun tick entrant ne puisse observer un état partiellement réinitialisé
    // (ex: ssTicket_=-1 mais ssLevel_ encore valide).
    {
        std::lock_guard lk(mu_);
        ssTicket_.store(-1); bsTicket_.store(-1);
        ssLevel_.store(0.0); bsLevel_.store(0.0);
    }
    recovery_->reset();
    basketMon_->reset();
    vlMgr_.reset();           // effacer tous les niveaux VL du cycle terminé
    waveState_.store(0);      // remettre à zéro le suivi des vagues pour le prochain cycle
    cycleRealizedPnL_ = 0.0;  // remettre à zéro le profit encaissé (VL partiels)

    // ── FIX CRITIQUE : vider le cache d'ordres immédiatement ────
    // Sans ce vidage, le tick suivant lit le cache périmé (7 ordres, P&L=+10$),
    // déclenche basketMon_->update() → true → nouvelle fermeture → boucle infinie.
    // En vidant ici, le prochain tick ne voit plus d'ordres → pas de déclenchement.
    { std::lock_guard lk(ordersMu_); orders_.clear(); }

    // Réétablir l'ancre géométrique sur le nouveau prix mid
    // La prochaine grille part d'un niveau propre, ancré au marché actuel
    {
        Tick t; { std::lock_guard lk(tickMu_); t = lastTick_; }
        gridOrigin_ = (t.bid + t.ask) * 0.5;
        log("=== BASKET FERMÉ en " + std::to_string(res.elapsed.count()) + "ms"
            + " | nouvelle origin=" + std::to_string(gridOrigin_)
            + " | θ-calibration appliquée ===");
    }
    state_.store(EAState::Init);
}

void TOP4EA_Engine::closeOneSide(int sideType) {
    // ── MaxRecovery atteint → MODE HOLD INSTITUTIONNEL ─────────────
    //
    // Vision institutionnelle : on ne sort PAS perdant automatiquement.
    //   1. Annuler les stops pendants SS/BS (plus aucun nouveau niveau ouvert)
    //   2. Réinitialiser les DEUX compteurs (évite la boucle CloseOneSide)
    //   3. CONSERVER toutes les positions marché ouvertes
    //   4. Basculer en état MaxRecoveryHold
    //
    // En état MaxRecoveryHold :
    //   - trailGrid() est bloqué (aucun nouvel ordre)
    //   - basketMon continue de surveiller les positions existantes
    //   - Si le marché revient → basket atteint le target → fermeture en PROFIT
    //   - Si basket_max_loss est atteint → sortie d'urgence (drawdown limité)
    //
    // basketClosedCallback() réinitialise l'état à Init quand c'est terminé.

    if (state_.load() == EAState::MaxRecoveryHold) return; // déjà en hold

    std::string side = (sideType == 0) ? "SELL" : "BUY";
    log("=== MAX RECOVERY côté " + side
        + " — MODE HOLD ACTIVÉ (positions conservées, attente retournement) ===");

    // ── [1] Annuler les ordres stops pendants (plus de nouvelles positions) ─
    int ss = ssTicket_.load();
    int bs = bsTicket_.load();
    if (ss > 0) { bridge_->sendCommand(makeDelete(ss)); ssTicket_.store(-1); }
    if (bs > 0) { bridge_->sendCommand(makeDelete(bs)); bsTicket_.store(-1); }
    ssLevel_.store(0.0); bsLevel_.store(0.0);

    // ── [2] Reset des compteurs recovery (évite boucle CloseOneSide) ──────
    // Les positions marché restent ouvertes — seuls les compteurs sont remis à 0
    // pour que les prochains appels à checkAndIncrement ne retournent plus CloseOneSide.
    recovery_->reset();

    // ── [3] État MaxRecoveryHold ───────────────────────────────────
    // trailGrid() est désactivé dans cet état. basketMon surveille le retournement.
    state_.store(EAState::MaxRecoveryHold);
    log("[MaxRecovery HOLD] Positions ouvertes conservées"
        " | Attente retournement → target=" + std::to_string(cfg_.basketTarget)
        + "$ | Urgence si P&L < " + std::to_string(cfg_.basketMaxLoss) + "$");
}

// ────────────────────────────────────────────────────────────
//  emergencyCloseAll — sortie forcée si basket_max_loss atteint
//  Appelé depuis processTick() quand en MaxRecoveryHold.
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::emergencyCloseAll(double currentPnL) {
    log("=== URGENCE MaxRecoveryHold : P&L=" + std::to_string(currentPnL)
        + "$ < seuil=" + std::to_string(cfg_.basketMaxLoss)
        + "$ — FERMETURE TOTALE au marché ===");

    // Annuler stops résiduels
    int ss = ssTicket_.load();
    int bs = bsTicket_.load();
    if (ss > 0) { bridge_->sendCommand(makeDelete(ss)); ssTicket_.store(-1); }
    if (bs > 0) { bridge_->sendCommand(makeDelete(bs)); bsTicket_.store(-1); }
    ssLevel_.store(0.0); bsLevel_.store(0.0);

    // Fermer toutes les positions au marché
    auto orders = fetchOrders();
    Tick t; { std::lock_guard lk(tickMu_); t = lastTick_; }
    int closed = 0;
    for (const auto& o : orders) {
        if (o.type == OrderType::Buy || o.type == OrderType::Sell) {
            double price = (o.type == OrderType::Buy) ? t.bid : t.ask;
            auto r = bridge_->sendCommand(makeClose(o, price));
            if (r.success) ++closed;
        }
    }
    log("[Urgence] " + std::to_string(closed) + " position(s) fermée(s) @ marché");

    // Reset complet + nouveau cycle
    vlMgr_.reset();
    basketMon_->reset();
    waveState_.store(0);
    cycleRealizedPnL_ = 0.0;
    { std::lock_guard lk(ordersMu_); orders_.clear(); }
    gridOrigin_ = (t.bid + t.ask) * 0.5;
    state_.store(EAState::Init);
    log("=== Nouveau cycle après urgence — origin=" + std::to_string(gridOrigin_)
        + " | SS=0/" + std::to_string(cfg_.maxRecovery)
        + " BS=0/" + std::to_string(cfg_.maxRecovery) + " ===");
}

// ────────────────────────────────────────────────────────────
//  updateProfitLockOrders — place/modifie les ordres V-BS / V-SS
//  Appelé 1×/seconde après fetchOrders() depuis processTick.
//
//  Logique :
//    vlMgr_.recompute(tick, orders) recalcule avgEntry et applique le ratchet.
//    Si needsPlace=true pour un slot :
//      - Si ticket broker existant  → ORDER_MODIFY (déplacer le stop au nouveau niveau)
//      - Sinon                      → créer nouvel ordre pending avec lot=totalLots
//    Le lot = totalLots pour fermer TOUT le côté au déclenchement.
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::updateProfitLockOrders(const std::vector<OrderInfo>& orders,
                                            const Tick& tick) {
    if (!cfg_.vlEnabled) return;

    bool changed = vlMgr_.recompute(tick, orders);
    if (!changed) return;

    // ── V-BS : trailing BuyStop côté SELL ────────────────────
    // Level descend avec l'ask (ratchet DOWN). Ticket sur broker pour visibilité.
    const auto& bsSlot = vlMgr_.bsVL();
    if (bsSlot.activated && bsSlot.level > 0.0 && bsSlot.needsPlace) {
        // Supprimer l'ancien ordre avant de placer le nouveau (évite doublon)
        if (bsSlot.ticket > 0) {
            bridge_->sendCommand(makeDelete(bsSlot.ticket));
        }
        // Valider le niveau : V-BS doit être au-dessus du prix courant (ask)
        if (bsSlot.level > tick.ask) {
            double vlLot = std::max(bsSlot.totalLots, cfg_.baseLot);
            vlLot = std::min(vlLot, cfg_.volumeMax);
            vlLot = std::max(std::round(vlLot / cfg_.volumeStep) * cfg_.volumeStep,
                             cfg_.volumeMin);
            TradeCmd cmd;
            cmd.op      = TradeCmd::Op::MarketOrder;
            cmd.type    = OrderType::BuyStop;
            cmd.lots    = vlLot;
            cmd.price   = bsSlot.level;
            cmd.slippage= cfg_.slippage;
            cmd.magic   = cfg_.magic;
            cmd.symbol  = cfg_.symbol;
            cmd.comment = cfg_.symbol + ":V_BS #" + std::to_string(++orderCounter_);
            auto res = bridge_->sendCommand(cmd);
            if (res.success) {
                vlMgr_.setBSTicket(res.ticket, bsSlot.level);
                log("[VL] V-BS placé/mis à jour @ " + std::to_string(bsSlot.level)
                    + " (avgSell=" + std::to_string(bsSlot.avgEntry)
                    + " L=" + std::to_string(vlLot)
                    + " totalSellLots=" + std::to_string(bsSlot.totalLots) + ")");
            } else {
                vlMgr_.clearBSNeedsPlace();
            }
        } else {
            log("[VL] V-BS ignoré : niveau " + std::to_string(bsSlot.level)
                + " ≤ ask " + std::to_string(tick.ask) + " (pas encore en profit suffisant)");
            vlMgr_.clearBSNeedsPlace();
        }
    }

    // ── V-SS : trailing SellStop côté BUY ────────────────────
    // Level monte avec le bid (ratchet UP). Ticket sur broker pour visibilité.
    const auto& ssSlot = vlMgr_.ssVL();
    if (ssSlot.activated && ssSlot.level > 0.0 && ssSlot.needsPlace) {
        if (ssSlot.ticket > 0) {
            bridge_->sendCommand(makeDelete(ssSlot.ticket));
        }
        // Valider le niveau : V-SS doit être en dessous du prix courant (bid)
        if (ssSlot.level < tick.bid) {
            double vlLot = std::max(ssSlot.totalLots, cfg_.baseLot);
            vlLot = std::min(vlLot, cfg_.volumeMax);
            vlLot = std::max(std::round(vlLot / cfg_.volumeStep) * cfg_.volumeStep,
                             cfg_.volumeMin);
            TradeCmd cmd;
            cmd.op      = TradeCmd::Op::MarketOrder;
            cmd.type    = OrderType::SellStop;
            cmd.lots    = vlLot;
            cmd.price   = ssSlot.level;
            cmd.slippage= cfg_.slippage;
            cmd.magic   = cfg_.magic;
            cmd.symbol  = cfg_.symbol;
            cmd.comment = cfg_.symbol + ":V_SS #" + std::to_string(++orderCounter_);
            auto res = bridge_->sendCommand(cmd);
            if (res.success) {
                vlMgr_.setSSTicket(res.ticket, ssSlot.level);
                log("[VL] V-SS placé/mis à jour @ " + std::to_string(ssSlot.level)
                    + " (avgBuy=" + std::to_string(ssSlot.avgEntry)
                    + " L=" + std::to_string(vlLot)
                    + " totalBuyLots=" + std::to_string(ssSlot.totalLots) + ")");
            } else {
                vlMgr_.clearSSNeedsPlace();
            }
        } else {
            log("[VL] V-SS ignoré : niveau " + std::to_string(ssSlot.level)
                + " ≥ bid " + std::to_string(tick.bid) + " (pas encore en profit suffisant)");
            vlMgr_.clearSSNeedsPlace();
        }
    }
}

// ────────────────────────────────────────────────────────────
//  closeSideAtVL — verrou déclenché → ferme UNIQUEMENT le côté verrouillé
//  side=0 → V-SS déclenché : ferme toutes les positions Buy (profit verrouillé)
//  side=1 → V-BS déclenché : ferme toutes les positions Sell (profit verrouillé)
//
//  PHILOSOPHIE DU SYSTÈME :
//    Scénario typique :
//      1. SS déclenché → Sell s'ouvre, monte en profit +X
//      2. Prix revient → BS déclenché → Buy s'ouvre
//      3. V-BS posé derrière le Sell (profit +X verrouillé)
//      4. V-BS touché → on ferme le Sell, on encaisse +X dans cycleRealizedPnL_
//      5. Le Buy continue normalement, le cycle poursuit
//      6. Quand cycleRealizedPnL_ + basketProfit(restant) >= target → close global
//
//    Le profit réalisé est mémorisé dans cycleRealizedPnL_ pour que le test
//    de target basket en tienne compte.  Le côté restant continue seul.
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::closeSideAtVL(int side) {
    // ── 1. Supprimer UNIQUEMENT le stop de direction opposée au VL ───
    //
    //  Règle de direction (clé de la philosophie verrou) :
    //    • V-SS est un SellStop  → même direction que SS → V-SS et SS partagent
    //      la direction VENTE.  Quand V-SS tire (Buy fermé), on GARDE le SS car
    //      le côté Sell a encore besoin de ses ordres de récupération SellStop.
    //      On SUPPRIME le BS (BuyStop) car les Buy sont fermés, plus d'entrées Buy.
    //
    //    • V-BS est un BuyStop   → même direction que BS → V-BS et BS partagent
    //      la direction ACHAT.  Quand V-BS tire (Sell fermé), on GARDE le BS car
    //      le côté Buy a encore besoin de ses ordres de récupération BuyStop.
    //      On SUPPRIME le SS (SellStop) car les Sell sont fermés, plus d'entrées Sell.
    //
    //  Résumé : conserver le stop HOMONYME du VL, supprimer l'opposé.
    int ss = ssTicket_.load();
    int bs = bsTicket_.load();
    if (side == 0) {
        // V-SS déclenché → Buy fermé → supprimer BS (plus d'entrées Buy), garder SS
        if (bs > 0) { bridge_->sendCommand(makeDelete(bs)); bsTicket_.store(-1); bsLevel_.store(0.0); }
    } else {
        // V-BS déclenché → Sell fermé → supprimer SS (plus d'entrées Sell), garder BS
        if (ss > 0) { bridge_->sendCommand(makeDelete(ss)); ssTicket_.store(-1); ssLevel_.store(0.0); }
    }

    auto orders = fetchOrders();
    OrderType toClose = (side == 0) ? OrderType::Buy : OrderType::Sell;
    Tick t; { std::lock_guard lk(tickMu_); t = lastTick_; }

    // ── 2. Calculer le profit du côté fermé (avant la fermeture) ─
    double sideProfit = 0.0;
    for (const auto& o : orders)
        if (o.type == toClose)
            sideProfit += o.profit + o.swap + o.commission;

    // ── 3. Fermer UNIQUEMENT les positions du côté verrouillé ──
    int closed = 0, total = 0;
    for (const auto& o : orders) {
        if (o.type != toClose) continue;
        ++total;
        double price = (toClose == OrderType::Buy) ? t.bid : t.ask;
        auto res = bridge_->sendCommand(makeClose(o, price), 2000);
        if (res.success) ++closed;
    }

    // ── 4. Encaisser le profit réalisé dans l'accumulateur du cycle ─
    cycleRealizedPnL_ += sideProfit;

    // ── 5. Reset recovery + VL du côté fermé ─────────────────
    if (side == 0) { recovery_->resetBuy();  vlMgr_.resetSS(); }
    else           { recovery_->resetSell(); vlMgr_.resetBS(); }

    // Rafraîchir le cache d'ordres
    auto fresh = fetchOrders();
    { std::lock_guard lk(ordersMu_); orders_ = fresh; }

    log("[VL] " + std::string(side==0?"V-SS":"V-BS")
        + " → côté " + std::string(side==0?"BUY":"SELL")
        + " fermé (" + std::to_string(closed) + "/" + std::to_string(total) + " pos)"
        + " | profit encaissé=" + std::to_string(sideProfit) + "$"
        + " | cycleRealizedPnL=" + std::to_string(cycleRealizedPnL_) + "$"
        + " — cycle continue côté " + std::string(side==0?"SELL":"BUY"));

    // ── 6. Vérifier si le target global est déjà atteint ─────
    //    (réalisé + flottant restant)
    double remPnl = basketProfit(fresh);
    double totalCycle = cycleRealizedPnL_ + remPnl;

    if (fresh.empty() || totalCycle >= cfg_.basketTarget) {
        log("[VL] Target global atteint après VL"
            + std::string(" (réalisé=") + std::to_string(cycleRealizedPnL_)
            + " flottant=" + std::to_string(remPnl)
            + " total=" + std::to_string(totalCycle) + "$)"
            + " → fermeture globale du panier");
        if (!basketClosing_.exchange(true)) {
            closingStart_ = Clock::now();
            closeBasketAtomic();
            basketClosing_.store(false);
        }
    } else {
        // ── 7. Repositionner la grille pour le côté restant ──
        Tick t2; { std::lock_guard lk(tickMu_); t2 = lastTick_; }
        gridOrigin_ = (t2.bid + t2.ask) * 0.5;
        state_.store(EAState::GridActive);
        log("[VL] Grille repositionnée @ " + std::to_string(gridOrigin_)
            + " | target restant=" + std::to_string(cfg_.basketTarget - cycleRealizedPnL_)
            + "$ (target=" + std::to_string(cfg_.basketTarget)
            + " - réalisé=" + std::to_string(cycleRealizedPnL_) + ")");
    }
}

// ────────────────────────────────────────────────────────────
//  Fermeture GLOBALE — stops + positions en parallèle
//  Utilisée par TOP4EA_CloseAll() et fin de session manuelle
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::closeAllOrders() {
    auto t0 = Clock::now();
    state_.store(EAState::Closing);
    log(">>> CLOSE ALL — fermeture globale démarrée");

    auto orders = fetchOrders();
    Tick tick;
    { std::lock_guard lk(tickMu_); tick = lastTick_; }

    // Phase 0 — purge stops pendants en parallèle
    std::vector<std::future<void>> stopFuts;
    for (const auto& o : orders) {
        if (o.type == OrderType::SellStop || o.type == OrderType::BuyStop) {
            int tkt = o.ticket;
            stopFuts.push_back(std::async(std::launch::async,
                [this, tkt]() { bridge_->sendCommand(makeDelete(tkt)); }));
        }
    }
    for (auto& f : stopFuts) f.get();
    ssTicket_.store(-1); bsTicket_.store(-1);
    ssLevel_.store(0.0); bsLevel_.store(0.0);

    // Phase 1 — fermeture positions marché en parallèle
    std::vector<std::future<bool>> closeFuts;
    for (const auto& o : orders) {
        if (o.type != OrderType::Buy && o.type != OrderType::Sell) continue;
        double price = (o.type == OrderType::Buy) ? tick.bid : tick.ask;
        TradeCmd cmd = makeClose(o, price);
        closeFuts.push_back(std::async(std::launch::async,
            [this, cmd]() mutable -> bool {
                auto res = bridge_->sendCommand(cmd, 2000);
                return res.success;
            }));
    }

    int closed = 0;
    for (int k = 0; k < (int)closeFuts.size(); ++k) {
        if (closeFuts[k].get()) ++closed;
    }

    recovery_->reset();
    basketMon_->reset();
    state_.store(EAState::Init);

    auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - t0);
    log(">>> CLOSE ALL terminé en " + std::to_string(elapsed.count())
        + "ms | fermés=" + std::to_string(closed)
        + "/" + std::to_string(closeFuts.size()));
}

// ────────────────────────────────────────────────────────────
//  Objectif journalier
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::resetDailyIfNeeded() {
    // Reset à minuit
    using namespace std::chrono;
    auto now = Clock::now();
    auto elapsed = duration_cast<hours>(now - dailyResetTime_);
    if (elapsed.count() >= 24) {
        std::lock_guard lk(accMu_);
        dailyStartBalance_  = account_.balance;
        dailyResetTime_     = now;
        dailyTargetReached_ = false;
        log("Objectif journalier reset — capital=" + std::to_string(dailyStartBalance_));
    }
}

void TOP4EA_Engine::checkDailyTarget() {
    if (cfg_.dailyTargetPct <= 0.0 || dailyTargetReached_) return;
    double balance, startBal;
    {
        std::lock_guard lk(accMu_);
        balance  = account_.balance;
        startBal = dailyStartBalance_;
    }
    double profitToday = balance - startBal;
    double target      = startBal * cfg_.dailyTargetPct / 100.0;
    if (profitToday >= target) {
        log("Objectif journalier ATTEINT: +" + std::to_string(profitToday)
            + "$ (" + std::to_string(cfg_.dailyTargetPct) + "%)");
        closeBasketAtomic();
        dailyTargetReached_ = true;
        state_.store(EAState::DailyPause);
    }
}

// ────────────────────────────────────────────────────────────
//  Helpers
// ────────────────────────────────────────────────────────────
std::vector<OrderInfo> TOP4EA_Engine::fetchOrders() {
    auto orders = bridge_->requestOrders(2000);
    // Filtrer uniquement les ordres appartenant à cet EA.
    // Double compatibilité :
    //   - Nouveau format : "XAUUSDm:SS_GRID #5"  → contient cfg_.symbol
    //   - Ancien format  : "SS_GRID #5"           → contient "SS_" ou "BS_"
    std::vector<OrderInfo> filtered;
    filtered.reserve(orders.size());
    for (const auto& o : orders) {
        bool ownedByThisEA =
            o.comment.find(cfg_.symbol) != std::string::npos ||
            o.comment.find("SS_")       != std::string::npos ||
            o.comment.find("BS_")       != std::string::npos;
        if (ownedByThisEA)
            filtered.push_back(o);
    }
    return filtered;
}

double TOP4EA_Engine::basketProfit(const std::vector<OrderInfo>& orders) const noexcept {
    double total = 0.0;
    for (const auto& o : orders)
        if (o.type == OrderType::Buy || o.type == OrderType::Sell)
            total += o.profit + o.swap + o.commission;
    return total;
}

int TOP4EA_Engine::countMarketOrders(const std::vector<OrderInfo>& orders) const noexcept {
    int cnt = 0;
    for (const auto& o : orders)
        if (o.type == OrderType::Buy || o.type == OrderType::Sell) ++cnt;
    return cnt;
}

bool TOP4EA_Engine::ticketStillPending(int ticket,
    const std::vector<OrderInfo>& orders) const noexcept
{
    if (ticket <= 0) return false;
    return std::any_of(orders.begin(), orders.end(), [ticket](const OrderInfo& o){
        return o.ticket == ticket &&
               (o.type == OrderType::BuyStop || o.type == OrderType::SellStop); });
}

// ────────────────────────────────────────────────────────────
//  Fabrication TradeCmd
// ────────────────────────────────────────────────────────────
TradeCmd TOP4EA_Engine::makeSellStop(double lot, double price, const std::string& comment) {
    TradeCmd cmd;
    cmd.op      = TradeCmd::Op::MarketOrder;
    cmd.type    = OrderType::SellStop;
    cmd.lots    = lot;
    cmd.price   = price;
    cmd.slippage= cfg_.slippage;
    cmd.magic   = cfg_.magic;
    cmd.symbol  = cfg_.symbol;
    // Préfixe symbole pour le filtre fetchOrders() : "XAUUSDm:SS_GRID #5"
    cmd.comment = cfg_.symbol + ":" + comment + " #" + std::to_string(++orderCounter_);
    return cmd;
}

TradeCmd TOP4EA_Engine::makeBuyStop(double lot, double price, const std::string& comment) {
    TradeCmd cmd;
    cmd.op      = TradeCmd::Op::MarketOrder;
    cmd.type    = OrderType::BuyStop;
    cmd.lots    = lot;
    cmd.price   = price;
    cmd.slippage= cfg_.slippage;
    cmd.magic   = cfg_.magic;
    cmd.symbol  = cfg_.symbol;
    // Préfixe symbole pour le filtre fetchOrders() : "XAUUSDm:BS_GRID #6"
    cmd.comment = cfg_.symbol + ":" + comment + " #" + std::to_string(++orderCounter_);
    return cmd;
}

TradeCmd TOP4EA_Engine::makeClose(const OrderInfo& ord, double price) {
    TradeCmd cmd;
    cmd.op     = TradeCmd::Op::CloseOrder;
    cmd.ticket = ord.ticket;
    cmd.lots   = ord.lots;
    cmd.price  = price;
    cmd.slippage = cfg_.slippage;
    return cmd;
}

TradeCmd TOP4EA_Engine::makeCloseBy(int buyTicket, int sellTicket) {
    TradeCmd cmd;
    cmd.op      = TradeCmd::Op::CloseBy;
    cmd.ticket  = buyTicket;
    cmd.ticket2 = sellTicket;
    return cmd;
}

TradeCmd TOP4EA_Engine::makeDelete(int ticket) {
    TradeCmd cmd;
    cmd.op     = TradeCmd::Op::DeletePending;
    cmd.ticket = ticket;
    return cmd;
}

// ────────────────────────────────────────────────────────────
//  Dashboard
// ────────────────────────────────────────────────────────────
std::string TOP4EA_Engine::dashboard() const {
    Tick tick;    { std::lock_guard lk(tickMu_); tick = lastTick_; }
    AccountSnapshot acc; { std::lock_guard lk(accMu_); acc = account_; }
    std::vector<OrderInfo> orders; { std::lock_guard lk(ordersMu_); orders = orders_; }

    double pnl   = basketProfit(orders);
    int    mkt   = countMarketOrders(orders);
    double freePct = (acc.balance > 0) ? (acc.freeMargin / acc.balance * 100.0) : 0.0;

    auto stateStr = [this]() -> std::string {
        switch(state_.load()) {
            case EAState::Init:             return "INIT";
            case EAState::GridActive:       return "ACTIF";
            case EAState::Recovery:         return "RECOVERY";
            case EAState::Closing:          return "CLOSING";
            case EAState::DailyPause:       return "PAUSE_DAILY";
            case EAState::MaxRecoveryHold:  return "MAX_HOLD [ATTENTE]";
        }
        return "?";
    }();

    BasketZone zone = basketMon_->zone();
    std::string zoneStr = (zone==BasketZone::A?"A":zone==BasketZone::B?"B":"C");

    // Lire les valeurs intelligence pour le trail
    double dashSsGap = 1.0, dashBsGap = 1.0; bool dashFrozen = false;
    {
        std::lock_guard lk(intelMu_);
        if (lastIntel_.regime != IntelligenceEngine::Regime::Unknown) {
            dashSsGap  = lastIntel_.ssGapFactor;
            dashBsGap  = lastIntel_.bsGapFactor;
            dashFrozen = lastIntel_.trailFrozen;
        }
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "\n╔═══════════════════════════════════╗\n";
    oss << "║    TOP4EA  C++20  V3.0 — INTEL    ║\n";
    oss << "╠═══════════════════════════════════╣\n";
    oss << "║  Symbole   : " << cfg_.symbol << "\n";
    oss << "║  État      : " << stateStr << "\n";
    // ── Panneau BrokerSpec ─────────────────────────────────────
    if (symbolSpec_.detected) {
        oss << "║  Broker    : " << cfg_.digits << "d"
            << "  pt=" << std::setprecision(symbolSpec_.digits) << cfg_.point
            << "  tv=" << std::setprecision(2) << cfg_.tickValue << "$/lot/pt"
            << "  cs=" << (int)cfg_.contractSize << "\n";
        oss << "║  Vol[" << cfg_.volumeMin << "/"
            << cfg_.volumeMax << "/" << cfg_.volumeStep << "]"
            << "  StpLvl=" << (int)cfg_.stopLevelPoints << "pts"
            << (symbolSpec_.brokerName.empty() ? "" : "  ["+symbolSpec_.brokerName+"]")
            << "\n";
    } else {
        int showPrec = (cfg_.digits > 0) ? cfg_.digits : 5;
        oss << "║  Broker    : " << (cfg_.digits > 0
                                     ? std::to_string(cfg_.digits) + "d [config manuel]"
                                     : "auto-detect en attente")
            << "  pt=" << std::setprecision(showPrec) << cfg_.point
            << std::setprecision(2) << "\n";
    }
    oss << std::setprecision(2);
    oss << "║  Bid/Ask   : " << tick.bid << " / " << tick.ask << "\n";
    oss << "╠═══════════════════════════════════╣\n";
    double cyclePnlDash = cycleRealizedPnL_ + pnl;
    oss << "║  Basket P&L: " << (pnl>=0?"+":"") << pnl << " $";
    if (cycleRealizedPnL_ > 0.01)
        oss << "  [+réalisé=" << std::setprecision(2) << cycleRealizedPnL_
            << " → cycle=" << (cyclePnlDash>=0?"+":"") << cyclePnlDash << "$]";
    oss << "\n";
    {
        double dynTarget = effectiveTarget(orders);
        oss << "║  Target    : " << dynTarget << " $";
        if (cfg_.useVABT && dynTarget != cfg_.basketTarget)
            oss << "  [VABT base=" << cfg_.basketTarget << "]";
        oss << "\n";
    }
    oss << "║  Vélocité  : " << basketMon_->velocity() << " $/tick\n";
    oss << "╠═══════════════════════════════════╣\n";
    // Affichage du mode trail : fixe ou adaptatif
    if (cfg_.trailFixedDistPts > 0) {
        int pipSize = (cfg_.digits >= 3) ? 10 : 10;   // 1 pip = 10 points
        double pips = (double)cfg_.trailFixedDistPts / pipSize;
        oss << "║  Trail  : " << (dashFrozen ? "❄FIGÉ (zone TP)" : "ACTIF")
            << " [FIXE " << cfg_.trailFixedDistPts << "pts≈" << pips << "pips]\n";
        oss << "║  SS dist: " << cfg_.trailFixedDistPts << "pts"
            << "  BS dist: " << cfg_.trailFixedDistPts << "pts\n";
    } else {
        oss << "║  Trail  : " << (dashFrozen ? "❄FIGÉ (zone TP)" : "ACTIF") << "\n";
        oss << "║  SS gap : ×" << std::setprecision(3) << dashSsGap
            << "  BS gap : ×" << dashBsGap << "\n";
    }
    oss << "╠═══════════════════════════════════╣\n";
    oss << "║  SS Recovery: " << recovery_->sellCount() << " / " << cfg_.maxRecovery << "\n";
    oss << "║  BS Recovery: " << recovery_->buyCount()  << " / " << cfg_.maxRecovery << "\n";
    oss << "║  Ordres mkt : " << mkt << "  [ZONE " << zoneStr << "]\n";
    oss << "║  GridGap   : " << grid_->effectiveGridGap() << " pts"
        << " (ATR=" << std::setprecision(1) << grid_->atrPoints() << ")\n";
    oss << "║  Régime    : ";
    switch(grid_->regime()) {
        case VolRegime::Low:    oss << "LOW VOL\n";  break;
        case VolRegime::Medium: oss << "NORMAL\n";   break;
        case VolRegime::High:   oss << "HIGH VOL\n"; break;
    }
    oss << std::setprecision(2);
    oss << "╠═══════════════════════════════════╣\n";
    oss << "║  Balance   : " << acc.balance << " " << acc.currency << "\n";
    oss << "║  Marge lib : " << freePct << "%\n";
    oss << "║  SS @ " << std::setprecision(5) << ssLevel_.load() << "\n";
    oss << "║  BS @ " << bsLevel_.load() << "\n";
    // ── Verrouillage Profit Trailing (V-BS / V-SS) ────────────
    if (cfg_.vlEnabled) {
        oss << "╠══════ VERROUILLAGE PROFIT (V) ══════╣\n";
        const auto& vBS = vlMgr_.bsVL();
        const auto& vSS = vlMgr_.ssVL();
        if (vBS.activated && vBS.level > 0.0)
            oss << "║  V-BS: " << std::setprecision(5) << vBS.level
                << " (avgSell=" << vBS.avgEntry
                << " L=" << std::setprecision(2) << vBS.totalLots
                << ") ↓ratchet\n";
        else if (vBS.active)
            oss << "║  V-BS: en attente activation (profit insuffisant)\n";
        else
            oss << "║  V-BS: inactif (aucune position Sell)\n";
        if (vSS.activated && vSS.level > 0.0)
            oss << "║  V-SS: " << std::setprecision(5) << vSS.level
                << " (avgBuy =" << vSS.avgEntry
                << " L=" << std::setprecision(2) << vSS.totalLots
                << ") ↑ratchet\n";
        else if (vSS.active)
            oss << "║  V-SS: en attente activation (profit insuffisant)\n";
        else
            oss << "║  V-SS: inactif (aucune position Buy)\n";
    }
    // Cascade guard — statut live basé sur le tick courant
    auto cg = recovery_->liveCheck(grid_->effectiveGridGap());
    oss << std::setprecision(1);
    oss << "╠═══════════════════════════════════╣\n";
    oss << "║  [1]Déplacement: " << (cg.priceDispatchOk?"✓":"✗")
        << " " << cg.priceDisplacement << "/" << cg.requiredDisplacement << "pts\n";
    oss << "║  [2]Spread    : " << (cg.spreadOk?"✓":"✗")
        << " " << cg.currentSpreadPts << "pts (moy=" << cg.avgSpreadPts << ")\n";
    oss << "║  [3]Whipsaw   : " << (cg.noWhipsawOk?"✓ tient":"✗ retour")<< "\n";
    oss << "║  Garde cascade: " << (cg.allowed ? "OUVERT" : "BLOQUÉ") << "\n";
    oss << "╠═══════════════════════════════════╣\n";
    oss << "║  Pairs tot : " << totalPairs_.load() << " | Singles: " << totalSingles_.load() << "\n";
    // ─── Panneau INTELLIGENCE ─────────────────────────────────
    {
        std::lock_guard lk(intelMu_);
        const auto& ia = lastIntel_;
        if (ia.regime != IntelligenceEngine::Regime::Unknown) {
            oss << std::setprecision(2);
            oss << "╠═══════ INTELLIGENCE ENGINE ════════╣\n";
            oss << "║  Régime    : " << IntelligenceEngine::regimeName(ia.regime)
                << "  ρ₁=" << std::setprecision(3) << ia.rho1 << "\n";
            oss << std::setprecision(2);
            oss << "║  Δ Delta   : " << (ia.netDelta>=0?"+":"") << ia.netDelta
                << " lots  (B=" << ia.buyLots << " S=" << ia.sellLots << ")\n";
            oss << "║  Pression  : " << (ia.hedgePressure*100.0) << "% ";
            if (ia.needsHedge) {
                oss << "→ ⚠ HEDGE "
                    << (ia.hedgeType==0?"BUY":"SELL") << " " << ia.hedgeLot << "L";
            } else {
                // Label basé sur le niveau de pression réel (pas seulement hedge on/off)
                double p = ia.hedgePressure;
                if      (p < 0.20) oss << "→ ✓ neutre";
                else if (p < 0.50) oss << "→ ~ modéré";
                else if (p < 0.80) oss << "→ ⚡ élevée";
                else               oss << "→ ⚠ critique";
            }
            oss << "\n";
            oss << "║  Risque    : " << (int)ia.riskScore << "/100"
                << "  DD=" << ia.drawdownPct << "%";
            if (ia.marginAlert)  oss << " ⚠MARGE";
            if (ia.cascadeRisk)  oss << " ⚠CASCADE";
            oss << "\n";
            oss << "║  Trail SS×" << std::setprecision(3) << ia.ssGapFactor
                << " BS×" << ia.bsGapFactor;
            if (ia.trailFrozen) oss << " ❄FIGÉ";
            oss << "\n";
            oss << "║  Vélocité  : " << std::setprecision(3) << ia.pnlVelocity << " $/tick"
                << "  CloseProb=" << std::setprecision(1) << ia.closeProbPct << "%\n";
        }
    }
    // ─── Panneau VISUAL AI + SOUS-AGENTS ─────────────────────
    {
        VisualDecision vd = marketIntel_->analyze();
        oss << "╠═══════ VISUAL AI — MULTI-AGENTS ═══╣\n";
        oss << "║  Session : " << MarketIntelligence::sessionName(vd.session)
            << "   Struct: " << MarketIntelligence::structureName(vd.structure) << "\n";
        oss << "║  Décision: " << MarketIntelligence::decisionName(vd.decision)
            << " [" << std::setprecision(0) << vd.confidence << "%]\n";
        if (!vd.reasoning.empty() && vd.reasoning.size() < 60)
            oss << "║  " << vd.reasoning.substr(0, 58) << "\n";
        oss << std::setprecision(1);
        oss << "║  iniMTA M5  ATR=" << vd.m5.atr
            << "  RSI=" << vd.m5.rsi
            << "  BB" << (vd.m5.bbNarrow()?"[SQZ]":"[OK]") << "\n";
        oss << "║  iniMTA M30 ATR=" << vd.m30.atr
            << "  MACD=" << std::setprecision(4) << vd.m30.macdHist
            << (vd.m30.macdBullish()?" ↑":" ↓") << "\n";
        // ── Couche DeepSeek LLM — avec âge du signal et stats ──
        if (!marketIntel_->hasDeepSeekKey()) {
            oss << "║  DeepSeek  : [clé API manquante — LLM désactivé]\n";
        } else if (vd.m30.barCount < 5) {
            int hNeeded = 5 - vd.m30.barCount;
            oss << "║  DeepSeek  : [démarrage — encore ~"
                << (hNeeded * 30) << "min]\n";
        } else if (vd.dsSignal.signal != DSSignal::UNKNOWN) {
            // ── Âge du signal : critique pour savoir s'il est frais ──
            long sigAge  = marketIntel_->dsSignalAgeSec();
            long callAge = marketIntel_->dsLastCallAge();
            int  calls   = marketIntel_->dsCallCount();
            int  errors  = marketIntel_->dsErrorCount();

            // Indicateur visuel de fraîcheur : vert/orange/rouge selon âge
            std::string freshMark;
            if (!vd.dsSignal.fresh)        freshMark = " ⚠PÉRIMÉ";
            else if (sigAge < 10)          freshMark = " ●LIVE";
            else if (sigAge < 30)          freshMark = " ◐récent";

            oss << std::setprecision(0);
            oss << "║  DeepSeek  : " << vd.dsSignal.signalName()
                << " [" << (int)vd.dsSignal.confidence << "%]"
                << (vd.dsOverride ? " ★OVERRIDE" : "")
                << freshMark << "\n";
            if (!vd.dsSignal.reason.empty()) {
                std::string ds_r = vd.dsSignal.reason.substr(0,50);
                oss << "║   " << ds_r << "\n";
            }
            // ── Ligne de diagnostic : âge signal, appels, erreurs ──
            oss << "║   Signal:" << sigAge << "s | Appel:" << callAge << "s"
                << " | OK=" << (calls - errors) << " ERR=" << errors << "\n";
        } else {
            long callAge = marketIntel_->dsLastCallAge();
            oss << "║  DeepSeek  : en attente réponse API..."
                << " (dernier appel:" << callAge << "s)\n";
        }
        // ── Audit Recovery — directionnel ────────────────────
        oss << std::setprecision(0);
        oss << "║  Recovery  : "
            << (vd.allowRecovery ? "✓ AUTORISÉ" : "✗ BLOQUÉ  ")
            << "  score=" << (int)vd.recoveryScore << "/100\n";
        // Ligne directionnelle BS / SS
        oss << "║   BS:"  << (vd.allowBSRecovery ? "✓" : "✗BLOQUÉ")
            << "  SS:"    << (vd.allowSSRecovery ? "✓" : "✗BLOQUÉ") << "\n";
        if (!vd.bsBlockReason.empty())
            oss << "║   " << vd.bsBlockReason.substr(0, 56) << "\n";
        if (!vd.ssBlockReason.empty())
            oss << "║   " << vd.ssBlockReason.substr(0, 56) << "\n";
        if (!vd.recoveryReason.empty()) {
            std::string rr = vd.recoveryReason.substr(0, 58);
            oss << "║   " << rr << "\n";
        }
        // Slots actifs
        auto snap = orchestrator_.snapshot();
        int activeN = 0;
        for (const auto& s : snap) if (s.isActive()) ++activeN;
        oss << std::setprecision(2);
        oss << "║  Sous-agents actifs : " << activeN << " / " << AgentOrchestrator::MAX_SLOTS << "\n";
        for (const auto& s : snap) {
            if (!s.isActive()) continue;
            oss << "║   " << std::left << std::setw(3) << s.label()
                << "[" << s.stateLabel() << "] Trail:" << s.trailLabel()
                << " SS#" << s.ssTicket << " BS#" << s.bsTicket
                << " P&L:" << (s.openPnl>=0?"+":"") << s.openPnl << "$\n";
        }
    }

    // ─── Panneau DEEP LEARNING / AUTO-CALIBRATION ─────────────
    {
        oss << "╠═══════ DEEP LEARNING — AUTO-CALIB ═╣\n";

        // Lire le statut du serveur AutoML (status.json)
        std::string mlState   = "hors-ligne";
        int         mlCycles  = 0;
        double      mlValLoss = 0.0;
        bool        mlHasModel = std::filesystem::exists("automl/checkpoints/top4ea_model.pt");

        {
            std::ifstream sf("automl/output/status.json");
            if (sf) {
                std::string sc((std::istreambuf_iterator<char>(sf)),
                                std::istreambuf_iterator<char>());
                auto extractStr = [&](const std::string& key) -> std::string {
                    auto p = sc.find("\"" + key + "\"");
                    if (p == std::string::npos) return "";
                    auto c = sc.find(':', p);
                    if (c == std::string::npos) return "";
                    auto q1 = sc.find('"', c);
                    if (q1 == std::string::npos) return "";
                    auto q2 = sc.find('"', q1 + 1);
                    if (q2 == std::string::npos) return "";
                    return sc.substr(q1 + 1, q2 - q1 - 1);
                };
                auto extractDbl = [&](const std::string& key) -> double {
                    auto p = sc.find("\"" + key + "\"");
                    if (p == std::string::npos) return 0.0;
                    auto c = sc.find(':', p);
                    if (c == std::string::npos) return 0.0;
                    try { return std::stod(sc.substr(c + 1)); }
                    catch (...) { return 0.0; }
                };
                auto extractInt = [&](const std::string& key) -> int {
                    auto p = sc.find("\"" + key + "\"");
                    if (p == std::string::npos) return 0;
                    auto c = sc.find(':', p);
                    if (c == std::string::npos) return 0;
                    try { return std::stoi(sc.substr(c + 1)); }
                    catch (...) { return 0; }
                };
                mlState   = extractStr("state");
                mlCycles  = extractInt("n_cycles");
                mlValLoss = extractDbl("best_val_loss");
            }
        }

        // Icône état
        std::string stateIcon;
        if      (mlState == "training")  stateIcon = "[TRAIN]   ";
        else if (mlState == "idle")      stateIcon = "✓ PRET    ";
        else if (mlState == "watching")  stateIcon = "~ OBSERVE ";
        else if (mlState == "error")     stateIcon = "✗ ERREUR  ";
        else                             stateIcon = "- OFFLINE ";

        oss << "║  AutoML    : " << stateIcon
            << "  Cycles=" << mlCycles << "\n";
        oss << "║  Modèle    : " << (mlHasModel ? "✓ chargé" : "○ non entraîné");
        if (mlHasModel && mlValLoss > 0.0)
            oss << "  loss=" << std::fixed << std::setprecision(5) << mlValLoss;
        oss << "\n";

        // Paramètres θ calibrés (depuis lastIntel_)
        {
            std::lock_guard lk(intelMu_);
            const auto& p = lastIntel_.calibratedParams;
            oss << std::fixed << std::setprecision(3);
            oss << "║  θ Adam t=" << p.t_adam
                << "  atr=" << p.atrMultiplier
                << "  kelly=" << p.kellyMaxFraction << "\n";
            oss << "║  θ gap=" << p.gapExpansionFactor
                << "  asym=" << p.trailAsymmetryFactor
                << "  vel=" << p.velocityTrigger << "\n";
        }
    }

    oss << "╚═══════════════════════════════════╝";
    return oss.str();
}

void TOP4EA_Engine::log(const std::string& msg) const {
    auto now   = std::chrono::system_clock::now();
    auto t     = std::chrono::system_clock::to_time_t(now);
    std::cout << "[" << std::put_time(std::localtime(&t), "%H:%M:%S") << "] "
              << msg << "\n";
}

// ============================================================
//  API DLL publique
// ============================================================

bool TOP4EA_Engine::isConnected() const noexcept {
    return bridge_ && bridge_->isConnected();
}

void TOP4EA_Engine::injectTick(const Tick& t) {
    onTick(t);
}

BridgeResponse TOP4EA_Engine::sendTrade(const TradeCmd& cmd) {
    if (!bridge_) {
        BridgeResponse r;
        r.success = false;
        r.error   = -1;
        r.msg     = "Bridge non initialisé";
        return r;
    }
    return bridge_->sendCommand(cmd);
}

double TOP4EA_Engine::getBalance() const noexcept {
    std::lock_guard lk(accMu_);
    return account_.balance;
}

double TOP4EA_Engine::getEquity() const noexcept {
    std::lock_guard lk(accMu_);
    return account_.equity;
}

double TOP4EA_Engine::getFreeMargin() const noexcept {
    std::lock_guard lk(accMu_);
    return account_.freeMargin;
}

double TOP4EA_Engine::getDailyPnL() const noexcept {
    std::lock_guard lk(accMu_);
    return account_.balance - dailyStartBalance_;
}

int TOP4EA_Engine::getOrderCount() const noexcept {
    std::lock_guard lk(ordersMu_);
    return static_cast<int>(orders_.size());
}

} // namespace top4ea
