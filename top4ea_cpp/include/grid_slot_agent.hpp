#pragma once
// ============================================================
//  TOP4EA — grid_slot_agent.hpp  C++20
//
//  SOUS-AGENT MODULAIRE — niveau Ln (L0, L1, L2 … L49+)
//  ─────────────────────────────────────────────────────────
//  Chaque instance gère UN SEUL niveau de grille :
//    • Un SellStop (SS) indépendant
//    • Un BuyStop  (BS) indépendant
//    • Machine à états propre : IDLE→ARMED→TRIGGERED→CLOSING
//    • TrailMode piloté par le Visual AI (AgentOrchestrator)
//
//  Architecture : les sous-agents ne se connaissent pas.
//  L'AgentOrchestrator coordonne le pool L0..Ln.
// ============================================================
#include "types.hpp"
#include <string>
#include <atomic>

namespace top4ea {

// ============================================================
//  GridSlotAgent — sous-agent d'un niveau de grille
// ============================================================
struct GridSlotAgent {

    // ── Identité ─────────────────────────────────────────────
    int  level      = -1;       // index niveau : 0=L0, 1=L1 …

    // ── Machine à états ──────────────────────────────────────
    SlotState      state        = SlotState::IDLE;
    TrailMode      trailMode    = TrailMode::NATURAL;
    MarketDecision lastDecision = MarketDecision::CONTINUE;

    // ── Côté SellStop ─────────────────────────────────────────
    int    ssTicket  = -1;      // ticket broker (−1 = absent)
    double ssPrice   = 0.0;     // prix d'entrée / déclenchement
    double ssLot     = 0.0;
    double ssPeakPnl = 0.0;     // peak P&L côté SS pour ce slot
    double ssOpenPnl = 0.0;     // P&L courant côté SS

    // ── Côté BuyStop ──────────────────────────────────────────
    int    bsTicket  = -1;
    double bsPrice   = 0.0;
    double bsLot     = 0.0;
    double bsPeakPnl = 0.0;
    double bsOpenPnl = 0.0;

    // ── Métriques agrégées ────────────────────────────────────
    double openPnl   = 0.0;     // ssOpenPnl + bsOpenPnl

    // ============================================================
    //  API
    // ============================================================

    // ─── Armer le slot avec les deux côtés ───────────────────
    void arm(int level_,
             int ssT, double ssP, double ssL,
             int bsT, double bsP, double bsL) noexcept
    {
        level     = level_;
        ssTicket  = ssT;  ssPrice = ssP; ssLot = ssL;
        bsTicket  = bsT;  bsPrice = bsP; bsLot = bsL;
        state     = SlotState::ARMED;
        trailMode = TrailMode::NATURAL;
        ssPeakPnl = bsPeakPnl = ssOpenPnl = bsOpenPnl = openPnl = 0.0;
    }

    // ─── Notifier qu'un côté s'est déclenché ─────────────────
    void notifyTriggered(int ticket) noexcept {
        if (ticket == ssTicket) {
            state = (bsTicket >= 0 && state == SlotState::TRIGGERED_BS)
                    ? SlotState::DUAL_ACTIVE
                    : SlotState::TRIGGERED_SS;
        } else if (ticket == bsTicket) {
            state = (ssTicket >= 0 && state == SlotState::TRIGGERED_SS)
                    ? SlotState::DUAL_ACTIVE
                    : SlotState::TRIGGERED_BS;
        }
    }

    // ─── Mise à jour P&L depuis l'ordre ──────────────────────
    void updatePnl(int ticket, double pnl) noexcept {
        if (ticket == ssTicket) {
            ssOpenPnl = pnl;
            if (pnl > ssPeakPnl) ssPeakPnl = pnl;
        } else if (ticket == bsTicket) {
            bsOpenPnl = pnl;
            if (pnl > bsPeakPnl) bsPeakPnl = pnl;
        }
        openPnl = ssOpenPnl + bsOpenPnl;
    }

    // ─── Notifier fermeture d'un ticket ──────────────────────
    void notifyClosed(int ticket) noexcept {
        if (ticket == ssTicket) { ssTicket = -1; ssOpenPnl = 0.0; }
        if (ticket == bsTicket) { bsTicket = -1; bsOpenPnl = 0.0; }
        openPnl = ssOpenPnl + bsOpenPnl;
        if (ssTicket < 0 && bsTicket < 0) {
            state = SlotState::IDLE;
        }
    }

    // ─── Appliquer une décision Visual AI ────────────────────
    void applyDecision(MarketDecision d) noexcept {
        lastDecision = d;
        switch (d) {
            case MarketDecision::TRAIL_AGGRESSIVE:
                trailMode = TrailMode::AGGRESSIVE; break;
            case MarketDecision::TRAIL_NATURAL:
                trailMode = TrailMode::NATURAL;    break;
            case MarketDecision::TRAIL_FREEZE:
            case MarketDecision::LOCK_WAIT:
                trailMode = TrailMode::FROZEN;     break;
            case MarketDecision::REMOVE_TRAIL:
                trailMode = TrailMode::REMOVED;    break;
            default:
                break;   // CONTINUE / OBSERVE / etc. → pas de changement
        }
    }

    // ─── Facteur gap trail selon le mode ─────────────────────
    [[nodiscard]] double trailGapFactor(double baseGapFactor) const noexcept {
        switch (trailMode) {
            case TrailMode::AGGRESSIVE: return baseGapFactor * 0.50;
            case TrailMode::NATURAL:    return baseGapFactor;
            case TrailMode::FROZEN:     return baseGapFactor * 99.0; // jamais modifié
            case TrailMode::REMOVED:    return 0.0;
        }
        return baseGapFactor;
    }

    // ─── Possession d'un ticket ──────────────────────────────
    [[nodiscard]] bool ownsTicket(int ticket) const noexcept {
        return (ticket >= 0) && (ticket == ssTicket || ticket == bsTicket);
    }

    // ─── Actif si au moins un côté existe ────────────────────
    [[nodiscard]] bool isActive() const noexcept {
        return state != SlotState::IDLE;
    }

    // ─── Reset total ─────────────────────────────────────────
    void reset() noexcept {
        ssTicket = bsTicket = -1;
        ssPrice = bsPrice = ssLot = bsLot = 0.0;
        ssPeakPnl = bsPeakPnl = ssOpenPnl = bsOpenPnl = openPnl = 0.0;
        state        = SlotState::IDLE;
        trailMode    = TrailMode::NATURAL;
        lastDecision = MarketDecision::CONTINUE;
    }

    // ─── Label lisible ────────────────────────────────────────
    [[nodiscard]] std::string label() const {
        return "L" + std::to_string(level);
    }

    [[nodiscard]] std::string trailLabel() const noexcept {
        switch (trailMode) {
            case TrailMode::NATURAL:    return "NAT";
            case TrailMode::AGGRESSIVE: return "AGR";
            case TrailMode::FROZEN:     return "FRZ";
            case TrailMode::REMOVED:    return "OFF";
        }
        return "?";
    }

    [[nodiscard]] std::string stateLabel() const noexcept {
        switch (state) {
            case SlotState::IDLE:         return "IDLE";
            case SlotState::ARMED:        return "ARMD";
            case SlotState::TRIGGERED_SS: return "SS  ";
            case SlotState::TRIGGERED_BS: return "BS  ";
            case SlotState::DUAL_ACTIVE:  return "DUAL";
            case SlotState::CLOSING:      return "CLOS";
        }
        return "????";
    }
};

} // namespace top4ea
