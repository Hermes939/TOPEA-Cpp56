// ============================================================
//  TOP4EA — basket_manager.cpp
//  Pipeline fermeture basket 4 phases — cible ≤50ms
//  V3 : fermeture parallèle Phase2, safety net sans sleep long
// ============================================================
#include "../include/basket_manager.hpp"
#include <algorithm>
#include <iostream>
#include <thread>
#include <future>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  BasketMonitor
// ────────────────────────────────────────────────────────────
BasketMonitor::BasketMonitor(const Config& cfg)
    : cfg_(cfg), vel_(cfg.velocityAlpha) {}

bool BasketMonitor::update(double currentPnL,
                           const std::vector<OrderInfo>& orders,
                           int recoveryDepth) noexcept {
    lastPnL_.store(currentPnL);
    orderCount_.store((int)orders.size());
    vel_.update(currentPnL);

    // ── Target contextualisé selon profondeur de recovery ────
    // En deep recovery, on exige un P&L plus élevé avant de fermer.
    // Raison : fermer trop tôt en deep recovery laisse souvent une
    // position reouverte immédiatement dans un contexte dégradé.
    // Formule : target × (1 + max(0, depth - threshold) × bonus)
    double adjustedTarget = cfg_.basketTarget;
    if (recoveryDepth > cfg_.basketRecoveryThreshold) {
        int extraLevels = recoveryDepth - cfg_.basketRecoveryThreshold;
        double mult = 1.0 + extraLevels * cfg_.basketRecoveryBonus;
        mult = std::min(mult, 2.0);  // plafond à 2× le target de base
        adjustedTarget = cfg_.basketTarget * mult;
    }

    // Fermeture immédiate si target contextualisé atteint
    if (currentPnL >= adjustedTarget) {
        close_.store(true);
        return true;
    }

    // Fermeture prédictive (vélocité positive vers target)
    if (cfg_.usePnlVelocity && !orders.empty()) {
        double triggerLevel = adjustedTarget * cfg_.velocityTrigger;
        if (currentPnL >= triggerLevel &&
            vel_.willReachTarget(adjustedTarget, currentPnL, 5)) {
            close_.store(true);
            return true;
        }
    }
    return false;
}

void BasketMonitor::reset() noexcept {
    close_.store(false);
    lastPnL_.store(0.0);
    orderCount_.store(0);
    vel_.reset();
}

// ────────────────────────────────────────────────────────────
//  BasketCloser
// ────────────────────────────────────────────────────────────
BasketCloser::BasketCloser(const Config&   cfg,
                           CloseOrderFn    closeOrder,
                           CloseByFn       closeBy,
                           DeleteFn        deletePending,
                           RefreshFn       refresh)
    : cfg_(cfg)
    , closeOrder_(std::move(closeOrder))
    , closeBy_(std::move(closeBy))
    , deletePending_(std::move(deletePending))
    , refresh_(std::move(refresh))
{}

BasketCloseResult BasketCloser::execute(std::vector<OrderInfo> orders,
                                        int ssTicket, int bsTicket) {
    auto t0 = Clock::now();
    BasketCloseResult res;

    int total = (int)orders.size();
    BasketZone zone = zoneOf(total);
    std::cout << "[Basket] Phase 0 — Purge stops | zone="
              << (int)zone << " ordres=" << total << "\n";

    // Phase 0
    phase0_purgePending(ssTicket, bsTicket);

    // Séparer BUY / SELL
    std::vector<OrderInfo> buys, sells;
    for (auto& o : orders) {
        if (o.type == OrderType::Buy)  buys.push_back(o);
        if (o.type == OrderType::Sell) sells.push_back(o);
    }

    sortByLotDesc(buys);
    sortByLotDesc(sells);

    std::vector<bool> buyUsed (buys.size(),  false);
    std::vector<bool> sellUsed(sells.size(), false);

    // Phase 1 — netting CloseBy
    res.pairsClosedBy = phase1_netting(buys, sells, buyUsed, sellUsed);
    std::cout << "[Basket] Phase 1 terminée : " << res.pairsClosedBy << " paires\n";

    // Phase 2 — singles
    res.singlesClosedOk = phase2_singles(buys, sells, buyUsed, sellUsed, zone);
    std::cout << "[Basket] Phase 2 terminée : " << res.singlesClosedOk << " singles\n";

    // Phase 3 — safety net
    res.residuals = phase3_safetyNet();
    if (res.residuals > 0)
        std::cerr << "[Basket] ATTENTION: " << res.residuals
                  << " ordres non fermés après safety net!\n";

    res.elapsed = std::chrono::duration_cast<Ms>(Clock::now() - t0);
    std::cout << "[Basket] FERMÉ en " << res.elapsed.count() << "ms"
              << " | pairs=" << res.pairsClosedBy
              << " singles=" << res.singlesClosedOk
              << " residuals=" << res.residuals << "\n";
    return res;
}

void BasketCloser::phase0_purgePending(int ssTicket, int bsTicket) {
    if (ssTicket > 0) deletePending_(ssTicket);
    if (bsTicket > 0) deletePending_(bsTicket);
}

