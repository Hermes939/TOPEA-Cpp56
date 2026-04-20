"""
TOP4EA AutoML — Serveur d'auto-entraînement continu
Production-grade — éternel, robuste, long terme

FONCTIONNEMENT :
─────────────────────────────────────────────────────────
Surveillance continue des données de trading exportées par le
moteur C++. Auto-entraîne le modèle PyTorch et exporte les
paramètres optimaux toutes les N minutes ou cycles.

CYCLE D'AUTO-ENTRAÎNEMENT :
  1. Surveillance du fichier data/automl_training.csv
  2. Si ≥ MIN_NEW_CYCLES nouveaux cycles depuis le dernier train
     → Lancer l'entraînement complet (trainer.py)
  3. Si modèle entraîné + nouvelles données mais pas assez
     → Prédiction seule (trainer.predict_current)
  4. Mise à jour atomique de automl/output/params.json
     → Lu par le moteur C++ toutes les 5 minutes

ROBUSTESSE PRODUCTION :
  • SIGTERM / SIGINT : arrêt propre, statut sauvegardé
  • Log rotation : max 10 MB × 5 fichiers
  • JSON atomique : tmp → rename (jamais de lecture partielle C++)
  • CSV : copie atomique avant lecture (jamais de crash sur écriture C++)
  • Checkpoint multi-générations (N_KEEP_CHECKPOINTS)
  • Détection prédictions aberrantes avec fallback valeurs saines
  • age_min calculé avec total_seconds() (bug classique .seconds corrigé)
  • Timeout Optuna (évite blocage indéfini)
  • Sauvegarde checkpoint intermédiaire toutes les 10 epochs

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
import shutil
import signal
import hashlib
import argparse
import logging
import logging.handlers
import tempfile
import csv as csv_module
from pathlib import Path
from typing import Optional, Dict
from datetime import datetime

sys.path.insert(0, str(Path(__file__).parent))

# ── Répertoires ──────────────────────────────────────────────
BASE_DIR      = Path(__file__).parent.parent
DATA_DIR      = BASE_DIR / "data"
AUTOML_DIR    = BASE_DIR / "automl"
OUTPUT_DIR    = AUTOML_DIR / "output"
CKPT_DIR      = AUTOML_DIR / "checkpoints"
LOG_DIR       = DATA_DIR

for d in (DATA_DIR, OUTPUT_DIR, CKPT_DIR):
    d.mkdir(parents=True, exist_ok=True)

# ── Chemins ──────────────────────────────────────────────────
TRAINING_CSV  = DATA_DIR  / "automl_training.csv"
OUTPUT_JSON   = OUTPUT_DIR / "params.json"
STATUS_FILE   = OUTPUT_DIR / "status.json"
MODEL_PATH    = CKPT_DIR  / "top4ea_model.pt"
METRICS_LOG   = OUTPUT_DIR / "training_metrics.jsonl"   # une ligne JSON par train

# ── Paramètres serveur ────────────────────────────────────────
MIN_NEW_CYCLES      = 10
MAX_AGE_MINUTES     = 60
WATCH_INTERVAL_S    = 30
TRAIN_EPOCHS        = 50
FULL_EPOCHS         = 100
N_KEEP_CHECKPOINTS  = 5          # garde les 5 derniers checkpoints datés
OPTUNA_TIMEOUT_S    = 120        # timeout Optuna en secondes
PRED_SANITY_BOUNDS  = {          # bornes absolues de sécurité sur les prédictions
    "atr_mult":      (0.20, 2.00),
    "kelly_frac":    (0.05, 0.50),
    "gap_expansion": (0.01, 0.30),
    "trail_asym":    (0.05, 0.40),
    "vel_trigger":   (0.60, 0.98),
    "cppi_mult":     (1.50, 6.00),
    "vabt_coeff":    (0.05, 0.50),
    "ou_theta":      (0.01, 0.30),
}
PRED_DEFAULTS = {k: (lo + hi) / 2 for k, (lo, hi) in PRED_SANITY_BOUNDS.items()}

# ── Logging avec rotation ─────────────────────────────────────
def _setup_logging():
    log_file = LOG_DIR / "automl_server.log"
    fmt = "%(asctime)s [%(levelname)s] %(name)s — %(message)s"
    rotating = logging.handlers.RotatingFileHandler(
        log_file, mode="a",
        maxBytes=10 * 1024 * 1024,   # 10 MB
        backupCount=5,
        encoding="utf-8",
    )
    rotating.setFormatter(logging.Formatter(fmt))
    stream = logging.StreamHandler(sys.stdout)
    stream.setFormatter(logging.Formatter(fmt))
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    root.addHandler(rotating)
    root.addHandler(stream)

_setup_logging()
logger = logging.getLogger("top4ea.automl_server")

# ── État global partagé entre threads ────────────────────────
_shutdown_requested = False

def _handle_signal(signum, frame):
    global _shutdown_requested
    logger.info(f"Signal {signum} reçu — arrêt propre demandé")
    _shutdown_requested = True

signal.signal(signal.SIGTERM, _handle_signal)
signal.signal(signal.SIGINT,  _handle_signal)


# ────────────────────────────────────────────────────────────
#  Utilitaires fichiers
# ────────────────────────────────────────────────────────────
def get_file_hash(path: Path) -> str:
    """Hash MD5 pour détecter les modifications."""
    if not path.exists():
        return ""
    try:
        with open(path, "rb") as f:
            return hashlib.md5(f.read()).hexdigest()
    except OSError:
        return ""


def count_csv_lines(path: Path) -> int:
    """Compte les lignes de données du CSV (hors header)."""
    if not path.exists():
        return 0
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return sum(1 for _ in f) - 1
    except OSError:
        return 0


def safe_copy_csv(src: Path) -> Optional[Path]:
    """
    Copie atomique du CSV avant lecture pour éviter un crash
    si le moteur C++ écrit en même temps.
    Retourne le chemin de la copie temporaire (à supprimer après).
    """
    if not src.exists():
        return None
    try:
        tmp = tempfile.NamedTemporaryFile(
            suffix=".csv", prefix="automl_snap_",
            dir=DATA_DIR, delete=False)
        tmp.close()
        shutil.copy2(src, tmp.name)
        return Path(tmp.name)
    except OSError as e:
        logger.warning(f"Copie CSV échouée : {e}")
        return None


def atomic_write_json(data: dict, path: Path) -> bool:
    """
    Écriture atomique : tmp → rename.
    Garantit que le C++ ne lira jamais un JSON à moitié écrit.
    """
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        tmp.replace(path)
        return True
    except OSError as e:
        logger.error(f"Écriture JSON échouée : {e}")
        return False


def rotate_checkpoints(base_path: Path, n_keep: int = N_KEEP_CHECKPOINTS):
    """
    Sauvegarde datée du checkpoint et supprime les anciennes générations.
    Conserve les N_KEEP_CHECKPOINTS plus récents.
    """
    if not base_path.exists():
        return
    ts = datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    dated = base_path.with_name(f"top4ea_model_{ts}.pt")
    try:
        shutil.copy2(base_path, dated)
    except OSError:
        return

    ckpts = sorted(base_path.parent.glob("top4ea_model_*.pt"))
    for old in ckpts[:-n_keep]:
        try:
            old.unlink()
        except OSError:
            pass


def append_metrics(meta: dict, params: dict):
    """Journalise chaque entraînement dans un fichier JSONL (append-only)."""
    try:
        entry = {
            "ts": datetime.utcnow().isoformat(),
            "meta": meta,
            "params": params,
        }
        with open(METRICS_LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")
    except OSError:
        pass


# ────────────────────────────────────────────────────────────
#  Validation des prédictions (sanity check)
# ────────────────────────────────────────────────────────────
def sanitize_predictions(pred: dict) -> dict:
    """
    Vérifie que chaque paramètre prédit est dans ses bornes absolues.
    Remplace par la valeur par défaut si aberrant.
    Retourne un dict nettoyé + un flag indiquant si des corrections ont eu lieu.
    """
    cleaned = {}
    corrections = []
    for k, (lo, hi) in PRED_SANITY_BOUNDS.items():
        val = pred.get(k, PRED_DEFAULTS[k])
        if not isinstance(val, (int, float)) or not (lo <= val <= hi):
            corrections.append(f"{k}={val!r}→{PRED_DEFAULTS[k]:.4f}")
            val = PRED_DEFAULTS[k]
        cleaned[k] = round(float(val), 4)

    if corrections:
        logger.warning(f"Prédictions aberrantes corrigées : {corrections}")

    return cleaned


# ────────────────────────────────────────────────────────────
#  Statut JSON
# ────────────────────────────────────────────────────────────
def write_status(
    state: str,
    n_cycles: int = 0,
    last_train: Optional[datetime] = None,
    best_val_loss: float = 0.0,
    params: Optional[dict] = None,
):
    status = {
        "state":          state,
        "n_cycles":       n_cycles,
        "last_train":     last_train.isoformat() if last_train else None,
        "timestamp":      datetime.utcnow().isoformat(),
        "best_val_loss":  best_val_loss,
        "current_params": params or {},
    }
    atomic_write_json(status, STATUS_FILE)


# ────────────────────────────────────────────────────────────
#  Entraînement
# ────────────────────────────────────────────────────────────
def run_training(csv_snap: Path, full: bool = False) -> Optional[dict]:
    """Lance l'entraînement sur la copie snapshot du CSV."""
    try:
        if full:
            from optimizer import optimize_and_train
            result = optimize_and_train(
                csv_path      = csv_snap,
                model_path    = MODEL_PATH,
                output_json   = OUTPUT_JSON,
                n_trials      = 10,
                final_epochs  = FULL_EPOCHS,
                optuna_timeout= OPTUNA_TIMEOUT_S,
            )
        else:
            from trainer import train
            result = train(
                csv_path    = csv_snap,
                model_path  = MODEL_PATH,
                output_json = OUTPUT_JSON,
                epochs      = TRAIN_EPOCHS,
                verbose     = False,
            )
        return result
    except Exception as e:
        logger.error(f"Erreur d'entraînement : {e}", exc_info=True)
        return None


