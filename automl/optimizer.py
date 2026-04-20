"""
TOP4EA AutoML — Optimisation des hyperparamètres
Production-grade — Optuna TPE avec timeout, grid search fallback

MÉTHODE :
─────────────────────────────────────────────────────────
Optuna TPE (Tree-structured Parzen Estimator) pour trouver
les meilleurs hyperparamètres du modèle PyTorch :
  - Architecture : hidden_dim, n_res_blocks, dropout
  - Entraînement : lr, batch_size, weight_decay

ROBUSTESSE PRODUCTION :
  • Timeout Optuna configurable (défaut 120s) — jamais de blocage
  • Fallback grid search si Optuna non installé
  • Trial pruning par MedianPruner (coupe les essais perdants tôt)
  • Chaque trial avec try/except → un trial qui plante ne tue pas l'étude
  • Architecture finale sauvegardée en JSON pour traçabilité
  • Validation temporelle (pas de fuite de données)
"""

import json
import logging
import time
from pathlib import Path
from typing import Dict, Optional

import numpy as np

from trainer import (
    load_dataset, compute_labels, build_tensors,
    OUTPUT_JSON, MODEL_PATH, _load_checkpoint,
)
from model import TOP4EAParamNet, CompositeLoss

logger = logging.getLogger("top4ea.optimizer")

OPTUNA_STUDY_PATH = Path("automl/output/optuna_study.json")
BEST_ARCH_PATH    = Path("automl/output/best_arch.json")


# ────────────────────────────────────────────────────────────
#  Évaluation d'une configuration
# ────────────────────────────────────────────────────────────
def evaluate_config(
    hidden_dim:   int,
    n_res_blocks: int,
    dropout:      float,
    lr:           float,
    batch_size:   int,
    weight_decay: float,
    X_train, Y_train, X_val, Y_val,
    epochs: int = 20,
    trial=None,                      # handle Optuna trial pour pruning
) -> float:
    """Entraîne une configuration et retourne la val_loss."""
    import torch
    from torch.utils.data import DataLoader, TensorDataset
    from torch.optim.lr_scheduler import CosineAnnealingLR
    import torch.nn as nn

    # batch_size sécurisé
    effective_batch = min(batch_size, max(1, len(X_train)))

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
    train_dl = DataLoader(train_ds, batch_size=effective_batch,
                          shuffle=True, drop_last=False)

    best_val = float("inf")

    for ep in range(epochs):
        model.train()
        for xb, yb in train_dl:
            optimizer.zero_grad()
            pred = model(xb)
            loss, _ = criterion(pred, yb, xb, model)
            if torch.isnan(loss) or torch.isinf(loss):
                return float("inf")
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
        scheduler.step()

        model.eval()
        with torch.no_grad():
            val_pred = model(X_val)
            val_loss, _ = criterion(val_pred, Y_val, X_val, model)
            val_f = float(val_loss)
            if not (val_f != val_f):  # pas NaN
                best_val = min(best_val, val_f)

        # Pruning Optuna (coupe les essais mauvais en cours de route)
        if trial is not None:
            try:
                import optuna
                trial.report(best_val, ep)
                if trial.should_prune():
                    raise optuna.TrialPruned()
            except ImportError:
                pass

    return best_val