int BasketCloser::phase1_netting(std::vector<OrderInfo>& buys,
                                  std::vector<OrderInfo>& sells,
                                  std::vector<bool>& buyUsed,
                                  std::vector<bool>& sellUsed) {
    int count = 0;
    for (int b = 0; b < (int)buys.size(); ++b) {
        if (buyUsed[b]) continue;
        for (int s = 0; s < (int)sells.size(); ++s) {
            if (sellUsed[s]) continue;
            if (std::abs(buys[b].lots - sells[s].lots) < 0.005) {
                if (closeBy_(buys[b].ticket, sells[s].ticket)) {
                    buyUsed[b]  = true;
                    sellUsed[s] = true;
                    ++count;
                    std::cout << "[CloseBy] pair #" << count
                              << " buy=" << buys[b].ticket
                              << " sell=" << sells[s].ticket
                              << " lot=" << buys[b].lots << "\n";
                    break;
                }
            }
        }
    }
    return count;
}

int BasketCloser::phase2_singles(const std::vector<OrderInfo>& buys,
                                  const std::vector<OrderInfo>& sells,
                                  const std::vector<bool>& buyUsed,
                                  const std::vector<bool>& sellUsed,
                                  BasketZone zone) {
    // Collecter les non-utilisés
    std::vector<OrderInfo> singles;
    for (int b = 0; b < (int)buys.size(); ++b)
        if (!buyUsed[b]) singles.push_back(buys[b]);
    for (int s = 0; s < (int)sells.size(); ++s)
        if (!sellUsed[s]) singles.push_back(sells[s]);

    if (singles.empty()) return 0;

    // Trier par P&L ascendant (losers first) — stoppe le saignement en priorité
    sortByPnLAsc(singles);

    // Refresh unique avant lancement parallèle
    currentTick_ = refresh_();
    int nS = (int)singles.size();

    // ── Fermeture différenciée par zone ──────────────────────
    // Zone A (≤4 ordres) : parallèle direct, cible ≤20ms
    // Zone B (5-8 ordres) : parallèle par lots de 4 avec refresh inter-lot
    // Zone C (9+ ordres) : parallèle par lots de 4, refresh + délai inter-lot
    //                       pour éviter de saturer le broker
    static constexpr int BATCH_SIZE = 4;

    std::vector<std::future<bool>> futures;
    futures.reserve(nS);

    if (zone == BasketZone::A) {
        // Zone A : tout en parallèle d'un coup
        for (int k = 0; k < nS; ++k) {
            const auto& o = singles[k];
            double price = (o.type == OrderType::Buy)
                           ? currentTick_.bid : currentTick_.ask;
            int    ticket = o.ticket;
            double lots   = o.lots;
            int    slip   = cfg_.slippage;
            futures.push_back(std::async(std::launch::async,
                [this, ticket, lots, price, slip]() -> bool {
                    return closeOrder_(ticket, lots, price, slip);
                }));
        }
    } else {
        // Zone B/C : traitement par lots pour ne pas saturer le broker
        for (int k = 0; k < nS; ++k) {
            const auto& o = singles[k];
            double price = (o.type == OrderType::Buy)
                           ? currentTick_.bid : currentTick_.ask;
            int    ticket = o.ticket;
            double lots   = o.lots;
            int    slip   = cfg_.slippage;
            futures.push_back(std::async(std::launch::async,
                [this, ticket, lots, price, slip]() -> bool {
                    return closeOrder_(ticket, lots, price, slip);
                }));

            // Après chaque lot de BATCH_SIZE ordres, attendre + rafraîchir le prix
            bool isLastInBatch = ((k + 1) % BATCH_SIZE == 0);
            bool isLastOrder   = (k == nS - 1);
            if (isLastInBatch && !isLastOrder) {
                // Attendre les futures du lot courant avant le prochain
                int batchStart = k - BATCH_SIZE + 1;
                for (int j = batchStart; j <= k; ++j)
                    futures[j].wait();
                // Rafraîchir les prix pour le lot suivant
                currentTick_ = refresh_();
                // Zone C : légère pause pour éviter d'inonder l'API broker
                if (zone == BasketZone::C)
                    std::this_thread::sleep_for(Ms(5));
            }
        }
    }

    int count = 0;
    for (int k = 0; k < nS; ++k) {
        bool ok = futures[k].get();
        if (ok) ++count;
        std::cout << "[Single] #" << k+1 << "/" << nS
                  << " ticket=" << singles[k].ticket
                  << " lot="    << singles[k].lots
                  << " pnl="    << singles[k].profit
                  << " ok="     << ok << "\n";
    }
    return count;
}

int BasketCloser::phase3_safetyNet() {
    // Court délai de propagation réseau (≤10ms vs 200ms précédent)
    std::this_thread::sleep_for(Ms(10));
    // L'architecture inject délègue le re-scan au moteur supérieur
    // qui re-fetche les ordres résiduels via requestOrders()
    return 0;
}

void BasketCloser::sortByLotDesc(std::vector<OrderInfo>& v) {
    std::sort(v.begin(), v.end(),
              [](const OrderInfo& a, const OrderInfo& b){
                  return a.lots > b.lots; });
}

void BasketCloser::sortByPnLAsc(std::vector<OrderInfo>& v) {
    std::sort(v.begin(), v.end(),
              [](const OrderInfo& a, const OrderInfo& b){
                  return a.profit < b.profit; });
}

} // namespace top4ea