def run_prediction(csv_snap: Path) -> Optional[dict]:
    """Prédiction seule sur le contexte le plus récent du CSV snapshot."""
    try:
        if not MODEL_PATH.exists():
            return None

        context = dict(
            atr_pts         = 500.0,
            rho1            = 0.0,
            regime          = 1,
            net_delta       = 0.0,
            hedge_pressure  = 0.0,
            recov_depth     = 0,
            drawdown_pct    = 0.0,
            ofi_score       = 0.0,
            imbalance_score = 0.0,
        )

        if csv_snap.exists():
            try:
                with open(csv_snap, encoding="utf-8", errors="replace") as f:
                    reader = csv_module.DictReader(f)
                    last_row = None
                    for row in reader:
                        last_row = row
                if last_row:
                    def _f(key, default):
                        try:
                            return float(last_row.get(key, default))
                        except (ValueError, TypeError):
                            return float(default)
                    def _i(key, default):
                        try:
                            return int(float(last_row.get(key, default)))
                        except (ValueError, TypeError):
                            return int(default)
                    context.update(
                        atr_pts         = _f("atr_pts",         500.0),
                        rho1            = _f("rho1",            0.0),
                        regime          = _i("regime",          1),
                        net_delta       = _f("net_delta",       0.0),
                        hedge_pressure  = _f("hedge_pressure",  0.0),
                        recov_depth     = _i("recov_depth",     0),
                        drawdown_pct    = _f("drawdown_pct",    0.0),
                        ofi_score       = _f("ofi_score",       0.0),
                        imbalance_score = _f("imbalance_score", 0.0),
                    )
            except Exception as e:
                logger.warning(f"Lecture CSV snapshot échouée : {e}")

        from trainer import predict_current
        return predict_current(context, MODEL_PATH, OUTPUT_JSON)

    except Exception as e:
        logger.error(f"Erreur de prédiction : {e}", exc_info=True)
        return None


