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

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  Constructor
// ────────────────────────────────────────────────────────────
TOP4EA_Engine::TOP4EA_Engine(Config cfg)
    : cfg_(std::move(cfg))
{
    bridge_    = std::make_unique<MetaApiBridge>(cfg_);
    grid_      = std::make_unique<GridEngine>(cfg_);
    basketMon_ = std::make_unique<BasketMonitor>(cfg_);
    recovery_  = std::make_unique<RecoveryEngine>(cfg_);
    intel_     = std::make_unique<IntelligenceEngine>(cfg_);

    // Callbacks bridge
    bridge_->onTick   ([this](const Tick& t)            { onTick(t);    });
    bridge_->onOrder  ([this](const OrderInfo& o)       { onOrder(o);   });
    bridge_->onAccount([this](const AccountSnapshot& a) { onAccount(a); });
    bridge_->onError  ([this](int c, const std::string& m){ onError(c,m); });
    bridge_->onConnect([this](bool b)                   { onConnect(b); });
}

TOP4EA_Engine::~TOP4EA_Engine() { stop(); }

// ────────────────────────────────────────────────────────────
//  Lifecycle
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::start() {
    running_.store(true);
    log("TOP4EA C++20 démarré — " + cfg_.symbol
        + " | Target=" + std::to_string(cfg_.basketTarget) + "$"
        + " | DailyPct=" + std::to_string(cfg_.dailyTargetPct) + "%");

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

void TOP4EA_Engine::stop() {
    running_.store(false);
    if (dashThread_.joinable()) dashThread_.request_stop();
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
        log("MetaApi connecté");
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

    // 2. Vérification basket P&L à CHAQUE tick sur ordres cachés (< 1µs)
    //    ─ Avant la gate 1 seconde pour ne rater aucune opportunité ≤50ms ─
    {
        std::vector<OrderInfo> cached;
        { std::lock_guard lk(ordersMu_); cached = orders_; }
        double pnl = basketProfit(cached);
        if (!cached.empty() && basketMon_->update(pnl, cached)) {
            if (!basketClosing_.exchange(true)) {
                log(">>> BASKET TARGET ATTEINT P&L=" + std::to_string(pnl) + " [tick]");
                closeBasketAtomic();
                basketClosing_.store(false);
            }
            return;
        }
    }

    // 3. Anti-doublon par seconde (logique grille + recovery)
    auto now = Clock::now();
    using Sec = std::chrono::seconds;
    if (std::chrono::duration_cast<Sec>(now - lastAction_).count() == 0) return;
    lastAction_ = now;

    // 4. Récupérer ordres frais (HTTP polling, 1×/seconde max)
    auto orders = fetchOrders();
    // Synchroniser le cache
    { std::lock_guard lk(ordersMu_); orders_ = orders; }

    // 5. Init ou restore
    if (state_.load() == EAState::Init) {
        initOrRestore();
    }

    // 6. Cohérence tickets
    checkConsistency(orders);

    // 7. Déclenchements stops
    handleTriggers(orders);

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

        // Signal de hedge : log si exposition fortement déséquilibrée
        if (analysis.needsHedge) {
            log("[INTEL] ⚠ HEDGE DELTA: delta=" + std::to_string(analysis.netDelta)
                + " lots | pression=" + std::to_string(analysis.hedgePressure * 100.0)
                + "% | régime=" + IntelligenceEngine::regimeName(analysis.regime));
        }
        // Alerte risque élevé
        if (analysis.riskScore >= 70.0) {
            log("[INTEL] ⚠ RISQUE " + std::to_string((int)analysis.riskScore)
                + "/100 | DD=" + std::to_string(analysis.drawdownPct) + "%"
                + (analysis.marginAlert ? " | MARGE CRITIQUE" : ""));
        }
    }

    // 9. Trail grille (utilise lastIntel_ pour les facteurs adaptatifs)
    trailGrid();
}

// ────────────────────────────────────────────────────────────
//  Init / Restauration
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::initOrRestore() {
    auto orders = fetchOrders();
    bool hasMkt = std::any_of(orders.begin(), orders.end(), [](const OrderInfo& o){
        return o.type == OrderType::Buy || o.type == OrderType::Sell; });
    bool hasStop= (ssTicket_.load() > 0 || bsTicket_.load() > 0);

    if (hasMkt || hasStop) {
        restoreState(orders);
    } else {
        state_.store(EAState::GridActive);
        log("Grille initialisée — aucun ordre existant");
    }
}

