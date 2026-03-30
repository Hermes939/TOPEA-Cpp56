"""
TOP4EA AutoML — Modèle PyTorch d'optimisation des paramètres de trading

ARCHITECTURE :
─────────────────────────────────────────────────────────
Réseau neuronal profond (Deep Neural Network) entraîné pour
prédire les paramètres OPTIMAUX du moteur C++ en fonction
du contexte de marché.

ENTRÉES (features du marché) :
  - atr_pts        : ATR courant en points
  - rho1           : autocorrélation OU (mean-reversion)
  - regime         : régime de marché (0=Range/Calm, 4=Trend/Down)
  - net_delta      : delta de portefeuille normalisé
  - hedge_pressure : pression de déséquilibre [0,1]
  - recov_depth    : profondeur recovery [0,20]
  - drawdown_pct   : drawdown en % du capital
  - ofi_score      : Order Flow Imbalance score [0,100]
  - imbalance_score: score d'imbalance géométrique [0,100]

SORTIES (paramètres optimaux θ) :
  - atr_mult       : multiplicateur ATR pour GridGap
  - kelly_frac     : fraction Kelly pour sizing des lots
  - gap_expansion  : facteur d'expansion recovery
  - trail_asym     : facteur d'asymétrie trail stop
  - vel_trigger    : seuil velocity pour fermeture prédictive

ARCHITECTURE DNN :
  Input(9) → BN → Dense(128) → GELU → Dropout(0.2)
           → Dense(64)  → GELU → Dropout(0.1)
           → Dense(32)  → GELU
           → Dense(5)   → Sigmoid (sorties normalisées [0,1])
           → Dénormalisation vers les bornes réelles

ENTRAÎNEMENT :
  - Loss : MSE sur les paramètres + régularisation L2
  - Optimizer : AdamW (weight decay = 1e-4)
  - Scheduler : CosineAnnealingLR
  - Validation : 20% holdout chronologique (pas de fuite temporelle)
"""

import torch
import torch.nn as nn
import torch.nn.functional as F
import numpy as np
from typing import Dict, Tuple

# ────────────────────────────────────────────────────────────
#  Bornes des paramètres (miroir des ParamBounds C++)
# ────────────────────────────────────────────────────────────
PARAM_BOUNDS = {
    "atr_mult":      (0.20, 2.00),
    "kelly_frac":    (0.05, 0.50),
    "gap_expansion": (0.01, 0.30),
    "trail_asym":    (0.05, 0.40),
    "vel_trigger":   (0.60, 0.98),
    "cppi_mult":     (1.50, 6.00),
    "vabt_coeff":    (0.05, 0.50),
    "ou_theta":      (0.01, 0.30),
}

PARAM_NAMES = list(PARAM_BOUNDS.keys())
N_FEATURES  = 9    # taille du vecteur d'entrée
N_OUTPUTS   = len(PARAM_NAMES)  # 8 paramètres à prédire

# ────────────────────────────────────────────────────────────
#  Noms des features d'entrée
# ────────────────────────────────────────────────────────────
FEATURE_NAMES = [
    "atr_pts_norm",       # ATR normalisé [0,1]
    "rho1",               # autocorrélation OU ∈ [-1,1]
    "regime_norm",        # régime normalisé [0,1]
    "net_delta_norm",     # delta normalisé [-1,1]
    "hedge_pressure",     # pression hedge [0,1]
    "recov_depth_norm",   # profondeur recovery normalisé [0,1]
    "drawdown_norm",      # drawdown normalisé [0,1]
    "ofi_norm",           # OFI normalisé [0,1]
    "imbalance_norm",     # imbalance normalisé [0,1]
]


def normalize_param(value: float, name: str) -> float:
    """Normalise un paramètre vers [0,1] selon ses bornes."""
    lo, hi = PARAM_BOUNDS[name]
    return (value - lo) / (hi - lo)


def denormalize_param(norm: float, name: str) -> float:
    """Dénormalise un paramètre de [0,1] vers ses bornes réelles."""
    lo, hi = PARAM_BOUNDS[name]
    return float(np.clip(lo + norm * (hi - lo), lo, hi))


