#pragma once
// ============================================================
//  TOP4EA — agent_orchestrator.hpp  C++20
//
//  ORCHESTRATEUR MULTI-AGENTS — pool L0 … L49+
//  ─────────────────────────────────────────────────────────
//  Gère le pool de GridSlotAgents :
//    • Création/réutilisation des slots par niveau
//    • Routage des événements ordre vers le bon slot
//    • Diffusion des décisions Visual AI à tous les slots
//    • Agrégation basket P&L + statistiques
//    • Interface unique pour le moteur principal
// ============================================================
#include "grid_slot_agent.hpp"
#include "market_intelligence.hpp"
#include <vector>
#include <mutex>
#include <optional>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace top4ea {

// ============================================================
//  AgentOrchestrator
// ============================================================
class AgentOrchestrator {
public:
    static constexpr int MAX_SLOTS = 50;

    explicit AgentOrchestrator() {
        slots_.reserve(MAX_SLOTS);
    }

    // ─── Armer un nouveau slot Ln ─────────────────────────────
    GridSlotAgent& armSlot(
        int level,
        int ssTicket, double ssPrice, double ssLot,
        int bsTicket, double bsPrice, double bsLot) noexcept
    {
        std::lock_guard lk(mu_);
        // Cherche un slot existant pour ce niveau
        for (auto& s : slots_) {
            if (s.level == level) {
                s.arm(level, ssTicket, ssPrice, ssLot,
                             bsTicket, bsPrice, bsLot);
                return s;
            }
        }
        // Nouveau slot
        GridSlotAgent ag;
        ag.arm(level, ssTicket, ssPrice, ssLot,
                       bsTicket, bsPrice, bsLot);
        slots_.push_back(ag);
        return slots_.back();
    }

    // ─── Routage événement ordre ──────────────────────────────
    void onOrderUpdate(int ticket, double pnl) noexcept {
        std::lock_guard lk(mu_);
        for (auto& s : slots_)
            if (s.ownsTicket(ticket))
                s.updatePnl(ticket, pnl);
    }

    void onOrderClosed(int ticket) noexcept {
        std::lock_guard lk(mu_);
        for (auto& s : slots_)
            if (s.ownsTicket(ticket))
                s.notifyClosed(ticket);
        // Purge des slots IDLE
        cleanupIdle();
    }

    void onOrderTriggered(int ticket) noexcept {
        std::lock_guard lk(mu_);
        for (auto& s : slots_)
            if (s.ownsTicket(ticket))
                s.notifyTriggered(ticket);
    }

    // ─── Mise à jour décision Visual AI ──────────────────────
    void applyDecision(const VisualDecision& vd) noexcept {
        std::lock_guard lk(mu_);
        globalDecision_ = vd.decision;
        // Diffuse à tous les slots actifs
        for (auto& s : slots_)
            if (s.isActive())
                s.applyDecision(vd.decision);
    }

    // ─── TrailMode pour un ticket donné ──────────────────────
    [[nodiscard]] TrailMode trailModeFor(int ticket) const noexcept {
        std::lock_guard lk(mu_);
        for (const auto& s : slots_)
            if (s.ownsTicket(ticket))
                return s.trailMode;
        return TrailMode::NATURAL;
    }

    // ─── Facteur gap trail pour un ticket ────────────────────
    [[nodiscard]] double trailGapFor(int ticket, double base) const noexcept {
        std::lock_guard lk(mu_);
        for (const auto& s : slots_)
            if (s.ownsTicket(ticket))
                return s.trailGapFactor(base);
        return base;
    }

    // ─── Agrégat P&L ─────────────────────────────────────────
    [[nodiscard]] double basketPnl() const noexcept {
        std::lock_guard lk(mu_);
        double total = 0.0;
        for (const auto& s : slots_) total += s.openPnl;
        return total;
    }

    // ─── Stats ────────────────────────────────────────────────
    [[nodiscard]] int activeSlots() const noexcept {
        std::lock_guard lk(mu_);
        return static_cast<int>(
            std::count_if(slots_.begin(), slots_.end(),
                          [](const GridSlotAgent& s){ return s.isActive(); }));
    }

    [[nodiscard]] MarketDecision currentDecision() const noexcept {
        std::lock_guard lk(mu_);
        return globalDecision_;
    }

    // ─── Vue sur les slots (copie snapshot) ──────────────────
    [[nodiscard]] std::vector<GridSlotAgent> snapshot() const {
        std::lock_guard lk(mu_);
        return slots_;
    }

    // ─── Slot par niveau (pointeur brut — protégé par mu_) ───
    // À utiliser UNIQUEMENT sous verrou externe
    [[nodiscard]] GridSlotAgent* findLevel(int level) noexcept {
        for (auto& s : slots_)
            if (s.level == level) return &s;
        return nullptr;
    }

    // ─── Reset complet ───────────────────────────────────────
    void resetAll() noexcept {
        std::lock_guard lk(mu_);
        slots_.clear();
        globalDecision_ = MarketDecision::CONTINUE;
    }

    // ─── Dashboard slots ─────────────────────────────────────
    [[nodiscard]] std::string slotTable() const {
        std::lock_guard lk(mu_);
        if (slots_.empty()) return "  Aucun slot actif\n";
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        for (const auto& s : slots_) {
            if (!s.isActive()) continue;
            oss << "  " << std::setw(3) << std::left << s.label()
                << " [" << s.stateLabel() << "] "
                << " Trail:" << s.trailLabel()
                << "  SS#" << s.ssTicket
                << "  BS#" << s.bsTicket
                << "  PnL:" << std::setw(7) << s.openPnl << "$\n";
        }
        return oss.str();
    }

private:
    void cleanupIdle() noexcept {
        slots_.erase(
            std::remove_if(slots_.begin(), slots_.end(),
                           [](const GridSlotAgent& s){ return !s.isActive(); }),
            slots_.end());
    }

    mutable std::mutex          mu_;
    std::vector<GridSlotAgent>  slots_;
    MarketDecision              globalDecision_ = MarketDecision::CONTINUE;
};

} // namespace top4ea