void TOP4EA_Engine::restoreState(const std::vector<OrderInfo>& orders) {
    int ssT = -1, bsT = -1;
    double ssLvl = 0, bsLvl = 0;
    int sellCnt = 0, buyCnt = 0;

    for (const auto& o : orders) {
        if (o.type == OrderType::SellStop) { ssT = o.ticket; ssLvl = o.openPrice; }
        if (o.type == OrderType::BuyStop)  { bsT = o.ticket; bsLvl = o.openPrice; }
        if (o.type == OrderType::Sell)     ++sellCnt;
        if (o.type == OrderType::Buy)      ++buyCnt;
    }
    ssTicket_.store(ssT);  ssLevel_ = ssLvl;
    bsTicket_.store(bsT);  bsLevel_ = bsLvl;
    recovery_->restoreFromOrders(sellCnt, buyCnt);
    state_.store(EAState::GridActive);
    log("État restauré SS=" + std::to_string(ssLvl)
        + " BS=" + std::to_string(bsLvl)
        + " sellR=" + std::to_string(sellCnt)
        + " buyR=" + std::to_string(buyCnt));
}

// ────────────────────────────────────────────────────────────
//  Cohérence tickets
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::checkConsistency(const std::vector<OrderInfo>& orders) {
    static TimePoint lastCheck = Clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(
            Clock::now() - lastCheck).count() < 10) return;
    lastCheck = Clock::now();

    int ss = ssTicket_.load();
    int bs = bsTicket_.load();
    bool ssOk = ticketStillPending(ss, orders);
    bool bsOk = ticketStillPending(bs, orders);
    if (!ssOk && ss > 0) { ssTicket_.store(-1); ssLevel_ = 0; }
    if (!bsOk && bs > 0) { bsTicket_.store(-1); bsLevel_ = 0; }
}

// ────────────────────────────────────────────────────────────
//  Déclenchements stops
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::handleTriggers(const std::vector<OrderInfo>& orders) {
    int ss = ssTicket_.load();
    int bs = bsTicket_.load();

    Tick tick;
    { std::lock_guard lk(tickMu_); tick = lastTick_; }
    int gap = grid_->effectiveGridGap();

    // SS déclenché — prix a franchi ssLevel_ à la baisse
    // On passe ssLevel_ (prix exact du stop) et le gridGap courant
    if (ss > 0 && !ticketStillPending(ss, orders)) {
        auto res = recovery_->onSellStopTriggered(ssLevel_, gap);
        ssTicket_.store(-1);
        if (res.action == TriggerResult::Action::NewRecovery)
            state_.store(EAState::Recovery);
        else if (res.action == TriggerResult::Action::CloseOneSide)
            closeOneSide(0);
        // Si Blocked : pas de recovery, la grille est simplement recréée
    }

    // BS déclenché — prix a franchi bsLevel_ à la hausse
    if (bs > 0 && !ticketStillPending(bs, orders)) {
        auto res = recovery_->onBuyStopTriggered(bsLevel_, gap);
        bsTicket_.store(-1);
        if (res.action == TriggerResult::Action::NewRecovery)
            state_.store(EAState::Recovery);
        else if (res.action == TriggerResult::Action::CloseOneSide)
            closeOneSide(1);
    }
}

// ────────────────────────────────────────────────────────────
//  Trail de grille — 4 axes adaptatifs
// ────────────────────────────────────────────────────────────
void TOP4EA_Engine::trailGrid() {
    int    ssR   = recovery_->sellCount();
    int    bsR   = recovery_->buyCount();
    double ssLot = recovery_->sellLot();
    double bsLot = recovery_->buyLot();

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
    // Paramètres [3] et [4] transmis au grid engine
    auto pl = grid_->computeSellStop(ss, ssLevel_, 0.0, recovDepth, gapFactor);

    if (pl.needsCreate) {
        // Création toujours permise, même en trail gelé (on ne peut pas rester sans stop)
        log("SS absent → création @ " + std::to_string(pl.targetPrice)
            + " [recov=" + std::to_string(recovDepth)
            + " gap×" + std::to_string(gapFactor) + "]");
        TradeCmd cmd = makeSellStop(lot, pl.targetPrice, comment);
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            ssTicket_.store(res.ticket);
            ssLevel_ = pl.targetPrice;
            log("SS créé ticket=" + std::to_string(res.ticket));
        }
    } else if (pl.needsModify && ss > 0 && !trailFrozen) {
        // [2] Si trail figé → pas de modification, seulement création
        TradeCmd cmd;
        cmd.op     = TradeCmd::Op::ModifyOrder;
        cmd.ticket = ss;
        cmd.price  = pl.targetPrice;
        cmd.sl     = 0; cmd.tp = 0;
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            ssLevel_ = pl.targetPrice;
            log("SS modifié → " + std::to_string(pl.targetPrice)
                + " [step=" + std::to_string(pl.delta / cfg_.point) + "pts]");
        }
    }
}