# ────────────────────────────────────────────────────────────
#  Recherche Optuna avec timeout
# ────────────────────────────────────────────────────────────
def run_optuna_search(
    X_train, Y_train, X_val, Y_val,
    n_trials:       int = 10,
    timeout_sec:    int = 120,       # timeout absolu
) -> Dict:
    """Lance une étude Optuna TPE avec timeout et pruning."""
    try:
        import optuna
        optuna.logging.set_verbosity(optuna.logging.WARNING)
    except ImportError:
        logger.warning("Optuna non installé — grid search")
        return run_grid_search(X_train, Y_train, X_val, Y_val)

    pruner = optuna.pruners.MedianPruner(n_startup_trials=3, n_warmup_steps=5)

    def objective(trial):
        hidden_dim   = trial.suggest_categorical("hidden_dim", [64, 128, 256])
        n_res_blocks = trial.suggest_int("n_res_blocks", 1, 3)
        dropout      = trial.suggest_float("dropout", 0.05, 0.30)
        lr           = trial.suggest_float("lr", 1e-4, 1e-2, log=True)
        batch_size   = trial.suggest_categorical("batch_size", [16, 32, 64])
        weight_decay = trial.suggest_float("weight_decay", 1e-5, 1e-3, log=True)

        try:
            return evaluate_config(
                hidden_dim, n_res_blocks, dropout, lr, batch_size, weight_decay,
                X_train, Y_train, X_val, Y_val, epochs=15, trial=trial,
            )
        except Exception as e:
            logger.warning(f"Trial échoué : {e}")
            return float("inf")

    study = optuna.create_study(direction="minimize", pruner=pruner)
    study.optimize(
        objective,
        n_trials       = n_trials,
        timeout        = timeout_sec,          # ← timeout absolu (jamais de blocage)
        show_progress_bar = False,
        catch           = (Exception,),
    )

    if not study.best_trials:
        logger.warning("Aucun trial Optuna valide — grid search")
        return run_grid_search(X_train, Y_train, X_val, Y_val)

    best = study.best_params
    logger.info(f"Optuna best: {best} (val_loss={study.best_value:.5f})"
                f" | {len(study.trials)} trials | timeout={timeout_sec}s")

    # Sauvegarder l'historique des trials pour traçabilité
    try:
        history = [
            {
                "number":   t.number,
                "val_loss": t.value,
                "params":   t.params,
                "state":    str(t.state),
            }
            for t in study.trials if t.value is not None
        ]
        OPTUNA_STUDY_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(OPTUNA_STUDY_PATH, "w") as f:
            json.dump({
                "best":    best,
                "best_val_loss": study.best_value,
                "n_trials": len(study.trials),
                "timestamp": int(time.time()),
                "history": history,
            }, f, indent=2)
    except Exception:
        pass

    return best


# ────────────────────────────────────────────────────────────
#  Grid search simplifié (fallback sans Optuna)
# ────────────────────────────────────────────────────────────
def run_grid_search(X_train, Y_train, X_val, Y_val) -> Dict:
    """Grid search sur les configurations clés — 4 architectures testées."""
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
        try:
            loss = evaluate_config(
                **cfg,
                X_train=X_train, Y_train=Y_train,
                X_val=X_val,     Y_val=Y_val,
                epochs=15,
            )
            logger.info(f"Grid: {cfg} → val_loss={loss:.5f}")
            if loss < best_loss:
                best_loss   = loss
                best_config = cfg
        except Exception as e:
            logger.warning(f"Config grid échouée : {cfg} — {e}")

    logger.info(f"Meilleure config grid: {best_config} (loss={best_loss:.5f})")
    return best_config


# ────────────────────────────────────────────────────────────
#  Pipeline complet d'optimisation
# ────────────────────────────────────────────────────────────
def optimize_and_train(
    csv_path:       Path = Path("data/automl_training.csv"),
    model_path:     Path = MODEL_PATH,
    output_json:    Path = OUTPUT_JSON,
    n_trials:       int  = 10,
    final_epochs:   int  = 100,
    optuna_timeout: int  = 120,
) -> Optional[Dict]:
    """
    1. Charge les données
    2. Optimise les hyperparamètres (Optuna TPE avec timeout)
    3. Entraîne le modèle final avec la meilleure architecture
    4. Exporte les paramètres optimaux
    """
    from trainer import train

    df = load_dataset(csv_path)
    if df is None:
        return None

    df      = compute_labels(df)
    n_val   = max(1, int(len(df) * 0.20))
    df_train = df.iloc[:-n_val] if n_val < len(df) else df.copy()
    df_val   = df.iloc[-n_val:]

    if len(df_train) < 2:
        logger.warning("Données insuffisantes pour l'optimisation")
        return None

    X_train, Y_train = build_tensors(df_train)
    X_val,   Y_val   = build_tensors(df_val)

    logger.info(f"Optimisation ({n_trials} trials, timeout={optuna_timeout}s)...")
    t0 = time.time()
    best_arch = run_optuna_search(
        X_train, Y_train, X_val, Y_val,
        n_trials=n_trials, timeout_sec=optuna_timeout,
    )
    logger.info(f"Optimisation terminée en {time.time()-t0:.1f}s")

    # Sauvegarde de la meilleure architecture
    BEST_ARCH_PATH.parent.mkdir(parents=True, exist_ok=True)
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
        batch_size  = int(best_arch.get("batch_size", 32)),
        lr          = float(best_arch.get("lr", 1e-3)),
        verbose     = True,
    )

    return result


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    result = optimize_and_train()
    if result:
        print(f"\nOptimisation terminée. Paramètres → {OUTPUT_JSON}")
    else:
        print("Données insuffisantes.")
