"""
TOP4EA AutoML — Entraîneur PyTorch
Production-grade — robuste, long terme, sans fuite temporelle

PIPELINE D'ENTRAÎNEMENT :
─────────────────────────────────────────────────────────
1. Chargement CSV avec validation schéma et nettoyage des lignes invalides
2. Feature engineering + normalisation
3. Construction du label supervisé depuis les données réelles
   → Cycles "optimaux" : top 30% par Sharpe proxy (pnl / max_dd)
   → Labels = paramètres utilisés dans ces cycles gagnants
4. Séparation temporelle (pas de lookahead) : val = derniers 20%
5. Entraînement AdamW + CosineAnnealing + early stopping
6. Checkpoint intermédiaire toutes les CHECKPOINT_EVERY époques
7. Export JSON atomique des paramètres optimaux

ROBUSTESSE :
  • Colonnes manquantes → fallback valeurs par défaut (pas de crash)
  • batch_size=1 safe : BatchNorm.eval() en validation
  • weights_only=True sur torch.load (sécurité PyTorch 2.x)
  • Checkpoint intermédiaire : jamais de tout-ou-rien
  • val_loss NaN/inf → ignoré, pas de crash
  • labels toujours dans les bornes PARAM_BOUNDS
"""

import os
import json
import time
import logging
import tempfile
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
    PARAM_NAMES, PARAM_BOUNDS, N_FEATURES,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger("top4ea.trainer")

# ── Chemins par défaut ────────────────────────────────────────
TRAINING_CSV   = Path("data/automl_training.csv")
CHECKPOINT_DIR = Path("automl/checkpoints")
OUTPUT_JSON    = Path("automl/output/params.json")
MODEL_PATH     = CHECKPOINT_DIR / "top4ea_model.pt"
CHECKPOINT_EVERY = 10           # sauvegarde intermédiaire toutes les 10 epochs

CHECKPOINT_DIR.mkdir(parents=True, exist_ok=True)
Path("automl/output").mkdir(parents=True, exist_ok=True)

# ── Colonnes CSV et leurs valeurs par défaut ──────────────────
CSV_DEFAULTS: Dict[str, float] = {
    "atr_pts":         500.0,
    "rho1":            0.0,
    "regime":          1.0,
    "net_delta":       0.0,
    "hedge_pressure":  0.0,
    "recov_depth":     0.0,
    "drawdown_pct":    0.0,
    "ofi_score":       0.0,
    "imbalance_score": 0.0,
    "pnl_result":      0.0,
    "atr_mult_used":   1.0,
    "kelly_frac_used": 0.25,
    "gap_exp_used":    0.05,
    "trail_asym_used": 0.15,
}


# ────────────────────────────────────────────────────────────
#  Chargement et nettoyage du dataset
# ────────────────────────────────────────────────────────────
def load_dataset(csv_path: Path = TRAINING_CSV) -> Optional[pd.DataFrame]:
    """
    Charge le CSV avec validation schéma et nettoyage des lignes invalides.
    Tolérant aux colonnes manquantes : fallback vers CSV_DEFAULTS.
    """
    if not csv_path.exists():
        logger.warning(f"CSV introuvable : {csv_path}")
        return None

    try:
        df = pd.read_csv(csv_path, on_bad_lines="skip")
    except Exception as e:
        logger.error(f"Lecture CSV échouée : {e}")
        return None

    if len(df) < 20:
        logger.warning(f"Données insuffisantes : {len(df)} lignes (min 20)")
        return None

    # Ajouter les colonnes manquantes avec leurs valeurs par défaut
    missing = []
    for col, default in CSV_DEFAULTS.items():
        if col not in df.columns:
            df[col] = default
            missing.append(col)
    if missing:
        logger.info(f"Colonnes absentes du CSV — valeurs par défaut : {missing}")

    # Convertir en numérique, remplacer les NaN par les défauts
    for col, default in CSV_DEFAULTS.items():
        df[col] = pd.to_numeric(df[col], errors="coerce").fillna(default)

    # Supprimer les lignes entièrement NaN
    df = df.dropna(how="all")
    df = df.reset_index(drop=True)

    logger.info(f"Dataset : {len(df)} cycles valides | colonnes : {list(df.columns)}")
    return df