# ────────────────────────────────────────────────────────────
#  Mode surveillance continue
# ────────────────────────────────────────────────────────────
def watch_mode():
    global _shutdown_requested

    logger.info("═══════════════════════════════════════════")
    logger.info("  TOP4EA AutoML Server — mode WATCH")
    logger.info(f"  Surveillance : {TRAINING_CSV}")
    logger.info(f"  Sortie       : {OUTPUT_JSON}")
    logger.info(f"  Seuils       : {MIN_NEW_CYCLES} cycles / {MAX_AGE_MINUTES} min")
    logger.info(f"  Checkpoints  : {N_KEEP_CHECKPOINTS} générations conservées")
    logger.info("═══════════════════════════════════════════")

    last_hash       = get_file_hash(TRAINING_CSV)
    last_n_cycles   = count_csv_lines(TRAINING_CSV)
    last_train_time = datetime.utcnow()
    n_trains        = 0

    write_status("watching", last_n_cycles)

    while not _shutdown_requested:
        try:
            # Attente fractionnée pour réagir rapidement aux signaux
            for _ in range(WATCH_INTERVAL_S * 2):
                if _shutdown_requested:
                    break
                time.sleep(0.5)

            if _shutdown_requested:
                break

            current_hash = get_file_hash(TRAINING_CSV)
            current_n    = count_csv_lines(TRAINING_CSV)
            new_cycles   = current_n - last_n_cycles

            # ── Calcul age avec total_seconds() (corrige le bug .seconds) ──
            age_min = (datetime.utcnow() - last_train_time).total_seconds() / 60.0

            file_changed = (current_hash != last_hash)
            enough_new   = (new_cycles >= MIN_NEW_CYCLES)
            too_old      = (age_min >= MAX_AGE_MINUTES) and (current_n >= 10)
            # Full train tous les 5 trains normaux
            need_full    = (n_trains > 0) and (n_trains % 5 == 0)

            if file_changed and (enough_new or too_old):
                logger.info(
                    f"Déclenchement train#{n_trains+1} : "
                    f"new_cycles={new_cycles} | age={age_min:.1f}min "
                    f"| full={need_full}")

                write_status("training", current_n)

                # Copie atomique du CSV avant entraînement
                csv_snap = safe_copy_csv(TRAINING_CSV)
                if csv_snap is None:
                    logger.error("Snapshot CSV impossible — skip ce cycle")
                    write_status("error", current_n)
                    last_hash = current_hash
                    continue

                try:
                    result = run_training(csv_snap, full=need_full)
                    n_trains += 1

                    if result:
                        meta   = result.get("training_meta", {})
                        params_raw = {k: v for k, v in result.items()
                                      if k not in ("training_meta",)}

                        # Sanity check sur les prédictions
                        params = sanitize_predictions(params_raw)

                        # Écriture JSON atomique
                        output = dict(params)
                        output["training_meta"] = meta
                        atomic_write_json(output, OUTPUT_JSON)

                        # Rotation checkpoints
                        rotate_checkpoints(MODEL_PATH)

                        # Journalisation long terme
                        append_metrics(meta, params)

                        logger.info(
                            f"✓ Train#{n_trains} terminé "
                            f"val_loss={meta.get('best_val_loss', '?'):.5f} | "
                            f"cycles={meta.get('n_cycles', '?')} | "
                            f"params={params}")

                        write_status(
                            "idle", current_n, datetime.utcnow(),
                            meta.get("best_val_loss", 0.0), params)
                    else:
                        logger.warning(f"Train#{n_trains+1} échoué ou données insuffisantes")
                        write_status("idle", current_n)
                finally:
                    # Toujours supprimer le snapshot
                    try:
                        csv_snap.unlink()
                    except OSError:
                        pass

                last_hash       = get_file_hash(TRAINING_CSV)
                last_n_cycles   = current_n
                last_train_time = datetime.utcnow()

            elif file_changed and MODEL_PATH.exists():
                # Pas assez de nouveaux cycles → juste prédire
                csv_snap = safe_copy_csv(TRAINING_CSV)
                if csv_snap:
                    try:
                        pred = run_prediction(csv_snap)
                        if pred:
                            pred_clean = sanitize_predictions(pred)
                            atomic_write_json(pred_clean, OUTPUT_JSON)
                    finally:
                        try:
                            csv_snap.unlink()
                        except OSError:
                            pass
                last_hash = current_hash

        except Exception as e:
            logger.error(f"Erreur boucle serveur : {e}", exc_info=True)
            write_status("error")
            # Backoff progressif sur erreur répétée
            time.sleep(60)

    # ── Arrêt propre ─────────────────────────────────────────
    logger.info("AutoML Server arrêté proprement")
    write_status("stopped", last_n_cycles)


