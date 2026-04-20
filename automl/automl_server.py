"""
TOP4EA AutoML — Serveur d'auto-entraînement continu

FONCTIONNEMENT :
─────────────────────────────────────────────────────────
Ce serveur s'exécute en arrière-plan et surveille les données
de trading exportées par le moteur C++.

CYCLE D'AUTO-ENTRAÎNEMENT :
  1. Surveillance du fichier data/automl_training.csv
  2. Si ≥ N nouveaux cycles depuis le dernier entraînement
     → Lancer l'entraînement complet (trainer.py)
  3. Si modèle déjà entraîné + nouveau contexte disponible
     → Lancer uniquement la prédiction (trainer.predict_current)
  4. Mise à jour du fichier automl/output/params.json
     → Lu par le moteur C++ toutes les 5 minutes

MODES :
  --mode train     : entraînement complet immédiat
  --mode predict   : prédiction seule (modèle existant)
  --mode optimize  : optimisation hyperparams + entraînement
  --mode watch     : surveillance continue (défaut)

SEUILS DE DÉCLENCHEMENT :
  MIN_NEW_CYCLES  = 10   : minimum de nouveaux cycles pour ré-entraîner
  MAX_AGE_MINUTES = 60   : ré-entraînement forcé après 60 min
"""

import os
import sys
import time
import json
import argparse
import logging
import hashlib
from pathlib import Path
from typing import Optional
from datetime import datetime

# Ajouter le répertoire du script au PYTHONPATH pour les imports relatifs
# (permet de lancer depuis n'importe quel répertoire courant)
sys.path.insert(0, str(Path(__file__).parent))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s — %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler("data/automl_server.log", mode="a"),
    ]
)
logger = logging.getLogger("top4ea.automl_server")

# ── Chemins ────────────────────────────────────────────────
TRAINING_CSV  = Path("data/automl_training.csv")
OUTPUT_JSON   = Path("automl/output/params.json")
STATUS_FILE   = Path("automl/output/status.json")
MODEL_PATH    = Path("automl/checkpoints/top4ea_model.pt")

# ── Paramètres du serveur ──────────────────────────────────
MIN_NEW_CYCLES    = 10      # cycles minimum avant ré-entraînement
MAX_AGE_MINUTES   = 60      # ré-entraînement forcé si > 60 min
WATCH_INTERVAL_S  = 30      # intervalle de surveillance (secondes)
TRAIN_EPOCHS      = 50      # époques d'entraînement normal
FULL_EPOCHS       = 100     # époques entraînement complet (optimisé)


def get_file_hash(path: Path) -> str:
    """Hash MD5 du fichier pour détecter les modifications."""
    if not path.exists():
        return ""
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def count_csv_lines(path: Path) -> int:
    """Compte les lignes de données du CSV."""
    if not path.exists():
        return 0
    with open(path) as f:
        return sum(1 for _ in f) - 1  # -1 pour le header


def write_status(
    state: str,
    n_cycles: int = 0,
    last_train: Optional[datetime] = None,
    best_val_loss: float = 0.0,
    params: Optional[dict] = None,
):
    """Écrit le statut du serveur dans le fichier JSON."""
    Path("automl/output").mkdir(parents=True, exist_ok=True)
    status = {
        "state":       state,
        "n_cycles":    n_cycles,
        "last_train":  last_train.isoformat() if last_train else None,
        "timestamp":   datetime.utcnow().isoformat(),
        "best_val_loss": best_val_loss,
        "current_params": params or {},
    }
    with open(STATUS_FILE, "w") as f:
        json.dump(status, f, indent=2)


def run_training(full: bool = False) -> Optional[dict]:
    """Lance l'entraînement et retourne les résultats."""
    try:
        if full:
            from optimizer import optimize_and_train
            result = optimize_and_train(
                csv_path    = TRAINING_CSV,
                model_path  = MODEL_PATH,
                output_json = OUTPUT_JSON,
                n_trials    = 10,
                final_epochs= FULL_EPOCHS,
            )
        else:
            from trainer import train
            result = train(
                csv_path    = TRAINING_CSV,
                model_path  = MODEL_PATH,
                output_json = OUTPUT_JSON,
                epochs      = TRAIN_EPOCHS,
                verbose     = False,
            )
        return result
    except Exception as e:
        logger.error(f"Erreur d'entraînement : {e}", exc_info=True)
        return None