def compute_labels(df: pd.DataFrame,
                   sharpe_threshold: float | None = None) -> pd.DataFrame:
    """
    Construit les labels supervisés depuis les cycles réels.

    Stratégie :
    Les cycles du TOP 30% par Sharpe proxy (pnl / (drawdown + 1)) sont
    considérés comme "optimaux". Les paramètres utilisés dans ces cycles
    deviennent les cibles de régression (interpolation pour les autres).

    FIX LOOKAHEAD BIAS :
    Le paramètre sharpe_threshold DOIT être calculé uniquement sur les
    données d'entraînement (df_train) avant d'appeler cette fonction sur
    l'ensemble du dataset. Si None, il est calculé sur df lui-même
    (usage interne ou mode autonome uniquement).
    """
    df = df.copy()

    # Sharpe proxy robuste (drawdown = 0 → dénominateur = 1)
    df["sharpe_proxy"] = df["pnl_result"] / (df["drawdown_pct"].abs() + 1.0)

    # Utiliser le seuil fourni (calculé sur train) ou en calculer un local
    if sharpe_threshold is None:
        # Fallback : pas de lookahead connu — calculer sur df courant
        threshold = df["sharpe_proxy"].quantile(0.70)
        if threshold == df["sharpe_proxy"].quantile(0.0):
            threshold = df["sharpe_proxy"].median()
    else:
        threshold = sharpe_threshold

    df["is_optimal"] = df["sharpe_proxy"] >= threshold

    optimal_mask = df["is_optimal"]

    # Paramètres trackés dans le CSV
    param_map = {
        "atr_mult":      "atr_mult_used",
        "kelly_frac":    "kelly_frac_used",
        "gap_expansion": "gap_exp_used",
        "trail_asym":    "trail_asym_used",
    }

    # Moyennes des cycles optimaux pour chaque paramètre
    optimal_means = {}
    for param, col in param_map.items():
        lo, hi = PARAM_BOUNDS[param]
        optimal_means[param] = float(
            np.clip(df[optimal_mask][col].mean(), lo, hi)
            if optimal_mask.any() else (lo + hi) / 2.0
        )
    logger.info(f"Paramètres optimaux moyens : {optimal_means}")

    # Labels interpolés : cycles optimaux → leurs propres params ;
    # autres → interpolation vers la moyenne optimale selon le Sharpe ratio
    for param, col in param_map.items():
        lo, hi = PARAM_BOUNDS[param]
        perf_ratio = np.clip(df["sharpe_proxy"] / (threshold + 1e-9), 0.0, 1.0)
        raw = df[col] * perf_ratio + optimal_means[param] * (1.0 - perf_ratio)
        df[f"label_{param}"] = np.clip(raw, lo, hi)

    # Paramètres non trackés — heuristiques cohérentes avec la stratégie :
    # vel_trigger : plus bas quand drawdown ↑ (fermeture plus rapide)
    df["label_vel_trigger"] = np.clip(
        0.88 - 0.08 * np.clip(df["drawdown_pct"] / 20.0, 0.0, 1.0),
        *PARAM_BOUNDS["vel_trigger"]
    )
    # cppi_mult : plus élevé quand recovery profond (protection capital)
    df["label_cppi_mult"] = np.clip(
        2.5 + np.clip(df["recov_depth"] / 5.0, 0.0, 1.0) * 2.0,
        *PARAM_BOUNDS["cppi_mult"]
    )
    # vabt_coeff : proportionnel à l'OFI score normalisé
    df["label_vabt_coeff"] = np.clip(
        0.10 + 0.20 * np.clip(df["ofi_score"] / 100.0, 0.0, 1.0),
        *PARAM_BOUNDS["vabt_coeff"]
    )
    # ou_theta : mean-reversion plus fort si rho1 négatif fort
    df["label_ou_theta"] = np.clip(
        0.05 + 0.10 * np.clip((-df["rho1"]).clip(0.0), 0.0, 1.0),
        *PARAM_BOUNDS["ou_theta"]
    )

    return df


