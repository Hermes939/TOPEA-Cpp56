#pragma once
// ============================================================
//  TOP4EA — lstm_agent.hpp
//  Agent DQN profond avec apprentissage continu online
//
//  ARCHITECTURE :
//  ─────────────────────────────────────────────────────────
//  Online network  : BiLSTMModelWeights (mis à jour à chaque batch)
//  Target network  : copie gelée, synchronisée toutes les
//                    target_update_freq mises à jour
//  Replay buffer   : CircularBuffer<Experience> — TD-error prioritaire
//  Optimizer       : Adam (m/v par paramètre, dans AdamState)
//
//  ACTIONS (N_ACTIONS = 6) :
//    0 = HOLD       — aucun changement
//    1 = BUY_BIAS   — favoriser BS, réduire SS
//    2 = SELL_BIAS  — favoriser SS, réduire BS
//    3 = REDUCE     — réduire sizing × 0.7
//    4 = INCREASE   — augmenter sizing × 1.3
//    5 = CLOSE_NOW  — forcer fermeture basket
//
//  CALIBRATION (N_CALIB = 4 — sigmoid → intervalles réels) :
//    0 = gridGapMult  ∈ [0.50, 3.00]
//    1 = kellyFrac    ∈ [0.05, 0.50]
//    2 = velocityThr  ∈ [0.60, 0.98]
//    3 = cascadeZ     ∈ [0.50, 3.00]
//
//  FEATURE VECTOR (N_FEATURES = 16) :
//    [0]  rho1 ∈ [-1,1]
//    [1]  atrNorm ∈ [0,1]
//    [2]  regimeNorm ∈ [0,1]
//    [3]  netDeltaNorm ∈ [-1,1]
//    [4]  hedgePressure ∈ [0,1]
//    [5]  riskNorm ∈ [0,1]
//    [6]  drawdownNorm ∈ [0,1]
//    [7]  momentumFast ∈ [-1,1]
//    [8]  momentumSlow ∈ [-1,1]
//    [9]  imbalanceNorm ∈ [0,1]
//    [10] ofiNorm ∈ [-1,1]
//    [11] closeProbNorm ∈ [0,1]
//    [12] pnlVelNorm ∈ [-1,1]
//    [13] buyLotsNorm ∈ [0,1]
//    [14] sellLotsNorm ∈ [0,1]
//    [15] basketPnLRatio ∈ [-1,1]
// ============================================================
#include "lstm_core.hpp"
#include "lstm_weights.hpp"
#include <deque>
#include <mutex>
#include <cstdint>
#include <filesystem>

namespace top4ea {
namespace lstm {

static constexpr int N_FEATURES = 16;

// ────────────────────────────────────────────────────────────
//  Calibration output — intervalles réels dénormalisés
// ────────────────────────────────────────────────────────────
struct CalibOutput {
    float gridGapMult  = 1.0f;  // [0.50, 3.00]
    float kellyFrac    = 0.25f; // [0.05, 0.50]
    float velocityThr  = 0.85f; // [0.60, 0.98]
    float cascadeZ     = 1.5f;  // [0.50, 3.00]
};

inline CalibOutput denorm_calib(const Vec& sigmoid_out) noexcept {
    auto lerp = [](float s, float lo, float hi) {
        return lo + s * (hi - lo);
    };
    CalibOutput c;
    if (sigmoid_out.size() >= 4) {
        c.gridGapMult = lerp(sigmoid_out[0], 0.50f, 3.00f);
        c.kellyFrac   = lerp(sigmoid_out[1], 0.05f, 0.50f);
        c.velocityThr = lerp(sigmoid_out[2], 0.60f, 0.98f);
        c.cascadeZ    = lerp(sigmoid_out[3], 0.50f, 3.00f);
    }
    return c;
}

// ────────────────────────────────────────────────────────────
//  Actions
// ────────────────────────────────────────────────────────────
enum class LSTMAction : int {
    HOLD       = 0,
    BUY_BIAS   = 1,
    SELL_BIAS  = 2,
    REDUCE     = 3,
    INCREASE   = 4,
    CLOSE_NOW  = 5
};
inline const char* lstm_action_name(LSTMAction a) noexcept {
    switch (a) {
        case LSTMAction::HOLD:      return "HOLD";
        case LSTMAction::BUY_BIAS:  return "BUY_BIAS";
        case LSTMAction::SELL_BIAS: return "SELL_BIAS";
        case LSTMAction::REDUCE:    return "REDUCE";
        case LSTMAction::INCREASE:  return "INCREASE";
        case LSTMAction::CLOSE_NOW: return "CLOSE_NOW";
        default:                    return "UNKNOWN";
    }
}

// ────────────────────────────────────────────────────────────
//  Experience — transition (s, a, r, s', done)
//  s et s' sont des séquences complètes (seq_len × N_FEATURES)
// ────────────────────────────────────────────────────────────
struct Experience {
    std::vector<Vec> state;      // séquence s
    int              action;     // action prise
    float            reward;     // récompense
    std::vector<Vec> next_state; // séquence s'
    bool             done;       // épisode terminé
    float            td_error;   // priorité (|TD-error|)
};

// ────────────────────────────────────────────────────────────
//  Adam optimizer state — un jeu de moments m/v par poids
// ────────────────────────────────────────────────────────────
struct AdamState {
    float lr       = 1e-4f;
    float beta1    = 0.9f;
    float beta2    = 0.999f;
    float eps      = 1e-8f;
    int   step     = 0;