def run_prediction() -> Optional[dict]:
    """Lance uniquement la prédiction sur le contexte actuel."""
    try:
        if not MODEL_PATH.exists():
            return None

        from trainer import predict_current
        # Contexte par défaut (sera remplacé par les données réelles)
        context = {
            "atr_pts":         500.0,
            "rho1":            -0.10,
            "regime":          1,
            "net_delta":       0.0,
            "hedge_pressure":  0.0,
            "recov_depth":     0,
            "drawdown_pct":    0.0,
            "ofi_score":       0.0,
            "imbalance_score": 0.0,
        }

        # Lire le dernier cycle du CSV si disponible
        if TRAINING_CSV.exists():
            import csv
            with open(TRAINING_CSV) as f:
                reader = csv.DictReader(f)
                last_row = None
                for row in reader:
                    last_row = row
            if last_row:
                context.update({
                    "atr_pts":         float(last_row.get("atr_pts",         500.0)),
                    "rho1":            float(last_row.get("rho1",            -0.10)),
                    "regime":          int(last_row.get("regime",            1)),
                    "net_delta":       float(last_row.get("net_delta",       0.0)),
                    "hedge_pressure":  float(last_row.get("hedge_pressure",  0.0)),
                    "recov_depth":     int(last_row.get("recov_depth",       0)),
                    "drawdown_pct":    float(last_row.get("drawdown_pct",    0.0)),
                    "ofi_score":       float(last_row.get("ofi_score",       0.0)),
                    "imbalance_score": float(last_row.get("imbalance_score", 0.0)),
                })

        return predict_current(context, MODEL_PATH, OUTPUT_JSON)

    except Exception as e:
        logger.error(f"Erreur de prédiction : {e}", exc_info=True)
        return None


# ────────────────────────────────────────────────────────────
#  Mode surveillance continue
# ────────────────────────────────────────────────────────────
def watch_mode():
    """Surveillance continue du fichier CSV — auto-entraînement."""
    logger.info("TOP4EA AutoML Server démarré en mode WATCH")
    logger.info(f"Surveillance : {TRAINING_CSV}")
    logger.info(f"Sortie       : {OUTPUT_JSON}")
    logger.info(f"Seuils       : {MIN_NEW_CYCLES} nouveaux cycles | "
                f"{MAX_AGE_MINUTES} min max")

    last_hash       = get_file_hash(TRAINING_CSV)
    last_n_cycles   = count_csv_lines(TRAINING_CSV)
    last_train_time = datetime.utcnow()
    n_trains        = 0
    full_train_done = False

    write_status("watching", last_n_cycles)

    while True:
        try:
            time.sleep(WATCH_INTERVAL_S)

            current_hash    = get_file_hash(TRAINING_CSV)
            current_n       = count_csv_lines(TRAINING_CSV)
            new_cycles      = current_n - last_n_cycles
            age_min         = (datetime.utcnow() - last_train_time).seconds / 60.0

            file_changed    = (current_hash != last_hash)
            enough_new      = (new_cycles >= MIN_NEW_CYCLES)
            too_old         = (age_min >= MAX_AGE_MINUTES) and current_n >= 10
            need_full       = (n_trains > 0 and n_trains % 5 == 0 and not full_train_done)

            if file_changed and (enough_new or too_old):
                logger.info(
                    f"Déclenchement entraînement : "
                    f"new_cycles={new_cycles} | age={age_min:.1f}min "
                    f"| full={need_full}")

                write_status("training", current_n)

                result = run_training(full=need_full)
                n_trains += 1
                full_train_done = need_full

                if result:
                    params = {k: v for k, v in result.items()
                              if k not in ("training_meta",)}
                    meta   = result.get("training_meta", {})
                    logger.info(
                        f"✓ Entraînement #{n_trains} terminé "
                        f"val_loss={meta.get('best_val_loss', '?'):.5f} | "
                        f"params={params}")
                    write_status(
                        "idle", current_n, datetime.utcnow(),
                        meta.get("best_val_loss", 0.0), params)
                else:
                    write_status("idle", current_n)

                last_hash       = get_file_hash(TRAINING_CSV)
                last_n_cycles   = current_n
                last_train_time = datetime.utcnow()
                full_train_done = False

            elif file_changed and MODEL_PATH.exists():
                # Données nouvelles mais pas assez → juste prédire
                run_prediction()
                last_hash = current_hash

        except KeyboardInterrupt:
            logger.info("AutoML Server arrêté par l'utilisateur")
            write_status("stopped")
            break
        except Exception as e:
            logger.error(f"Erreur serveur : {e}", exc_info=True)
            write_status("error")
            time.sleep(60)


# ────────────────────────────────────────────────────────────
#  Point d'entrée
# ────────────────────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="TOP4EA AutoML Server")
    parser.add_argument(
        "--mode",
        choices=["train", "predict", "optimize", "watch"],
        default="watch",
        help="Mode d'exécution",
    )
    args = parser.parse_args()

    # Créer les répertoires nécessaires
    Path("data").mkdir(exist_ok=True)
    Path("automl/output").mkdir(parents=True, exist_ok=True)
    Path("automl/checkpoints").mkdir(parents=True, exist_ok=True)

    if args.mode == "train":
        logger.info("Mode : entraînement complet")
        result = run_training(full=False)
        if result:
            print(json.dumps(result, indent=2))

    elif args.mode == "optimize":
        logger.info("Mode : optimisation + entraînement")
        result = run_training(full=True)
        if result:
            print(json.dumps(result, indent=2))

    elif args.mode == "predict":
        logger.info("Mode : prédiction seule")
        result = run_prediction()
        if result:
            print(json.dumps(result, indent=2))
        else:
            print("Aucun modèle disponible — lancez d'abord --mode train")

    else:
        watch_mode()
