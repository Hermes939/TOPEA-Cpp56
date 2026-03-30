"""
TOP4EA AutoML — Entraîneur PyTorch

PIPELINE D'ENTRAÎNEMENT :
─────────────────────────────────────────────────────────
1. Chargement du CSV de trading historique (exporté par C++)
2. Feature engineering + normalisation
3. Construction du label supervisé depuis les données réelles
   → Label : paramètres qui auraient donné le MEILLEUR P&L
   → Méthode : régression sur les paramètres utilisés dans
     les cycles gagnants (top 30% par P&L ajusté au risque)
4. Entraînement avec validation temporelle (pas de lookahead)
5. Sauvegarde du modèle + export JSON des meilleurs paramètres
"""

import os
import json
import time
import logging
import numpy as np
import pandas as pd
import torch
import torch.nn as nn
from torch.utils.data import DataLoader, TensorDataset
from torch.optim.lr_scheduler import CosineAnnealingLR
from pathlib import Path
from typing import Optional, Dict, Tuple

from model import (
    TOP4EAParamNet, CompositeLoss,
    build_features, denormalize_params, normalize_param,
    PARAM_NAMES, N_FEATURES,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger("top4ea.trainer")


# ────────────────────────────────────────────────────────────
#  Chemins par défaut
# ────────────────────────────────────────────────────────────
TRAINING_CSV   = Path("data/automl_training.csv")
CHECKPOINT_DIR = Path("automl/checkpoints")
OUTPUT_JSON    = Path("automl/output/params.json")
MODEL_PATH     = Path("automl/checkpoints/top4ea_model.pt")

CHECKPOINT_DIR.mkdir(parents=True, exist_ok=True)
Path("automl/output").mkdir(parents=True, exist_ok=True)


# ────────────────────────────────────────────────────────────
#  Chargement et préparation du dataset
# ────────────────────────────────────────────────────────────
def load_dataset(csv_path: Path = TRAINING_CSV) -> Optional[pd.DataFrame]:
    """Charge le CSV de trading et le prépare pour l'entraînement."""
    if not csv_path.exists():
        logger.warning(f"CSV d'entraînement introuvable : {csv_path}")
        return None

    df = pd.read_csv(csv_path)
    if len(df) < 20:
        logger.warning(f"Données insuffisantes : {len(df)} lignes (min 20)")
        return None

    logger.info(f"Dataset chargé : {len(df)} cycles | colonnes : {list(df.columns)}")
    return df


def compute_labels(df: pd.DataFrame) -> pd.DataFrame:
    """
    Construit les labels supervisés depuis les cycles réels.

    Stratégie :
    Les cycles du TOP 30% par Sharpe proxy (pnl / max_dd) sont
    considérés comme des cycles "optimaux". Les paramètres utilisés
    dans ces cycles deviennent les cibles de régression.

    Pour les cycles NON-optimaux, les paramètres sont ajustés
    proportionnellement à l'écart de performance.
    """
    df = df.copy()

    # Sharpe proxy : P&L / (drawdown + 1)
    df["sharpe_proxy"] = df["pnl_result"] / (df["drawdown_pct"].abs() + 1.0)

    # Threshold : top 30% des cycles
    threshold = df["sharpe_proxy"].quantile(0.70)
    df["is_optimal"] = df["sharpe_proxy"] >= threshold

    # Labels = paramètres utilisés dans les cycles optimaux
    # Pour les autres cycles, on prend les paramètres moyens des optimaux
    optimal_mask = df["is_optimal"]

    param_cols = [
        "atr_mult_used", "kelly_frac_used",
        "gap_exp_used", "trail_asym_used"
    ]

    # Paramètres moyens des cycles optimaux
    optimal_means = df[optimal_mask][param_cols].mean().to_dict()
    logger.info(f"Paramètres optimaux moyens : {optimal_means}")

    # Créer les labels
    for i, p in enumerate(["atr_mult", "kelly_frac", "gap_expansion", "trail_asym"]):
        col_used = param_cols[i]
        # Pour les cycles optimaux : garder les paramètres utilisés
        # Pour les autres : interpoler vers la moyenne optimale
        perf_ratio = np.clip(
            df["sharpe_proxy"] / (threshold + 1e-9), 0.0, 1.0)
        col_mean = optimal_means[col_used]
        df[f"label_{p}"] = (
            df[col_used] * perf_ratio + col_mean * (1 - perf_ratio)
        )

    # Ajouter les paramètres non trackés avec des valeurs par défaut sensées
    df["label_vel_trigger"] = 0.85 - 0.05 * np.clip(
        df["drawdown_pct"] / 20.0, 0.0, 1.0)
    df["label_cppi_mult"]   = 3.0 + np.clip(df["recov_depth"] / 10.0, 0.0, 1.5)
    df["label_vabt_coeff"]  = 0.15
    df["label_ou_theta"]    = 0.05 + 0.05 * np.clip(
        df.get("rho1", pd.Series(0.0, index=df.index)).abs(), 0.0, 1.0)

    return df


def build_tensors(df: pd.DataFrame) -> Tuple[torch.Tensor, torch.Tensor]:
    """Construit les tenseurs features et targets depuis le DataFrame."""
    X_list = []
    Y_list = []

    label_cols = [
        "label_atr_mult", "label_kelly_frac", "label_gap_expansion",
        "label_trail_asym", "label_vel_trigger", "label_cppi_mult",
        "label_vabt_coeff", "label_ou_theta",
    ]

    param_bounds_list = [
        (0.20, 2.00), (0.05, 0.50), (0.01, 0.30),
        (0.05, 0.40), (0.60, 0.98), (1.50, 6.00),
        (0.05, 0.50), (0.01, 0.30),
    ]

    for _, row in df.iterrows():
        # Features
        features = build_features(
            atr_pts         = float(row.get("atr_pts",         500.0)),
            rho1            = float(row.get("rho1",            0.0)),
            regime          = int(row.get("regime",            1)),
            net_delta       = float(row.get("net_delta",       0.0)),
            hedge_pressure  = float(row.get("hedge_pressure",  0.0)),
            recov_depth     = int(row.get("recov_depth",       0)),
            drawdown_pct    = float(row.get("drawdown_pct",    0.0)),
            ofi_score       = float(row.get("ofi_score",       0.0)),
            imbalance_score = float(row.get("imbalance_score", 0.0)),
        )
        X_list.append(features)

        # Targets normalisés ∈ [0,1]
        labels_norm = []
        for col, (lo, hi) in zip(label_cols, param_bounds_list):
            val = float(row.get(col, (lo + hi) / 2.0))
            val_norm = (np.clip(val, lo, hi) - lo) / (hi - lo)
            labels_norm.append(val_norm)
        Y_list.append(labels_norm)

    X = torch.FloatTensor(np.array(X_list))
    Y = torch.FloatTensor(np.array(Y_list))

    return X, Y


# ────────────────────────────────────────────────────────────
#  Entraînement principal
# ────────────────────────────────────────────────────────────
def train(
    csv_path:    Path = TRAINING_CSV,
    model_path:  Path = MODEL_PATH,
    output_json: Path = OUTPUT_JSON,
    epochs:      int  = 50,
    batch_size:  int  = 32,
    lr:          float = 1e-3,
    val_split:   float = 0.20,
    verbose:     bool  = True,
) -> Optional[Dict]:
    """
    Entraîne le modèle sur les données historiques.

    Retourne les meilleurs paramètres prédits pour le contexte actuel,
    ou None si l'entraînement a échoué.
    """
    df = load_dataset(csv_path)
    if df is None:
        return None

    df = compute_labels(df)

    # Séparation temporelle (pas de lookahead) : val = derniers 20%
    n_val   = max(1, int(len(df) * val_split))
    df_train= df.iloc[:-n_val] if n_val < len(df) else df
    df_val  = df.iloc[-n_val:]

    X_train, Y_train = build_tensors(df_train)
    X_val,   Y_val   = build_tensors(df_val)

    logger.info(f"Train: {len(X_train)} | Val: {len(X_val)}")

    # Chargement ou création du modèle
    model = TOP4EAParamNet()
    if model_path.exists():
        try:
            state = torch.load(model_path, map_location="cpu")
            model.load_state_dict(state, strict=False)
            logger.info(f"Checkpoint chargé : {model_path}")
        except Exception as e:
            logger.warning(f"Impossible de charger le checkpoint : {e}")

    optimizer = torch.optim.AdamW(
        model.parameters(), lr=lr, weight_decay=1e-4)
    scheduler = CosineAnnealingLR(optimizer, T_max=epochs, eta_min=lr * 0.01)
    criterion = CompositeLoss()

    train_ds = TensorDataset(X_train, Y_train)
    train_dl = DataLoader(train_ds, batch_size=batch_size, shuffle=True,
                          drop_last=False)

    best_val_loss = float("inf")
    best_state    = None
    patience      = 10
    no_improve    = 0

    for epoch in range(1, epochs + 1):
        model.train()
        epoch_losses = []

        for xb, yb in train_dl:
            optimizer.zero_grad()
            pred = model(xb)
            loss, loss_dict = criterion(pred, yb, xb, model)
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            epoch_losses.append(float(loss))

        scheduler.step()

        # Validation
        model.eval()
        with torch.no_grad():
            val_pred = model(X_val)
            val_loss, _ = criterion(val_pred, Y_val, X_val, model)
            val_loss_f = float(val_loss)

        if val_loss_f < best_val_loss:
            best_val_loss = val_loss_f
            best_state    = {k: v.clone() for k, v in model.state_dict().items()}
            no_improve    = 0
        else:
            no_improve += 1

        if verbose and epoch % 10 == 0:
            train_avg = float(np.mean(epoch_losses))
            logger.info(
                f"Epoch {epoch:3d}/{epochs} | "
                f"train={train_avg:.5f} | val={val_loss_f:.5f} | "
                f"lr={scheduler.get_last_lr()[0]:.6f}")

        if no_improve >= patience:
            logger.info(f"Early stopping à l'époque {epoch}")
            break

    # Restaurer le meilleur modèle
    if best_state is not None:
        model.load_state_dict(best_state)

    # Sauvegarder le checkpoint
    torch.save(model.state_dict(), model_path)
    logger.info(f"Modèle sauvegardé : {model_path} (val_loss={best_val_loss:.5f})")

    # ── Prédiction sur le contexte récent (dernière ligne) ────
    model.eval()
    last_row = df.iloc[-1]
    features = build_features(
        atr_pts         = float(last_row.get("atr_pts",         500.0)),
        rho1            = float(last_row.get("rho1",            0.0)),
        regime          = int(last_row.get("regime",            1)),
        net_delta       = float(last_row.get("net_delta",       0.0)),
        hedge_pressure  = float(last_row.get("hedge_pressure",  0.0)),
        recov_depth     = int(last_row.get("recov_depth",       0)),
        drawdown_pct    = float(last_row.get("drawdown_pct",    0.0)),
        ofi_score       = float(last_row.get("ofi_score",       0.0)),
        imbalance_score = float(last_row.get("imbalance_score", 0.0)),
    )

    predicted = model.predict_params(features)
    logger.info(f"Paramètres prédits : {predicted}")

    # ── Export JSON pour le moteur C++ ────────────────────────
    output = {
        "atr_mult":      round(predicted["atr_mult"],      4),
        "kelly_frac":    round(predicted["kelly_frac"],    4),
        "gap_expansion": round(predicted["gap_expansion"], 4),
        "trail_asym":    round(predicted["trail_asym"],    4),
        "vel_trigger":   round(predicted["vel_trigger"],   4),
        "cppi_mult":     round(predicted["cppi_mult"],     4),
        "vabt_coeff":    round(predicted["vabt_coeff"],    4),
        "ou_theta":      round(predicted["ou_theta"],      4),
        "training_meta": {
            "n_cycles":    len(df),
            "best_val_loss": round(best_val_loss, 6),
            "timestamp":   int(time.time()),
            "epochs_run":  epoch,
        }
    }

    with open(output_json, "w") as f:
        json.dump(output, f, indent=2)
    logger.info(f"JSON exporté : {output_json}")

    return output


# ────────────────────────────────────────────────────────────
#  Prédiction seule (sans ré-entraînement)
# ────────────────────────────────────────────────────────────
def predict_current(
    context: Dict,
    model_path: Path = MODEL_PATH,
    output_json: Path = OUTPUT_JSON,
) -> Optional[Dict]:
    """
    Prédit les paramètres optimaux pour un contexte donné,
    sans ré-entraînement.
    """
    if not model_path.exists():
        logger.warning("Aucun modèle entraîné disponible")
        return None

    model = TOP4EAParamNet()
    state = torch.load(model_path, map_location="cpu")
    model.load_state_dict(state, strict=False)

    features = build_features(**context)
    predicted = model.predict_params(features)

    output = {k: round(v, 4) for k, v in predicted.items()}
    output["timestamp"] = int(time.time())

    with open(output_json, "w") as f:
        json.dump(output, f, indent=2)

    return output


if __name__ == "__main__":
    logger.info("Lancement de l'entraînement TOP4EA AutoML...")
    result = train(verbose=True)
    if result:
        logger.info("Entraînement terminé avec succès")
        logger.info(f"Meilleurs paramètres : {result}")
    else:
        logger.warning("Données insuffisantes — entraînement ignoré")
