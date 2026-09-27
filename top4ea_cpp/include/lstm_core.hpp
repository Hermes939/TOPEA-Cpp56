#pragma once
// ============================================================
//  TOP4EA — lstm_core.hpp
//  Bi-LSTM + Multi-Head Attention — C++20 header-only
//  Aucune dépendance externe — arithmétique pure std::vector
//
//  ARCHITECTURE :
//  ─────────────────────────────────────────────────────────
//  LSTMCell<H>            : cellule LSTM standard (4 portes)
//  BiLSTMLayer            : couche bidirectionnelle (fwd+bwd)
//  BiLSTMModel            : L couches empilées + attention
//  MultiHeadAttention     : scaled dot-product, masque causal
//
//  CONVENTIONS :
//  - Matrices stockées row-major : M[r*cols + c]
//  - Types float32 (float) pour portabilité .bin export
//  - Thread-safe si l'appelant ne modifie pas les poids concurrents
// ============================================================
#include <vector>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <string>
#include <cstring>
#include <random>
#include <functional>

namespace top4ea {
namespace lstm {

// ────────────────────────────────────────────────────────────
//  Types de base
// ────────────────────────────────────────────────────────────
using Vec = std::vector<float>;  // vecteur
using Mat = std::vector<float>;  // matrice row-major [rows*cols]

// ────────────────────────────────────────────────────────────
//  Activations scalaires
// ────────────────────────────────────────────────────────────
[[nodiscard]] inline float sigmoid(float x) noexcept {
    return 1.0f / (1.0f + std::exp(-x));
}
[[nodiscard]] inline float tanh_act(float x) noexcept {
    return std::tanh(x);
}
[[nodiscard]] inline float relu(float x) noexcept {
    return x > 0.0f ? x : 0.0f;
}
[[nodiscard]] inline float dsigmoid(float s) noexcept { return s * (1.0f - s); }
[[nodiscard]] inline float dtanh(float t) noexcept { return 1.0f - t * t; }

// ────────────────────────────────────────────────────────────
//  Opérations matricielles de base
// ────────────────────────────────────────────────────────────

// y += M * x   (M: rows×cols, x: cols, y: rows)
inline void matvec_add(const Mat& M, const Vec& x, Vec& y,
                        int rows, int cols) noexcept {
    for (int r = 0; r < rows; ++r) {
        float sum = 0.0f;
        const float* row = M.data() + r * cols;
        for (int c = 0; c < cols; ++c) sum += row[c] * x[c];
        y[r] += sum;
    }
}

// y = M * x + b
inline void linear(const Mat& M, const Vec& b, const Vec& x,
                   Vec& y, int out, int in_) noexcept {
    y.assign(b.begin(), b.end());
    matvec_add(M, x, y, out, in_);
}

// Softmax en-place
inline void softmax(Vec& v) noexcept {
    float mx = *std::max_element(v.begin(), v.end());
    float s  = 0.0f;
    for (auto& x : v) { x = std::exp(x - mx); s += x; }
    for (auto& x : v) x /= s;
}

// Scalaire dot-product
[[nodiscard]] inline float dot(const Vec& a, const Vec& b) noexcept {
    float s = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
    return s;
}

// ────────────────────────────────────────────────────────────
//  Initialisation Xavier / He
// ────────────────────────────────────────────────────────────
inline void xavier_init(Mat& M, int fan_in, int fan_out,
                         std::mt19937& rng) {
    float limit = std::sqrt(6.0f / (float)(fan_in + fan_out));
    std::uniform_real_distribution<float> dist(-limit, limit);
    for (auto& v : M) v = dist(rng);
}
inline void zero_init(Vec& v) { std::fill(v.begin(), v.end(), 0.0f); }

// ────────────────────────────────────────────────────────────
//  LSTMWeights — matrices d'une cellule (une direction)
//
//  Conventions portes : ordre [f, i, g, o] dans les blocs
//    W : [4H × I]   — poids entrée
//    U : [4H × H]   — poids récurrence
//    b : [4H]       — biais
// ────────────────────────────────────────────────────────────
struct LSTMWeights {
    int I = 0;   // taille entrée
    int H = 0;   // taille hidden

    Mat W;        // [4H × I]
    Mat U;        // [4H × H]
    Vec b;        // [4H]