void TOP4EA_Engine::ensureBuyStop(double lot, const std::string& comment,
                                  int recovDepth, double gapFactor,
                                  bool trailFrozen) {
    int bs = bsTicket_.load();
    auto pl = grid_->computeBuyStop(bs, bsLevel_, 0.0, recovDepth, gapFactor);

    if (pl.needsCreate) {
        log("BS absent → création @ " + std::to_string(pl.targetPrice)
            + " [recov=" + std::to_string(recovDepth)
            + " gap×" + std::to_string(gapFactor) + "]");
        TradeCmd cmd = makeBuyStop(lot, pl.targetPrice, comment);
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            bsTicket_.store(res.ticket);
            bsLevel_ = pl.targetPrice;
            log("BS créé ticket=" + std::to_string(res.ticket));
        }
    } else if (pl.needsModify && bs > 0 && !trailFrozen) {
        TradeCmd cmd;
        cmd.op     = TradeCmd::Op::ModifyOrder;
        cmd.ticket = bs;
        cmd.price  = pl.targetPrice;
        cmd.sl     = 0; cmd.tp = 0;
        auto res = bridge_->sendCommand(cmd);
        if (res.success) {
            bsLevel_ = pl.targetPrice;
            log("BS modifié → " + std::to_string(pl.targetPrice)
                + " [step=" + std::to_string(pl.delta / cfg_.point) + "pts]");
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
void TOP4EA_Engine::closeBasketAtomic() {
    state_.store(EAState::Closing);
    auto orders = fetchOrders();

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

    // Reset complet
    ssTicket_.store(-1); bsTicket_.store(-1);
    ssLevel_ = 0; bsLevel_ = 0;
    recovery_->reset();
    basketMon_->reset();
    state_.store(EAState::Init);
    log("=== BASKET FERMÉ en " + std::to_string(res.elapsed.count()) + "ms ===");
}

void TOP4EA_Engine::closeOneSide(int sideType) {
    // sideType 0=fermer BUY (MaxRecovery SELL atteint), 1=fermer SELL
    int ss = ssTicket_.load();
    int bs = bsTicket_.load();
    if (ss > 0) { bridge_->sendCommand(makeDelete(ss)); ssTicket_.store(-1); }
    if (bs > 0) { bridge_->sendCommand(makeDelete(bs)); bsTicket_.store(-1); }

    auto orders = fetchOrders();
    OrderType toClose = (sideType == 0) ? OrderType::Buy : OrderType::Sell;
    for (const auto& o : orders) {
        if (o.type == toClose) {
            Tick t;
            { std::lock_guard lk(tickMu_); t = lastTick_; }
            double price = (toClose == OrderType::Buy) ? t.bid : t.ask;
            bridge_->sendCommand(makeClose(o, price));
        }
    }
    if (sideType == 0) recovery_->resetBuy();
    else               recovery_->resetSell();
    log("Côté " + std::string(sideType==0?"BUY":"SELL") + " fermé (MaxRecovery)");
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
    ssLevel_ = 0; bsLevel_ = 0;

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
    // Filtrer par magic + symbol
    std::vector<OrderInfo> filtered;
    for (const auto& o : orders) {
        if (o.comment.find(cfg_.symbol) != std::string::npos || true)
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
    cmd.comment = comment + " #" + std::to_string(++orderCounter_);
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
    cmd.comment = comment + " #" + std::to_string(++orderCounter_);
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
            case EAState::Init:       return "INIT";
            case EAState::GridActive: return "ACTIF";
            case EAState::Recovery:   return "RECOVERY";
            case EAState::Closing:    return "CLOSING";
            case EAState::DailyPause: return "PAUSE_DAILY";
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
    oss << "║  Bid/Ask   : " << tick.bid << " / " << tick.ask << "\n";
    oss << "╠═══════════════════════════════════╣\n";
    oss << "║  Basket P&L: " << (pnl>=0?"+":"") << pnl << " $\n";
    oss << "║  Target    : " << cfg_.basketTarget << " $\n";
    oss << "║  Vélocité  : " << basketMon_->velocity() << " $/tick\n";
    oss << "╠═══════════════════════════════════╣\n";
    oss << "║  Trail  : " << (dashFrozen ? "❄FIGÉ (zone TP)" : "ACTIF") << "\n";
    oss << "║  SS gap : ×" << std::setprecision(3) << dashSsGap
        << "  BS gap : ×" << dashBsGap << "\n";
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
    oss << "║  SS @ " << std::setprecision(5) << ssLevel_ << "\n";
    oss << "║  BS @ " << bsLevel_ << "\n";
    // Cascade guard
    auto cg = recovery_->lastCascadeCheck();
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
            if (ia.needsHedge)
                oss << "→ ⚠ HEDGE "
                    << (ia.hedgeType==0?"BUY":"SELL") << " " << ia.hedgeLot << "L";
            else
                oss << "→ ✓ neutre";
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