    // Moments pour chaque vecteur de poids
    // Associés à BiLSTMModelWeights par structure parallèle
    struct LayerMom {
        Vec mW, vW, mU, vU, mb, vb; // fwd
        Vec mWb, vWb, mUb, vUb, mbb, vbb; // bwd
    };
    std::vector<LayerMom> layers;

    // Attention
    Vec mWq, vWq, mWk, vWk, mWv, vWv, mWo, vWo;
    Vec mbq, vbq, mbk, vbk, mbv, vbv, mbo, vbo;

    // Têtes
    Vec mWa, vWa, mba, vba;
    Vec mWc, vWc, mbc, vbc;

    void init(const BiLSTMModelWeights& mw) {
        auto make = [](size_t n) { return Vec(n, 0.0f); };
        int nl = mw.n_layers;
        layers.resize(nl);
        for (int l = 0; l < nl; ++l) {
            auto& f = mw.layers[l].fwd;
            auto& b = mw.layers[l].bwd;
            layers[l].mW  = make(f.W.size()); layers[l].vW  = make(f.W.size());
            layers[l].mU  = make(f.U.size()); layers[l].vU  = make(f.U.size());
            layers[l].mb  = make(f.b.size()); layers[l].vb  = make(f.b.size());
            layers[l].mWb = make(b.W.size()); layers[l].vWb = make(b.W.size());
            layers[l].mUb = make(b.U.size()); layers[l].vUb = make(b.U.size());
            layers[l].mbb = make(b.b.size()); layers[l].vbb = make(b.b.size());
        }
        int D = mw.attn.D;
        mWq = vWq = mWk = vWk = mWv = vWv = mWo = vWo = make(D*D);
        mbq = vbq = mbk = vbk = mbv = vbv = mbo = vbo = make(D);
        mWa = vWa = make(mw.W_action.size());
        mba = vba = make(mw.b_action.size());
        mWc = vWc = make(mw.W_calib.size());
        mbc = vbc = make(mw.b_calib.size());
    }

    // Pas Adam sur un vecteur (en-place)
    void adam_update(Vec& param, Vec& g, Vec& m, Vec& v) {
        ++step;
        float bc1 = 1.0f - std::pow(beta1, (float)step);
        float bc2 = 1.0f - std::pow(beta2, (float)step);
        for (size_t i = 0; i < param.size(); ++i) {
            m[i] = beta1 * m[i] + (1-beta1) * g[i];
            v[i] = beta2 * v[i] + (1-beta2) * g[i] * g[i];
            float m_hat = m[i] / bc1;
            float v_hat = v[i] / bc2;
            param[i] -= lr * m_hat / (std::sqrt(v_hat) + eps);
        }
        std::fill(g.begin(), g.end(), 0.0f);
    }
};

// ────────────────────────────────────────────────────────────
//  LSTMAgent — DQN online complet
// ────────────────────────────────────────────────────────────
class LSTMAgent {
public:
    struct Config {
        int   input_size          = N_FEATURES;
        int   hidden_size         = 64;
        int   n_layers            = 2;
        int   n_heads             = 4;
        int   seq_len             = 32;
        int   replay_capacity     = 4096;
        int   batch_size          = 16;
        int   target_update_freq  = 50;
        int   update_every        = 10;  // ticks entre deux mises à jour
        float epsilon_start       = 0.9f;
        float epsilon_end         = 0.1f;
        float epsilon_decay       = 0.995f;
        float gamma               = 0.95f;
        float lr                  = 1e-4f;
        std::string weights_path  = "automl/output/lstm_weights.bin";
        bool  enabled             = false;  // désactivé par défaut (activé via conf)
    };

