"""
TOP4EA AutoML — Optimisation des hyperparamètres avec Optuna

MÉTHODE :
─────────────────────────────────────────────────────────
Optuna utilise l'algorithme TPE (Tree-structured Parzen Estimator)
pour trouver les meilleurs hyperparamètres du modèle PyTorch :
  - Architecture (hidden_dim, n_res_blocks, dropout)
  - Entraînement (lr, batch_size, weight_decay)

Le critère optimisé est la loss de validation minimale
sur le jeu de données de trading historique.

Optuna est optionnel — si non installé, l'optimizer utilise
un grid search simplifié sans dépendances.
"""

import json
import logging
import time
from pathlib import Path
from typing import Dict, Optional

import numpy as np

from trainer import load_dataset, compute_labels, build_tensors, OUTPUT_JSON, MODEL_PATH
from model import TOP4EAParamNet, CompositeLoss

logger = logging.getLogger("top4ea.optimizer")

OPTUNA_STUDY_PATH = Path("automl/output/optuna_study.json")
BEST_ARCH_PATH    = Path("automl/output/best_arch.json")


# ────────────────────────────────────────────────────────────
#  Évaluation d'une configuration hyperparams
# ────────────────────────────────────────────────────────────
def evaluate_config(
    hidden_dim:   int,
    n_res_blocks: int,
    dropout:      float,
    lr:           float,
    batch_size:   int,
    weight_decay: float,
    X_train, Y_train, X_val, Y_val,
    epochs:       int = 20,
) -> float:
    """Entraîne et évalue une configuration — retourne la val_loss."""
    import torch
    from torch.utils.data import DataLoader, TensorDataset
    from torch.optim.lr_scheduler import CosineAnnealingLR

    model     = TOP4EAParamNet(
        hidden_dim   = hidden_dim,
        n_res_blocks = n_res_blocks,
        dropout      = dropout,
    )
    optimizer = torch.optim.AdamW(
        model.parameters(), lr=lr, weight_decay=weight_decay)
    scheduler = CosineAnnealingLR(optimizer, T_max=epochs, eta_min=lr * 0.01)
    criterion = CompositeLoss()

    train_ds = TensorDataset(X_train, Y_train)
    train_dl = DataLoader(train_ds, batch_size=batch_size,
                          shuffle=True, drop_last=False)

    best_val = float("inf")
    import torch.nn as nn

    for _ in range(epochs):
        model.train()
        for xb, yb in train_dl:
            optimizer.zero_grad()
            pred = model(xb)
            loss, _ = criterion(pred, yb, xb, model)
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
        scheduler.step()

        model.eval()
        with torch.no_grad():
            val_pred = model(X_val)
            val_loss, _ = criterion(val_pred, Y_val, X_val, model)
        best_val = min(best_val, float(val_loss))

    return best_val


# ────────────────────────────────────────────────────────────
#  Recherche Optuna (si disponible)
# ────────────────────────────────────────────────────────────
def run_optuna_search(
    X_train, Y_train, X_val, Y_val,
    n_trials: int = 20,
) -> Dict:
    """Lance une étude Optuna pour trouver les meilleurs hyperparams."""
    try:
        import optuna
        optuna.logging.set_verbosity(optuna.logging.WARNING)
    except ImportError:
        logger.warning("Optuna non installé — utilisation du grid search")
        return run_grid_search(X_train, Y_train, X_val, Y_val)

    def objective(trial):
        hidden_dim   = trial.suggest_categorical("hidden_dim", [64, 128, 256])
        n_res_blocks = trial.suggest_int("n_res_blocks", 1, 3)
        dropout      = trial.suggest_float("dropout", 0.05, 0.30)
        lr           = trial.suggest_float("lr", 1e-4, 1e-2, log=True)
        batch_size   = trial.suggest_categorical("batch_size", [16, 32, 64])
        weight_decay = trial.suggest_float("weight_decay", 1e-5, 1e-3, log=True)

        return evaluate_config(
            hidden_dim, n_res_blocks, dropout, lr, batch_size, weight_decay,
            X_train, Y_train, X_val, Y_val, epochs=15,
        )

    study = optuna.create_study(direction="minimize")
    study.optimize(objective, n_trials=n_trials, show_progress_bar=False)

    best = study.best_params
    logger.info(f"Optuna meilleurs params: {best} (val_loss={study.best_value:.5f})")

    return best


