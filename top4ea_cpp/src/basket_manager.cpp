// ============================================================
//  TOP4EA — basket_manager.cpp
//  Pipeline fermeture basket 4 phases
// ============================================================
#include "../include/basket_manager.hpp"
#include <algorithm>
#include <iostream>
#include <thread>

namespace top4ea {

// ────────────────────────────────────────────────────────────
//  BasketMonitor
// ────────────────────────────────────────────────────────────
BasketMonitor::BasketMonitor(const Config& cfg)
    : cfg_(cfg), vel_(cfg.velocityAlpha) {}

bool BasketMonitor::update(double currentPnL,
                           const std::vector<OrderInfo>& orders) noexcept {
    lastPnL_.store(currentPnL);
    orderCount_.store((int)orders.size());
    vel_.update(currentPnL);

    // Fermeture immédiate si target atteint
    if (currentPnL >= cfg_.basketTarget) {
        close_.store(true);
        return true;
    }

    // Fermeture prédictive (vélocité positive vers target)
    if (cfg_.usePnlVelocity && !orders.empty()) {
        double triggerLevel = cfg_.basketTarget * cfg_.velocityTrigger;
        if (currentPnL >= triggerLevel &&
            vel_.willReachTarget(cfg_.basketTarget, currentPnL, 5)) {
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

    // Trier par P&L ascendant (losers first)
    sortByPnLAsc(singles);

    int interval = refreshInterval(zone);
    int count    = 0;

    for (int k = 0; k < (int)singles.size(); ++k) {
        if (k % interval == 0) {
            currentTick_ = refresh_();
        }
        const auto& o = singles[k];
        double price = (o.type == OrderType::Buy)
                       ? currentTick_.bid
                       : currentTick_.ask;
        bool ok = closeOrder_(o.ticket, o.lots, price, cfg_.slippage);
        if (ok) ++count;
        std::cout << "[Single] #" << k+1 << "/" << singles.size()
                  << " ticket=" << o.ticket
                  << " lot=" << o.lots
                  << " pnl=" << o.profit
                  << " ok=" << ok << "\n";
    }
    return count;
}

int BasketCloser::phase3_safetyNet() {
    std::this_thread::sleep_for(Ms(200));
    int residual = 0;

    for (int attempt = 0; attempt < 3; ++attempt) {
        residual = 0;
        currentTick_ = refresh_();
        // Le caller doit re-scanner les ordres restants
        // Ici on retourne 0 (architecture inject → le moteur supérieur gère)
        break;
    }
    return residual;
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
