#pragma once
// ============================================================
//  TOP4EA — lstm_weights.hpp
//  Format binaire portable pour les poids du BiLSTM
//
//  STRUCTURE FICHIER .bin :
//  ─────────────────────────────────────────────────────────
//  [10 bytes] Magic : "TOP4EALSTM"
//  [4  bytes] Version : uint32_t = 1
//  [4  bytes] input_size  (int32)
//  [4  bytes] hidden_size (int32)
//  [4  bytes] n_layers    (int32)
//  [4  bytes] n_heads     (int32)
//  Pour chaque couche BiLSTM [0 .. n_layers-1] :
//    forward  LSTMWeights : W(4H×I), U(4H×H), b(4H) — float32
//    backward LSTMWeights : W(4H×I), U(4H×H), b(4H) — float32
//  AttentionWeights : Wq,Wk,Wv,Wo (D×D), bq,bk,bv,bo (D) — float32
//  W_action(N_ACTIONS×2H), b_action(N_ACTIONS)
//  W_calib (N_CALIB   ×2H), b_calib (N_CALIB)
//
//  Compatible avec export NumPy/PyTorch (np.float32, contiguous).
// ============================================================
#include "lstm_core.hpp"
#include <fstream>
#include <cstdint>
#include <stdexcept>
#include <iostream>