def build_tensors(df: pd.DataFrame) -> Tuple[torch.Tensor, torch.Tensor]:
    """Construit les tenseurs features (X) et targets (Y) depuis le DataFrame."""
    label_cols = [
        "label_atr_mult", "label_kelly_frac", "label_gap_expansion",
        "label_trail_asym", "label_vel_trigger", "label_cppi_mult",
        "label_vabt_coeff", "label_ou_theta",
    ]
    param_bounds_list = [PARAM_BOUNDS[p] for p in PARAM_NAMES]

    X_list, Y_list = [], []
    for _, row in df.iterrows():
        features = build_features(
            atr_pts         = float(row["atr_pts"]),
            rho1            = float(row["rho1"]),
            regime          = int(row["regime"]),
            net_delta       = float(row["net_delta"]),
            hedge_pressure  = float(row["hedge_pressure"]),
            recov_depth     = int(row["recov_depth"]),
            drawdown_pct    = float(row["drawdown_pct"]),
            ofi_score       = float(row["ofi_score"]),
            imbalance_score = float(row["imbalance_score"]),
        )
        X_list.append(features)

        labels_norm = []
        for col, (lo, hi) in zip(label_cols, param_bounds_list):
            val = float(row.get(col, (lo + hi) / 2.0))
            val_norm = np.clip((val - lo) / (hi - lo + 1e-9), 0.0, 1.0)
            labels_norm.append(val_norm)
        Y_list.append(labels_norm)

    X = torch.FloatTensor(np.array(X_list, dtype=np.float32))
    Y = torch.FloatTensor(np.array(Y_list, dtype=np.float32))
    return X, Y