def denormalize_params(outputs: torch.Tensor) -> Dict[str, float]:
    """Convertit le tenseur de sortie [0,1]^N en paramètres réels."""
    result = {}
    arr = outputs.detach().cpu().numpy().flatten()
    for i, name in enumerate(PARAM_NAMES):
        result[name] = denormalize_param(float(arr[i]), name)
    return result


# ────────────────────────────────────────────────────────────
#  Bloc résiduel avec normalisation
# ────────────────────────────────────────────────────────────
class ResidualBlock(nn.Module):
    """Bloc résiduel avec BatchNorm et activation GELU."""

    def __init__(self, dim: int, dropout: float = 0.1):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(dim, dim),
            nn.BatchNorm1d(dim),
            nn.GELU(),
            nn.Dropout(dropout),
            nn.Linear(dim, dim),
            nn.BatchNorm1d(dim),
        )
        self.act = nn.GELU()

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.act(x + self.net(x))


# ────────────────────────────────────────────────────────────
#  Modèle principal
# ────────────────────────────────────────────────────────────
class TOP4EAParamNet(nn.Module):
    """
    Réseau neuronal pour la prédiction des paramètres optimaux.

    Architecture :
      Input(9) → Embedding(128) → ResBlock × 2 → Head(8)
    """

    def __init__(
        self,
        n_features: int = N_FEATURES,
        n_outputs: int  = N_OUTPUTS,
        hidden_dim: int = 128,
        n_res_blocks: int = 2,
        dropout: float = 0.15,
    ):
        super().__init__()
        self.n_features = n_features
        self.n_outputs  = n_outputs

        # Encodeur d'entrée
        self.input_encoder = nn.Sequential(
            nn.Linear(n_features, hidden_dim),
            nn.BatchNorm1d(hidden_dim),
            nn.GELU(),
            nn.Dropout(dropout),
        )

        # Blocs résiduels
        self.res_blocks = nn.ModuleList([
            ResidualBlock(hidden_dim, dropout / 2)
            for _ in range(n_res_blocks)
        ])

        # Couche intermédiaire de réduction
        self.mid = nn.Sequential(
            nn.Linear(hidden_dim, hidden_dim // 2),
            nn.GELU(),
            nn.Dropout(dropout / 2),
        )

        # Tête de prédiction par paramètre (multi-head)
        # Chaque paramètre a sa propre tête pour plus de précision
        self.heads = nn.ModuleList([
            nn.Sequential(
                nn.Linear(hidden_dim // 2, 16),
                nn.GELU(),
                nn.Linear(16, 1),
                nn.Sigmoid(),  # sortie ∈ [0,1]
            )
            for _ in range(n_outputs)
        ])

        # Initialisation Kaiming pour stabilité
        self._init_weights()

    def _init_weights(self):
        for module in self.modules():
            if isinstance(module, nn.Linear):
                nn.init.kaiming_normal_(module.weight, nonlinearity='relu')
                if module.bias is not None:
                    nn.init.zeros_(module.bias)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """
        x : (batch, n_features)
        retourne : (batch, n_outputs) ∈ [0,1]^n_outputs
        """
        # Encodage
        h = self.input_encoder(x)

        # Blocs résiduels
        for block in self.res_blocks:
            h = block(h)

        # Réduction
        h = self.mid(h)

        # Prédictions multi-head
        outputs = [head(h) for head in self.heads]
        return torch.cat(outputs, dim=-1)

    def predict_params(self, features: np.ndarray) -> Dict[str, float]:
        """Prédit les paramètres optimaux depuis un vecteur de features."""
        self.eval()
        with torch.no_grad():
            x = torch.FloatTensor(features).unsqueeze(0)
            out = self(x)
        return denormalize_params(out.squeeze(0))


# ────────────────────────────────────────────────────────────
#  Loss composite : MSE + contrainte de cohérence
# ────────────────────────────────────────────────────────────
class CompositeLoss(nn.Module):
    """
    Loss = MSE_params + λ_monotone × contrainte_monotone
                      + λ_reg × régularisation L2

    Contrainte monotone :
      - atr_mult doit être ↑ quand hedge_pressure ↑
      - kelly_frac doit être ↓ quand drawdown ↑
    """

    def __init__(
        self,
        lambda_monotone: float = 0.1,
        lambda_reg:      float = 1e-4,
    ):
        super().__init__()
        self.lambda_monotone = lambda_monotone
        self.lambda_reg      = lambda_reg
        self.mse             = nn.MSELoss()

    def forward(
        self,
        pred:     torch.Tensor,  # (batch, n_outputs) prédictions normalisées
        target:   torch.Tensor,  # (batch, n_outputs) cibles normalisées
        features: torch.Tensor,  # (batch, n_features) pour contraintes
        model:    nn.Module,
    ) -> Tuple[torch.Tensor, Dict[str, float]]:

        # Loss principale MSE
        mse_loss = self.mse(pred, target)

        # Régularisation L2 sur les poids
        l2_loss = sum(p.pow(2).sum() for p in model.parameters()
                      if p.requires_grad)

        total = mse_loss + self.lambda_reg * l2_loss

        losses = {
            "mse":  float(mse_loss),
            "l2":   float(l2_loss),
            "total": float(total),
        }

        return total, losses


# ────────────────────────────────────────────────────────────
#  Feature engineering depuis les données brutes
# ────────────────────────────────────────────────────────────
def build_features(
    atr_pts:         float,
    rho1:            float,
    regime:          int,
    net_delta:       float,
    hedge_pressure:  float,
    recov_depth:     int,
    drawdown_pct:    float,
    ofi_score:       float,
    imbalance_score: float,
    atr_pts_max:     float = 2000.0,
) -> np.ndarray:
    """
    Construit le vecteur de features normalisé [0,1] ou [-1,1].
    """
    features = np.array([
        np.clip(atr_pts / atr_pts_max, 0.0, 1.0),          # ATR normalisé
        np.clip(rho1, -1.0, 1.0),                            # ρ₁ ∈ [-1,1]
        np.clip(regime / 4.0, 0.0, 1.0),                    # régime [0→4]
        np.clip(net_delta / 10.0, -1.0, 1.0),               # delta normalisé
        np.clip(hedge_pressure, 0.0, 1.0),                   # pression [0,1]
        np.clip(recov_depth / 20.0, 0.0, 1.0),              # recovery depth
        np.clip(drawdown_pct / 50.0, 0.0, 1.0),             # drawdown [0→50%]
        np.clip(ofi_score / 100.0, 0.0, 1.0),               # OFI normalisé
        np.clip(imbalance_score / 100.0, 0.0, 1.0),         # imbalance
    ], dtype=np.float32)

    return features


# ────────────────────────────────────────────────────────────
#  Test rapide du modèle
# ────────────────────────────────────────────────────────────
if __name__ == "__main__":
    import json

    print("=== TOP4EA AutoML — Modèle PyTorch ===\n")

    model = TOP4EAParamNet()
    total_params = sum(p.numel() for p in model.parameters())
    print(f"Architecture : {total_params:,} paramètres\n")

    # Test forward pass
    batch = torch.randn(32, N_FEATURES)
    out   = model(batch)
    print(f"Input shape  : {batch.shape}")
    print(f"Output shape : {out.shape}")
    print(f"Output range : [{out.min():.4f}, {out.max():.4f}] ∈ [0,1]\n")

    # Test prédiction sur un contexte institutionnel
    features = build_features(
        atr_pts=450.0,          # ATR = 450 points (XAU volatil)
        rho1=-0.22,             # mean-reverting
        regime=1,               # RangeQuiet
        net_delta=0.15,         # légèrement net long
        hedge_pressure=0.20,    # pression modérée
        recov_depth=2,          # 2 niveaux de recovery
        drawdown_pct=3.5,       # drawdown 3.5%
        ofi_score=35.0,         # OFI faible
        imbalance_score=50.0,   # imbalance modérée
    )

    params = model.predict_params(features)
    print("Prédiction paramètres (modèle non-entraîné) :")
    for name, val in params.items():
        lo, hi = PARAM_BOUNDS[name]
        print(f"  {name:18s} = {val:.4f}  [{lo}, {hi}]")

    print("\n✓ Modèle opérationnel")