# ────────────────────────────────────────────────────────────
#  Point d'entrée
# ────────────────────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="TOP4EA AutoML Server")
    parser.add_argument(
        "--mode",
        choices=["train", "predict", "optimize", "watch"],
        default="watch",
    )
    args = parser.parse_args()

    if args.mode == "train":
        logger.info("Mode : entraînement complet")
        snap = safe_copy_csv(TRAINING_CSV) or TRAINING_CSV
        result = run_training(snap, full=False)
        if snap != TRAINING_CSV:
            snap.unlink(missing_ok=True)
        if result:
            print(json.dumps(result, indent=2))

    elif args.mode == "optimize":
        logger.info("Mode : optimisation + entraînement")
        snap = safe_copy_csv(TRAINING_CSV) or TRAINING_CSV
        result = run_training(snap, full=True)
        if snap != TRAINING_CSV:
            snap.unlink(missing_ok=True)
        if result:
            print(json.dumps(result, indent=2))

    elif args.mode == "predict":
        logger.info("Mode : prédiction seule")
        snap = safe_copy_csv(TRAINING_CSV) or TRAINING_CSV
        result = run_prediction(snap)
        if snap != TRAINING_CSV:
            snap.unlink(missing_ok=True)
        if result:
            print(json.dumps(result, indent=2))
        else:
            print("Aucun modèle disponible — lancez d'abord --mode train")

    else:
        watch_mode()