# ────────────────────────────────────────────────────────────
#  Chargement sécurisé du checkpoint
# ────────────────────────────────────────────────────────────
def _atomic_torch_save(state: dict, dest: Path) -> None:
    """
    FIX ÉCRITURE ATOMIQUE : enregistre un state_dict PyTorch en deux étapes.
      1. Écriture dans un fichier temporaire (même répertoire).
      2. os.replace() atomique → le moteur C++ ne lira jamais un fichier partiel.
    Le fichier temporaire est dans le même répertoire que dest pour garantir
    que le rename ne traverse pas de systèmes de fichiers (sinon non-atomique).
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(dir=dest.parent, suffix=".tmp")
    try:
        os.close(fd)
        torch.save(state, tmp_path)
        os.replace(tmp_path, dest)
    except Exception:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise


def _load_checkpoint(model: nn.Module, path: Path) -> bool:
    """
    Charge un checkpoint avec weights_only=True (sécurité PyTorch 2.x).
    Essaie en mode strict puis non-strict si le schéma a changé.
    """
    if not path.exists():
        return False
    try:
        state = torch.load(path, map_location="cpu", weights_only=True)
        try:
            model.load_state_dict(state, strict=True)
            logger.info(f"Checkpoint chargé (strict) : {path}")
        except RuntimeError:
            model.load_state_dict(state, strict=False)
            logger.info(f"Checkpoint chargé (non-strict, schéma partiel) : {path}")
        return True
    except Exception as e:
        logger.warning(f"Impossible de charger le checkpoint : {e}")
        return False


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

    # FIX LOOKAHEAD BIAS : séparation temporelle AVANT le calcul des labels.
    # Le seuil Sharpe est calculé sur df_train uniquement, puis appliqué à
    # l'ensemble pour que la validation ne "voit" pas les futurs cycles.
    n_val    = max(1, int(len(df) * val_split))
    df_train_raw = df.iloc[:-n_val] if n_val < len(df) else df.copy()
    df_val_raw   = df.iloc[-n_val:]

    # FIX BATCH_SIZE=1 / BatchNorm crash : BatchNorm1d plante si un batch ne
    # contient qu'un seul échantillon en mode train (variance indéfinie).
    # Garantir au moins 2 échantillons dans le dataset d'entraînement.
    if len(df_train_raw) < 2:
        logger.warning("Dataset trop petit pour BatchNorm1d (< 2 échantillons train) — cycle ignoré")
        return None

    # Calcul du seuil sur train uniquement (pas de lookahead)
    sharpe_train = df_train_raw["pnl_result"] / (df_train_raw["drawdown_pct"].abs() + 1.0)
    threshold_train = float(sharpe_train.quantile(0.70))
    if threshold_train == float(sharpe_train.quantile(0.0)):
        threshold_train = float(sharpe_train.median())

    df_train = compute_labels(df_train_raw, sharpe_threshold=threshold_train)
    df_val   = compute_labels(df_val_raw,   sharpe_threshold=threshold_train)

    X_train, Y_train = build_tensors(df_train)
    X_val,   Y_val   = build_tensors(df_val)
    logger.info(f"Train: {len(X_train)} | Val: {len(X_val)} | Sharpe threshold (train): {threshold_train:.4f}")

    # FIX BATCH_SIZE=1 / BatchNorm crash :
    # build_tensors peut filtrer des lignes NaN → vérifier à nouveau après conversion.
    if len(X_train) < 2:
        logger.warning("Tenseurs train insuffisants après build_tensors (< 2) — cycle ignoré")
        return None

    # effective_batch >= 2 : évite tout batch de taille 1 en mode train.
    # Couplé à drop_last=True dans DataLoader, garantit que chaque batch
    # a exactement effective_batch échantillons → BatchNorm1d ne plante jamais.
    effective_batch = max(2, min(batch_size, len(X_train)))

    model     = TOP4EAParamNet()
    _load_checkpoint(model, model_path)

    optimizer = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    # T_max = epochs → lr descend jusqu'à lr*0.01 sur toute la durée
    scheduler = CosineAnnealingLR(optimizer, T_max=epochs, eta_min=lr * 0.01)
    criterion = CompositeLoss()

    train_ds = TensorDataset(X_train, Y_train)
    # FIX BUG 6 — drop_last=True : le dernier mini-batch est éliminé s'il a
    # moins de effective_batch échantillons, ce qui garantit que BatchNorm1d
    # ne reçoit JAMAIS un batch de taille 1 (variance indéfinie → crash).
    # drop_last peut réduire marginalement l'utilisation des données, mais
    # effective_batch >= 2 et len(X_train) >= 2 assurent au moins 1 batch complet.
    train_dl = DataLoader(train_ds, batch_size=effective_batch,
                          shuffle=True, drop_last=True)

    best_val_loss = float("inf")
    best_state    = None
    # Patience = 15% des epochs pour laisser CosineAnnealing converger
    patience   = max(10, int(epochs * 0.15))
    no_improve = 0
    epoch      = 0

    for epoch in range(1, epochs + 1):
        model.train()
        epoch_losses = []

        for xb, yb in train_dl:
            optimizer.zero_grad()
            pred = model(xb)
            loss, _ = criterion(pred, yb, xb, model)
            if torch.isnan(loss) or torch.isinf(loss):
                logger.warning(f"Loss NaN/inf à l'epoch {epoch} — batch ignoré")
                continue
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            epoch_losses.append(float(loss))

        scheduler.step()

        # Validation (BatchNorm en mode eval = stats globales)
        model.eval()
        with torch.no_grad():
            val_pred = model(X_val)
            val_loss, _ = criterion(val_pred, Y_val, X_val, model)
            val_loss_f  = float(val_loss)

        if not (val_loss_f != val_loss_f) and val_loss_f < best_val_loss:
            best_val_loss = val_loss_f
            best_state    = {k: v.clone() for k, v in model.state_dict().items()}
            no_improve    = 0
        else:
            no_improve += 1

        if verbose and epoch % 10 == 0:
            train_avg = float(np.mean(epoch_losses)) if epoch_losses else float("nan")
            logger.info(
                f"Epoch {epoch:3d}/{epochs} | "
                f"train={train_avg:.5f} | val={val_loss_f:.5f} | "
                f"lr={scheduler.get_last_lr()[0]:.6f} | "
                f"best={best_val_loss:.5f}")

        # Checkpoint intermédiaire (sauvegarde toutes les N epochs)
        if epoch % CHECKPOINT_EVERY == 0 and best_state is not None:
            tmp_state = model.state_dict()
            model.load_state_dict(best_state)
            _atomic_torch_save(best_state, model_path)  # FIX BUG 7 : écriture atomique
            model.load_state_dict(tmp_state)

        if no_improve >= patience:
            logger.info(f"Early stopping epoch {epoch} (no_improve={no_improve})")
            break

    # Restaurer le meilleur modèle et sauvegarder définitivement
    if best_state is not None:
        model.load_state_dict(best_state)
    _atomic_torch_save(model.state_dict(), model_path)  # FIX BUG 7 : écriture atomique
    logger.info(f"Modèle sauvegardé : {model_path} (val_loss={best_val_loss:.5f})")

    # ── Prédiction sur le contexte le plus récent ─────────────
    model.eval()
    last_row = df.iloc[-1]
    features = build_features(
        atr_pts         = float(last_row["atr_pts"]),
        rho1            = float(last_row["rho1"]),
        regime          = int(last_row["regime"]),
        net_delta       = float(last_row["net_delta"]),
        hedge_pressure  = float(last_row["hedge_pressure"]),
        recov_depth     = int(last_row["recov_depth"]),
        drawdown_pct    = float(last_row["drawdown_pct"]),
        ofi_score       = float(last_row["ofi_score"]),
        imbalance_score = float(last_row["imbalance_score"]),
    )

    predicted = model.predict_params(features)
    logger.info(f"Paramètres prédits : {predicted}")

    # ── Export JSON (écriture directe ici; atomique dans automl_server) ──
    output = {k: round(v, 4) for k, v in predicted.items()}
    output["training_meta"] = {
        "n_cycles":      len(df),
        "best_val_loss": round(best_val_loss, 6),
        "timestamp":     int(time.time()),
        "epochs_run":    epoch,
    }

    output_json.parent.mkdir(parents=True, exist_ok=True)
    with open(output_json, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2)
    logger.info(f"JSON exporté : {output_json}")

    return output


# ────────────────────────────────────────────────────────────
#  Prédiction seule (sans ré-entraînement)
# ────────────────────────────────────────────────────────────
def predict_current(
    context: Dict,
    model_path:  Path = MODEL_PATH,
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
    if not _load_checkpoint(model, model_path):
        return None

    features  = build_features(**context)
    predicted = model.predict_params(features)

    output = {k: round(v, 4) for k, v in predicted.items()}
    output["timestamp"] = int(time.time())

    output_json.parent.mkdir(parents=True, exist_ok=True)
    with open(output_json, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2)

    return output


if __name__ == "__main__":
    logger.info("Lancement entraînement TOP4EA AutoML...")
    result = train(verbose=True)
    if result:
        logger.info("Entraînement terminé avec succès")
        meta = result.get("training_meta", {})
        logger.info(f"Meilleurs params | val_loss={meta.get('best_val_loss'):.5f}"
                    f" | epochs={meta.get('epochs_run')}")
    else:
        logger.warning("Données insuffisantes — entraînement ignoré")