    void init(int input_size, int hidden_size, std::mt19937& rng) {
        I = input_size;
        H = hidden_size;
        W.resize(4 * H * I);   xavier_init(W, I, 4*H, rng);
        U.resize(4 * H * H);   xavier_init(U, H, 4*H, rng);
        b.resize(4 * H, 0.0f);
        // Forget gate bias = 1.0 (convention LSTM pour mémoire longue)
        for (int j = 0; j < H; ++j) b[j] = 1.0f;
    }
    void init_zero(int input_size, int hidden_size) {
        I = input_size; H = hidden_size;
        W.assign(4*H*I, 0.0f);
        U.assign(4*H*H, 0.0f);
        b.assign(4*H, 0.0f);
    }
};

// ────────────────────────────────────────────────────────────
//  LSTMState — (h, c) d'une cellule
// ────────────────────────────────────────────────────────────
struct LSTMState {
    Vec h, c;
    void init(int H) { h.assign(H, 0.0f); c.assign(H, 0.0f); }
    void reset()     { std::fill(h.begin(), h.end(), 0.0f);
                       std::fill(c.begin(), c.end(), 0.0f); }
};

// ────────────────────────────────────────────────────────────
//  LSTMCellOutput — sorties + cache pour backprop
// ────────────────────────────────────────────────────────────
struct LSTMCellCache {
    Vec x, h_prev, c_prev;  // entrées sauvegardées
    Vec f, i, g, o;         // portes post-activation
    Vec c_new, h_new;       // sorties
};

// ────────────────────────────────────────────────────────────
//  lstm_cell_forward — forward pass d'une cellule
// ────────────────────────────────────────────────────────────
inline LSTMCellCache lstm_cell_forward(
        const Vec& x, const LSTMState& st,
        const LSTMWeights& w) noexcept
{
    int H = w.H, I = w.I;
    LSTMCellCache ca;
    ca.x      = x;
    ca.h_prev = st.h;
    ca.c_prev = st.c;

    // Calcul des préactivations : gates = W*x + U*h + b
    Vec gates(4 * H, 0.0f);
    for (int g = 0; g < 4; ++g) {
        for (int j = 0; j < H; ++j) {
            int base = (g * H + j);
            float wx = 0.0f, uh = 0.0f;
            const float* Wrow = w.W.data() + base * I;
            const float* Urow = w.U.data() + base * H;
            for (int k = 0; k < I; ++k) wx += Wrow[k] * x[k];
            for (int k = 0; k < H; ++k) uh += Urow[k] * st.h[k];
            gates[base] = wx + uh + w.b[base];
        }
    }

    // Activation des portes
    ca.f.resize(H); ca.i.resize(H); ca.g.resize(H); ca.o.resize(H);
    ca.c_new.resize(H); ca.h_new.resize(H);
    for (int j = 0; j < H; ++j) {
        ca.f[j] = sigmoid(gates[0*H + j]);
        ca.i[j] = sigmoid(gates[1*H + j]);
        ca.g[j] = tanh_act(gates[2*H + j]);
        ca.o[j] = sigmoid(gates[3*H + j]);
        ca.c_new[j] = ca.f[j] * st.c[j] + ca.i[j] * ca.g[j];
        ca.h_new[j] = ca.o[j] * tanh_act(ca.c_new[j]);
    }
    return ca;
}

// ────────────────────────────────────────────────────────────
//  Gradients d'une cellule LSTM (BPTT — un pas)
//
//  Entrées : cache du forward + dh_next + dc_next (accumulés)
//  Sorties : dL/dW, dL/dU, dL/db, dL/dx, dh_prev, dc_prev
// ────────────────────────────────────────────────────────────
struct LSTMCellGrad {
    LSTMWeights dW;   // gradients poids (même shape que LSTMWeights)
    Vec         dx;   // gradient entrée x
    Vec         dh;   // gradient h_prev
    Vec         dc;   // gradient c_prev
};

inline LSTMCellGrad lstm_cell_backward(
        const LSTMCellCache& ca,
        const Vec& dh_next,  // gradient sur h_new
        const Vec& dc_next,  // gradient sur c_new (accumulé)
        const LSTMWeights& w,
        float clip = 5.0f) noexcept
{
    int H = w.H, I = w.I;
    LSTMCellGrad g;
    g.dW.init_zero(I, H);
    g.dx.assign(I, 0.0f);
    g.dh.assign(H, 0.0f);
    g.dc.assign(H, 0.0f);

    // dh_new → do, dc_new
    Vec dtanh_c(H), dc_total(H), df(H), di(H), dgg(H), dout(H);
    Vec dpreact(4 * H, 0.0f);

    for (int j = 0; j < H; ++j) {
        float tc = tanh_act(ca.c_new[j]);
        // dL/d(o_j) = dh_next_j * tanh(c_new_j)
        float do_j = dh_next[j] * tc;
        // dL/d(c_new) = dh_next_j * o_j * (1 - tanh²) + dc_next_j
        float dc_new_j = dh_next[j] * ca.o[j] * dtanh(tc) + dc_next[j];

        // Gradients portes
        float df_j  = dc_new_j * ca.c_prev[j];
        float di_j  = dc_new_j * ca.g[j];
        float dg_j  = dc_new_j * ca.i[j];
        float dc_prev_j = dc_new_j * ca.f[j];

        // Pré-activations
        dpreact[0*H+j] = df_j  * dsigmoid(ca.f[j]);
        dpreact[1*H+j] = di_j  * dsigmoid(ca.i[j]);
        dpreact[2*H+j] = dg_j  * dtanh(ca.g[j]);
        dpreact[3*H+j] = do_j  * dsigmoid(ca.o[j]);
        g.dc[j]        = dc_prev_j;
    }

    // Gradient clip
    for (auto& v : dpreact)
        v = std::clamp(v, -clip, clip);

    // dW += dpreact ⊗ x
    for (int gate = 0; gate < 4; ++gate) {
        for (int j = 0; j < H; ++j) {
            int base = gate * H + j;
            float dp = dpreact[base];
            float* Wrow = g.dW.W.data() + base * I;
            float* Urow = g.dW.U.data() + base * H;
            for (int k = 0; k < I; ++k) Wrow[k] += dp * ca.x[k];
            for (int k = 0; k < H; ++k) Urow[k] += dp * ca.h_prev[k];
            g.dW.b[base] += dp;
        }
    }

    // dx = W^T * dpreact
    for (int gate = 0; gate < 4; ++gate) {
        for (int j = 0; j < H; ++j) {
            float dp = dpreact[gate*H + j];
            const float* Wrow = w.W.data() + (gate*H+j) * I;
            const float* Urow = w.U.data() + (gate*H+j) * H;
            for (int k = 0; k < I; ++k) g.dx[k] += dp * Wrow[k];
            for (int k = 0; k < H; ++k) g.dh[k] += dp * Urow[k];
        }
    }

    return g;
}

// ────────────────────────────────────────────────────────────
//  BiLSTMLayer — une couche bidirectionnelle
//  Sortie : concaténation [h_fwd || h_bwd] pour chaque timestep
// ────────────────────────────────────────────────────────────
struct BiLSTMLayerWeights {
    LSTMWeights fwd, bwd;
    int output_size() const { return 2 * fwd.H; }
};

// Forward pass d'une couche BiLSTM
// seq : T vecteurs de taille I → retourne T vecteurs de taille 2H
inline std::vector<Vec> bilstm_layer_forward(
        const std::vector<Vec>& seq,
        const BiLSTMLayerWeights& lw,
        std::vector<LSTMCellCache>* fwd_caches = nullptr,
        std::vector<LSTMCellCache>* bwd_caches = nullptr) noexcept
{
    int T = (int)seq.size();
    int H = lw.fwd.H;

    // Forward pass
    std::vector<Vec> h_fwd(T), h_bwd(T);
    LSTMState sf; sf.init(H);
    if (fwd_caches) fwd_caches->resize(T);
    for (int t = 0; t < T; ++t) {
        auto ca = lstm_cell_forward(seq[t], sf, lw.fwd);
        h_fwd[t] = ca.h_new;
        sf.h = ca.h_new;
        sf.c = ca.c_new;
        if (fwd_caches) (*fwd_caches)[t] = std::move(ca);
    }

    // Backward pass (input reversed)
    LSTMState sb; sb.init(H);
    if (bwd_caches) bwd_caches->resize(T);
    for (int t = T - 1; t >= 0; --t) {
        auto ca = lstm_cell_forward(seq[t], sb, lw.bwd);
        h_bwd[t] = ca.h_new;
        sb.h = ca.h_new;
        sb.c = ca.c_new;
        if (bwd_caches) (*bwd_caches)[t] = std::move(ca);
    }

    // Concaténation
    std::vector<Vec> out(T, Vec(2*H));
    for (int t = 0; t < T; ++t) {
        std::copy(h_fwd[t].begin(), h_fwd[t].end(), out[t].begin());
        std::copy(h_bwd[t].begin(), h_bwd[t].end(), out[t].begin() + H);
    }
    return out;
}

// ────────────────────────────────────────────────────────────
//  MultiHeadAttention — Scaled Dot-Product
//
//  seq : T × D
//  Wq,Wk,Wv,Wo : D×D (projetés en n_heads × head_dim)
//  Retourne : T × D
// ────────────────────────────────────────────────────────────
struct AttentionWeights {
    int D = 0, n_heads = 0, head_dim = 0;
    Mat Wq, Wk, Wv, Wo;   // D×D chacune
    Vec bq, bk, bv, bo;   // D chacune
    void init(int d, int heads, std::mt19937& rng) {
        D = d; n_heads = heads; head_dim = d / heads;
        Wq.resize(D*D); xavier_init(Wq, D, D, rng);
        Wk.resize(D*D); xavier_init(Wk, D, D, rng);
        Wv.resize(D*D); xavier_init(Wv, D, D, rng);
        Wo.resize(D*D); xavier_init(Wo, D, D, rng);
        bq.assign(D,0); bk.assign(D,0); bv.assign(D,0); bo.assign(D,0);
    }
    void init_zero(int d, int heads) {
        D = d; n_heads = heads; head_dim = d / heads;
        Wq.assign(D*D,0); Wk.assign(D*D,0);
        Wv.assign(D*D,0); Wo.assign(D*D,0);
        bq.assign(D,0);   bk.assign(D,0);
        bv.assign(D,0);   bo.assign(D,0);
    }
};

inline std::vector<Vec> multihead_attention(
        const std::vector<Vec>& seq,
        const AttentionWeights& aw) noexcept
{
    int T = (int)seq.size();
    int D = aw.D, H = aw.n_heads, Hd = aw.head_dim;
    float scale = 1.0f / std::sqrt((float)Hd);

    // Project Q, K, V : T × D
    auto project = [&](const Mat& W, const Vec& b, std::vector<Vec>& out) {
        out.resize(T);
        for (int t = 0; t < T; ++t) {
            linear(W, b, seq[t], out[t], D, D);
        }
    };
    std::vector<Vec> Q, K, V;
    project(aw.Wq, aw.bq, Q);
    project(aw.Wk, aw.bk, K);
    project(aw.Wv, aw.bv, V);

    // Per-head attention
    std::vector<Vec> ctx(T, Vec(D, 0.0f));
    for (int h = 0; h < H; ++h) {
        int off = h * Hd;
        for (int t = 0; t < T; ++t) {
            // Scores : s[t] = Q[t][h:] · K[s][h:] * scale  (causal mask)
            Vec scores(t + 1);
            for (int s = 0; s <= t; ++s) {
                float sc = 0.0f;
                for (int d = 0; d < Hd; ++d)
                    sc += Q[t][off+d] * K[s][off+d];
                scores[s] = sc * scale;
            }
            softmax(scores);
            // Context = weighted sum of V
            for (int s = 0; s <= t; ++s) {
                for (int d = 0; d < Hd; ++d)
                    ctx[t][off+d] += scores[s] * V[s][off+d];
            }
        }
    }

    // Output projection : out = ctx * Wo + bo
    std::vector<Vec> out(T);
    for (int t = 0; t < T; ++t)
        linear(aw.Wo, aw.bo, ctx[t], out[t], D, D);

    // Residual connection
    for (int t = 0; t < T; ++t)
        for (int d = 0; d < D; ++d)
            out[t][d] += seq[t][d];

    return out;
}

// ────────────────────────────────────────────────────────────
//  BiLSTMModel — modèle complet
//
//  Architecture :
//    Input(I) × T
//    → BiLSTMLayer × n_layers (sortie 2H par couche)
//    → MultiHeadAttention (sur la sortie finale 2H)
//    → Last timestep → Linear(2H → N_ACTIONS)   action head
//                    → Linear(2H → N_CALIB)     calibration head
//
//  N_ACTIONS = 6   (voir lstm_agent.hpp)
//  N_CALIB   = 4   (gridGapMult, kellyFrac, velocityThr, cascadeZ)
// ────────────────────────────────────────────────────────────
static constexpr int N_ACTIONS = 6;
static constexpr int N_CALIB   = 4;

struct BiLSTMModelWeights {
    int input_size  = 16;
    int hidden_size = 64;
    int n_layers    = 2;
    int n_heads     = 4;