# ────────────────────────────────────────────────────────────
#  Grid search simplifié (sans Optuna)
# ────────────────────────────────────────────────────────────
def run_grid_search(X_train, Y_train, X_val, Y_val) -> Dict:
    """Grid search simple sur les hyperparamètres clés."""
    configs = [
        {"hidden_dim": 64,  "n_res_blocks": 1, "dropout": 0.10, "lr": 1e-3,
         "batch_size": 32,  "weight_decay": 1e-4},
        {"hidden_dim": 128, "n_res_blocks": 2, "dropout": 0.15, "lr": 5e-4,
         "batch_size": 32,  "weight_decay": 1e-4},
        {"hidden_dim": 128, "n_res_blocks": 1, "dropout": 0.10, "lr": 1e-3,
         "batch_size": 16,  "weight_decay": 1e-5},
        {"hidden_dim": 256, "n_res_blocks": 2, "dropout": 0.20, "lr": 3e-4,
         "batch_size": 64,  "weight_decay": 1e-4},
    ]

    best_loss   = float("inf")
    best_config = configs[0]

    for cfg in configs:
        loss = evaluate_config(
            **cfg, X_train=X_train, Y_train=Y_train,
            X_val=X_val, Y_val=Y_val, epochs=10,
        )
        logger.info(f"Config: {cfg} → val_loss={loss:.5f}")
        if loss < best_loss:
            best_loss   = loss
            best_config = cfg

    logger.info(f"Meilleure config: {best_config} (loss={best_loss:.5f})")
    return best_config


# ────────────────────────────────────────────────────────────
#  Pipeline complet d'optimisation
# ────────────────────────────────────────────────────────────
def optimize_and_train(
    csv_path:     Path = Path("data/automl_training.csv"),
    model_path:   Path = MODEL_PATH,
    output_json:  Path = OUTPUT_JSON,
    n_trials:     int  = 15,
    final_epochs: int  = 100,
) -> Optional[Dict]:
    """
    1. Charge les données
    2. Optimise les hyperparamètres
    3. Entraîne le modèle final
    4. Exporte les paramètres optimaux
    """
    import torch
    from trainer import train

    df = load_dataset(csv_path)
    if df is None:
        return None

    df = compute_labels(df)
    n_val = max(1, int(len(df) * 0.20))
    df_train = df.iloc[:-n_val] if n_val < len(df) else df
    df_val   = df.iloc[-n_val:]

    X_train, Y_train = build_tensors(df_train)
    X_val,   Y_val   = build_tensors(df_val)

    logger.info(f"Lancement optimisation hyperparams ({n_trials} essais)...")
    best_arch = run_optuna_search(X_train, Y_train, X_val, Y_val, n_trials)

    # Sauvegarder la meilleure architecture
    with open(BEST_ARCH_PATH, "w") as f:
        json.dump(best_arch, f, indent=2)
    logger.info(f"Architecture sauvegardée : {BEST_ARCH_PATH}")

    # Entraînement final avec les meilleurs hyperparams
    logger.info(f"Entraînement final ({final_epochs} époques)...")
    result = train(
        csv_path    = csv_path,
        model_path  = model_path,
        output_json = output_json,
        epochs      = final_epochs,
        batch_size  = best_arch.get("batch_size", 32),
        lr          = best_arch.get("lr", 1e-3),
        verbose     = True,
    )

    return result


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    result = optimize_and_train()
    if result:
        print(f"\nOptimisation terminée. Paramètres exportés vers {OUTPUT_JSON}")
    else:
        print("Données insuffisantes.")