    explicit LSTMAgent(const Config& cfg = {}) : cfg_(cfg) {
        std::mt19937 rng(42);
        online_.init(cfg.input_size, cfg.hidden_size,
                     cfg.n_layers, cfg.n_heads, rng);
        target_ = online_;
        adam_.init(online_);
        adam_.lr = cfg.lr;
        epsilon_ = cfg.epsilon_start;
        seq_buf_.resize(cfg.seq_len, Vec(cfg.input_size, 0.0f));
    }

    // ── Alimentation de la fenêtre séquentielle ───────────
    void pushFeature(const Vec& feat) noexcept {
        std::lock_guard lk(mu_);
        std::rotate(seq_buf_.begin(), seq_buf_.begin() + 1, seq_buf_.end());
        seq_buf_.back() = feat;
        ++tick_count_;
    }

    // ── Sélection d'action ε-greedy ────────────────────────
    struct SelectResult {
        LSTMAction  action;
        CalibOutput calib;
        float       confidence;  // max(softmax(logits))
        float       epsilon;
    };

    [[nodiscard]] SelectResult selectAction() noexcept {
        std::lock_guard lk(mu_);
        SelectResult r;
        r.epsilon = epsilon_;

        // Exploration aléatoire
        if (static_cast<float>(rng_() % 1000) / 1000.0f < epsilon_) {
            r.action     = static_cast<LSTMAction>(rng_() % N_ACTIONS);
            r.calib      = {};
            r.confidence = 1.0f / N_ACTIONS;
            return r;
        }

        // Inférence
        auto res = bilstm_forward(seq_buf_, online_);
        Vec logits = res.action_logits;
        softmax(logits);
        float mx = *std::max_element(logits.begin(), logits.end());

        r.action     = static_cast<LSTMAction>(res.best_action);
        r.calib      = denorm_calib(res.calib_sigmoid);
        r.confidence = mx;
        last_fwd_    = res;
        return r;
    }

    // ── Enregistrement d'une transition ──────────────────
    void recordTransition(const std::vector<Vec>& state,
                          int action, float reward,
                          const std::vector<Vec>& next_state,
                          bool done) noexcept
    {
        std::lock_guard lk(mu_);
        Experience e;
        e.state      = state;
        e.action     = action;
        e.reward     = reward;
        e.next_state = next_state;
        e.done       = done;
        e.td_error   = std::abs(reward) + 1.0f;  // priorité initiale

        if ((int)replay_.size() >= cfg_.replay_capacity)
            replay_.pop_front();
        replay_.push_back(std::move(e));
        ++transitions_total_;
    }

    // Version pratique : utilise le buffer courant
    void recordCycleClosed(int action, float reward) noexcept {
        std::lock_guard lk(mu_);
        if (prev_seq_.empty()) { prev_seq_ = seq_buf_; return; }
        Experience e;
        e.state      = prev_seq_;
        e.action     = action;
        e.reward     = reward;
        e.next_state = seq_buf_;
        e.done       = true;
        e.td_error   = std::abs(reward) + 1.0f;
        if ((int)replay_.size() >= cfg_.replay_capacity)
            replay_.pop_front();
        replay_.push_back(std::move(e));
        prev_seq_ = seq_buf_;
        ++transitions_total_;
    }