    std::vector<BiLSTMLayerWeights> layers;
    AttentionWeights                attn;

    // Output heads : [2H → N_ACTIONS] et [2H → N_CALIB]
    Mat  W_action, W_calib;
    Vec  b_action, b_calib;

    void init(int inp, int hid, int nl, int heads, std::mt19937& rng) {
        input_size = inp; hidden_size = hid;
        n_layers   = nl;  n_heads     = heads;

        layers.resize(nl);
        int in_dim = inp;
        for (int l = 0; l < nl; ++l) {
            layers[l].fwd.init(in_dim, hid, rng);
            layers[l].bwd.init(in_dim, hid, rng);
            in_dim = 2 * hid;  // concaténation fwd+bwd
        }
        attn.init(2 * hid, heads, rng);

        int D = 2 * hid;
        W_action.resize(N_ACTIONS * D); xavier_init(W_action, D, N_ACTIONS, rng);
        W_calib .resize(N_CALIB   * D); xavier_init(W_calib,  D, N_CALIB,   rng);
        b_action.assign(N_ACTIONS, 0.0f);
        b_calib .assign(N_CALIB,   0.0f);
    }
};

// ── Forward pass complet ──────────────────────────────────
struct ForwardResult {
    Vec  action_logits;  // N_ACTIONS (softmax non appliqué)
    Vec  calib_raw;      // N_CALIB   (sigmoid non appliqué)
    int  best_action;    // argmax
    Vec  calib_sigmoid;  // N_CALIB   post-sigmoid
    // Caches pour backprop
    std::vector<std::vector<LSTMCellCache>> fwd_caches, bwd_caches;  // [layer][t]
    std::vector<Vec> layer_outputs_last;  // sortie dernière couche après attention
};

inline ForwardResult bilstm_forward(
        const std::vector<Vec>& seq,
        const BiLSTMModelWeights& mw) noexcept
{
    ForwardResult res;
    int nl = mw.n_layers;
    res.fwd_caches.resize(nl);
    res.bwd_caches.resize(nl);

    std::vector<Vec> x = seq;
    for (int l = 0; l < nl; ++l) {
        x = bilstm_layer_forward(x, mw.layers[l],
                                 &res.fwd_caches[l],
                                 &res.bwd_caches[l]);
    }

    // Attention sur la sortie du dernier BiLSTM
    auto ctx = multihead_attention(x, mw.attn);

    // Dernier timestep → têtes de sortie
    const Vec& last = ctx.back();
    int D = mw.hidden_size * 2;

    linear(mw.W_action, mw.b_action, last, res.action_logits, N_ACTIONS, D);
    linear(mw.W_calib,  mw.b_calib,  last, res.calib_raw,     N_CALIB,   D);

    // Sigmoid sur calibration
    res.calib_sigmoid.resize(N_CALIB);
    for (int k = 0; k < N_CALIB; ++k)
        res.calib_sigmoid[k] = sigmoid(res.calib_raw[k]);

    // Argmax actions
    res.best_action = (int)(std::max_element(res.action_logits.begin(),
                                              res.action_logits.end())
                            - res.action_logits.begin());

    res.layer_outputs_last = ctx;
    return res;
}

// ── Gradients output heads (simplified — full BiLSTM backprop) ──
struct HeadGradients {
    Mat  dW_action, dW_calib;
    Vec  db_action, db_calib;
    Vec  d_last;  // gradient sur le dernier vecteur (pour BPTT)
};

inline HeadGradients compute_head_gradients(
        const Vec& last_ctx,
        const Vec& d_action_logits,
        const Vec& d_calib_raw,
        const BiLSTMModelWeights& mw) noexcept
{
    int D = mw.hidden_size * 2;
    HeadGradients hg;
    hg.dW_action.assign(N_ACTIONS * D, 0.0f);
    hg.dW_calib .assign(N_CALIB   * D, 0.0f);
    hg.db_action = d_action_logits;
    hg.db_calib  = d_calib_raw;
    hg.d_last.assign(D, 0.0f);

    // dW = d_out ⊗ last_ctx
    for (int a = 0; a < N_ACTIONS; ++a) {
        float da = d_action_logits[a];
        for (int d = 0; d < D; ++d)
            hg.dW_action[a * D + d] += da * last_ctx[d];
    }
    for (int k = 0; k < N_CALIB; ++k) {
        float dc = d_calib_raw[k];
        for (int d = 0; d < D; ++d)
            hg.dW_calib[k * D + d] += dc * last_ctx[d];
    }

    // d_last = W_action^T * d_action + W_calib^T * d_calib
    for (int a = 0; a < N_ACTIONS; ++a) {
        float da = d_action_logits[a];
        for (int d = 0; d < D; ++d)
            hg.d_last[d] += da * mw.W_action[a * D + d];
    }
    for (int k = 0; k < N_CALIB; ++k) {
        float dc = d_calib_raw[k];
        for (int d = 0; d < D; ++d)
            hg.d_last[d] += dc * mw.W_calib[k * D + d];
    }
    return hg;
}

} // namespace lstm
} // namespace top4ea