namespace top4ea {
namespace lstm {

static constexpr char LSTM_MAGIC[10] = {'T','O','P','4','E','A','L','S','T','M'};
static constexpr uint32_t LSTM_VERSION = 1;

// ── Utilitaires de lecture/écriture binaire ───────────────
inline void write_bytes(std::ofstream& f, const void* data, size_t n) {
    f.write(reinterpret_cast<const char*>(data), (std::streamsize)n);
}
inline void read_bytes(std::ifstream& f, void* data, size_t n) {
    f.read(reinterpret_cast<char*>(data), (std::streamsize)n);
    if (!f) throw std::runtime_error("lstm_weights: lecture tronquée");
}

inline void write_vec(std::ofstream& f, const std::vector<float>& v) {
    uint32_t sz = (uint32_t)v.size();
    write_bytes(f, &sz, 4);
    if (sz) write_bytes(f, v.data(), sz * 4);
}
inline void read_vec(std::ifstream& f, std::vector<float>& v) {
    uint32_t sz = 0;
    read_bytes(f, &sz, 4);
    v.resize(sz);
    if (sz) read_bytes(f, v.data(), sz * 4);
}

inline void write_lstm_weights(std::ofstream& f, const LSTMWeights& w) {
    write_vec(f, w.W);
    write_vec(f, w.U);
    write_vec(f, w.b);
}
inline void read_lstm_weights(std::ifstream& f, LSTMWeights& w, int I, int H) {
    w.I = I; w.H = H;
    read_vec(f, w.W);
    read_vec(f, w.U);
    read_vec(f, w.b);
}

inline void write_attention(std::ofstream& f, const AttentionWeights& aw) {
    write_vec(f, aw.Wq); write_vec(f, aw.Wk);
    write_vec(f, aw.Wv); write_vec(f, aw.Wo);
    write_vec(f, aw.bq); write_vec(f, aw.bk);
    write_vec(f, aw.bv); write_vec(f, aw.bo);
}
inline void read_attention(std::ifstream& f, AttentionWeights& aw, int D, int heads) {
    aw.D = D; aw.n_heads = heads; aw.head_dim = D / heads;
    read_vec(f, aw.Wq); read_vec(f, aw.Wk);
    read_vec(f, aw.Wv); read_vec(f, aw.Wo);
    read_vec(f, aw.bq); read_vec(f, aw.bk);
    read_vec(f, aw.bv); read_vec(f, aw.bo);
}

// ── saveLSTMWeights — sauvegarde complète ─────────────────
[[nodiscard]] inline bool saveLSTMWeights(
        const std::string& path,
        const BiLSTMModelWeights& mw) noexcept
{
    try {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;

        // Header
        write_bytes(f, LSTM_MAGIC, 10);
        write_bytes(f, &LSTM_VERSION, 4);
        int32_t inp  = mw.input_size,  hid = mw.hidden_size;
        int32_t nl   = mw.n_layers,    nh  = mw.n_heads;
        write_bytes(f, &inp, 4); write_bytes(f, &hid, 4);
        write_bytes(f, &nl,  4); write_bytes(f, &nh,  4);

        // Couches BiLSTM
        int in_dim = inp;
        for (int l = 0; l < nl; ++l) {
            write_lstm_weights(f, mw.layers[l].fwd);
            write_lstm_weights(f, mw.layers[l].bwd);
            in_dim = 2 * hid;
        }

        // Attention
        write_attention(f, mw.attn);

        // Têtes de sortie
        write_vec(f, mw.W_action); write_vec(f, mw.b_action);
        write_vec(f, mw.W_calib);  write_vec(f, mw.b_calib);

        return f.good();
    } catch (...) { return false; }
}

// ── loadLSTMWeights — chargement complet ─────────────────
[[nodiscard]] inline bool loadLSTMWeights(
        const std::string& path,
        BiLSTMModelWeights& mw) noexcept
{
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        // Vérification magic
        char magic[10] = {};
        read_bytes(f, magic, 10);
        if (std::memcmp(magic, LSTM_MAGIC, 10) != 0) {
            std::cerr << "[LSTM] Magic invalide dans " << path << "\n";
            return false;
        }
        uint32_t ver = 0;
        read_bytes(f, &ver, 4);
        if (ver != LSTM_VERSION) {
            std::cerr << "[LSTM] Version " << ver << " != " << LSTM_VERSION << "\n";
            return false;
        }

        // Dimensions
        int32_t inp = 0, hid = 0, nl = 0, nh = 0;
        read_bytes(f, &inp, 4); read_bytes(f, &hid, 4);
        read_bytes(f, &nl,  4); read_bytes(f, &nh,  4);
        mw.input_size  = inp; mw.hidden_size = hid;
        mw.n_layers    = nl;  mw.n_heads     = nh;

        // Couches BiLSTM
        mw.layers.resize(nl);
        int in_dim = inp;
        for (int l = 0; l < nl; ++l) {
            read_lstm_weights(f, mw.layers[l].fwd, in_dim, hid);
            read_lstm_weights(f, mw.layers[l].bwd, in_dim, hid);
            in_dim = 2 * hid;
        }

        // Attention
        read_attention(f, mw.attn, 2 * hid, nh);

        // Têtes de sortie
        read_vec(f, mw.W_action); read_vec(f, mw.b_action);
        read_vec(f, mw.W_calib);  read_vec(f, mw.b_calib);

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[LSTM] Erreur chargement " << path << " : " << e.what() << "\n";
        return false;
    }
}

// ── Compatibilité Python : export format NumPy ───────────
// Le binaire Python bilstm_trainer.py écrit dans le même format,
// mais utilise le header Python suivant pour les dimensions :
//
//   struct.pack('<10sIiiii', b'TOP4EALSTM', 1, inp, hid, nl, nh)
//
// Puis pour chaque vecteur float32 :
//   struct.pack('<I', len(arr)) + arr.astype(np.float32).tobytes()
//
// La compatibilité est garantie par l'ordre little-endian et float32.

// ── Informations sur les poids chargés ────────────────────
inline std::string lstmWeightsInfo(const BiLSTMModelWeights& mw) {
    size_t total = 0;
    for (const auto& l : mw.layers) {
        total += l.fwd.W.size() + l.fwd.U.size() + l.fwd.b.size();
        total += l.bwd.W.size() + l.bwd.U.size() + l.bwd.b.size();
    }
    total += mw.attn.Wq.size() + mw.attn.Wk.size()
           + mw.attn.Wv.size() + mw.attn.Wo.size();
    total += mw.W_action.size() + mw.W_calib.size();
    std::string s = "BiLSTM inp=" + std::to_string(mw.input_size)
                  + " hid=" + std::to_string(mw.hidden_size)
                  + " L="   + std::to_string(mw.n_layers)
                  + " H="   + std::to_string(mw.n_heads)
                  + " params=" + std::to_string(total);
    return s;
}

} // namespace lstm
} // namespace top4ea