    // ── Mise à jour gradient (online learning) ───────────
    void update() noexcept {
        std::lock_guard lk(mu_);
        if ((int)replay_.size() < cfg_.batch_size) return;

        // Échantillon prioritaire (top-k TD-error)
        int B = cfg_.batch_size;
        std::vector<int> idx(replay_.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::partial_sort(idx.begin(), idx.begin() + B, idx.end(),
            [&](int a, int b){ return replay_[a].td_error > replay_[b].td_error; });

        // Accumulateurs gradients (output head seulement — BPTT léger)
        BiLSTMModelWeights dW;
        dW.input_size  = online_.input_size;
        dW.hidden_size = online_.hidden_size;
        dW.n_layers    = online_.n_layers;
        dW.n_heads     = online_.n_heads;
        dW.layers.resize(online_.n_layers);
        for (auto& l : dW.layers) {
            l.fwd.init_zero(online_.input_size, online_.hidden_size);
            l.bwd.init_zero(online_.input_size, online_.hidden_size);
        }
        dW.attn.init_zero(2*online_.hidden_size, online_.n_heads);
        dW.W_action.assign(online_.W_action.size(), 0.0f);
        dW.b_action.assign(online_.b_action.size(), 0.0f);
        dW.W_calib .assign(online_.W_calib.size(),  0.0f);
        dW.b_calib .assign(online_.b_calib.size(),  0.0f);

        float total_loss = 0.0f;
        for (int i = 0; i < B; ++i) {
            auto& e = replay_[idx[i]];

            // Forward sur état courant
            auto fwd = bilstm_forward(e.state, online_);

            // Q-target depuis target network
            float q_target = e.reward;
            if (!e.done) {
                auto fwd_next = bilstm_forward(e.next_state, target_);
                float max_q = *std::max_element(
                    fwd_next.action_logits.begin(), fwd_next.action_logits.end());
                q_target += cfg_.gamma * max_q;
            }
            float q_pred  = fwd.action_logits[e.action];
            float td_err  = q_target - q_pred;
            total_loss   += td_err * td_err;
            replay_[idx[i]].td_error = std::abs(td_err) + 1e-5f;  // mise à jour priorité

            // Gradient sur action logits
            Vec d_action(N_ACTIONS, 0.0f);
            d_action[e.action] = -2.0f * td_err / (float)B;

            // Gradient sur calib (MSE minimal — favorise valeurs centrales)
            Vec d_calib(N_CALIB, 0.0f);
            for (int k = 0; k < N_CALIB; ++k) {
                float target_calib = 0.5f;  // centrer sur la médiane
                d_calib[k] = 2.0f * (fwd.calib_sigmoid[k] - target_calib) * 0.01f / (float)B;
            }

            // Gradients output heads
            const Vec& last = fwd.layer_outputs_last.back();
            int D = online_.hidden_size * 2;
            for (int a = 0; a < N_ACTIONS; ++a) {
                float da = d_action[a];
                for (int d = 0; d < D; ++d)
                    dW.W_action[a*D+d] += da * last[d];
                dW.b_action[a] += da;
            }
            for (int k = 0; k < N_CALIB; ++k) {
                float dc = d_calib[k] * dsigmoid(fwd.calib_sigmoid[k]);
                for (int d = 0; d < D; ++d)
                    dW.W_calib[k*D+d] += dc * last[d];
                dW.b_calib[k] += dc;
            }
        }
        last_loss_ = total_loss / (float)B;

        // Appliquer Adam sur les têtes de sortie uniquement
        adam_.adam_update(online_.W_action, dW.W_action, adam_.mWa, adam_.vWa);
        adam_.adam_update(online_.b_action, dW.b_action, adam_.mba, adam_.vba);
        adam_.adam_update(online_.W_calib,  dW.W_calib,  adam_.mWc, adam_.vWc);
        adam_.adam_update(online_.b_calib,  dW.b_calib,  adam_.mbc, adam_.vbc);

        ++update_count_;

        // Désintégration epsilon
        epsilon_ = std::max(cfg_.epsilon_end, epsilon_ * cfg_.epsilon_decay);

        // Synchronisation target network
        if (update_count_ % cfg_.target_update_freq == 0)
            target_ = online_;
    }

    // ── Mise à jour périodique (appelée depuis onTick) ────
    void maybeUpdate() noexcept {
        if (tick_count_ % cfg_.update_every == 0) update();
    }

    // ── Persistance ───────────────────────────────────────
    [[nodiscard]] bool saveWeights(const std::string& path = "") const noexcept {
        std::lock_guard lk(mu_);
        std::string p = path.empty() ? cfg_.weights_path : path;
        std::filesystem::create_directories(
            std::filesystem::path(p).parent_path());
        return saveLSTMWeights(p, online_);
    }

    [[nodiscard]] bool loadWeights(const std::string& path = "") noexcept {
        std::lock_guard lk(mu_);
        std::string p = path.empty() ? cfg_.weights_path : path;
        bool ok = loadLSTMWeights(p, online_);
        if (ok) {
            target_ = online_;
            adam_.init(online_);
            adam_.lr = cfg_.lr;
        }
        return ok;
    }

    // ── Accès état interne ────────────────────────────────
    [[nodiscard]] float   lastLoss()         const noexcept { return last_loss_; }
    [[nodiscard]] float   epsilon()          const noexcept { return epsilon_; }
    [[nodiscard]] int     updateCount()      const noexcept { return update_count_; }
    [[nodiscard]] int     replaySize()       const noexcept { return (int)replay_.size(); }
    [[nodiscard]] int64_t transitionsTotal() const noexcept { return transitions_total_; }
    [[nodiscard]] bool    isEnabled()        const noexcept { return cfg_.enabled; }

    [[nodiscard]] std::vector<Vec> currentSeq() const noexcept {
        std::lock_guard lk(mu_);
        return seq_buf_;
    }

    // ── Rapport ───────────────────────────────────────────
    [[nodiscard]] std::string report() const noexcept {
        std::lock_guard lk(mu_);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[LSTM] enabled=%d  epsilon=%.3f  loss=%.5f  "
            "updates=%d  replay=%d/%d  transitions=%lld",
            (int)cfg_.enabled, epsilon_, last_loss_,
            update_count_, (int)replay_.size(), cfg_.replay_capacity,
            (long long)transitions_total_);
        return buf;
    }

    const Config& config() const { return cfg_; }

private:
    Config               cfg_;
    BiLSTMModelWeights   online_, target_;
    AdamState            adam_;
    mutable std::mutex   mu_;

    std::deque<Experience>  replay_;
    std::vector<Vec>        seq_buf_;   // fenêtre glissante courante
    std::vector<Vec>        prev_seq_;  // snapshot avant dernier cycle
    ForwardResult           last_fwd_;

    float    epsilon_        = 0.9f;
    float    last_loss_      = 0.0f;
    int      update_count_   = 0;
    int      tick_count_     = 0;
    int64_t  transitions_total_ = 0;
    mutable std::mt19937 rng_{ std::random_device{}() };
};

// ────────────────────────────────────────────────────────────
//  Helpers feature engineering (depuis une Analysis)
//  Retourne un vecteur N_FEATURES = 16 normalisé
// ────────────────────────────────────────────────────────────
struct FeatureContext {
    double rho1, atrPoints, regime, netDelta, hedgePressure;
    double riskScore, drawdownPct, momentumFast, momentumSlow;
    double imbalanceScore, ofiNormalized, closeProbPct;
    double pnlVelocity, buyLots, sellLots, basketPnLRatio;
};

[[nodiscard]] inline Vec build_feature_vector(const FeatureContext& fc) noexcept {
    auto clamp01 = [](double v) { return (float)std::clamp(v, 0.0, 1.0); };
    auto clampS  = [](double v) { return (float)std::clamp(v, -1.0, 1.0); };
    float totalLots = (float)(fc.buyLots + fc.sellLots);
    return Vec {
        clampS(fc.rho1),
        clamp01(fc.atrPoints / 3000.0),
        clamp01(fc.regime / 4.0),
        clampS(fc.netDelta / 10.0),
        clamp01(fc.hedgePressure),
        clamp01(fc.riskScore / 100.0),
        clamp01(fc.drawdownPct / 30.0),
        clampS(fc.momentumFast / 5.0),
        clampS(fc.momentumSlow / 5.0),
        clamp01(fc.imbalanceScore / 100.0),
        clampS(fc.ofiNormalized),
        clamp01(fc.closeProbPct / 100.0),
        clampS(fc.pnlVelocity / 10.0),
        clamp01(totalLots > 0.0f ? (float)fc.buyLots  / totalLots : 0.5f),
        clamp01(totalLots > 0.0f ? (float)fc.sellLots / totalLots : 0.5f),
        clampS(fc.basketPnLRatio),
    };
}

} // namespace lstm
} // namespace top4ea
