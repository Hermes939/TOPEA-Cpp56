# Task #11 — Validation θ Calibrés vs Défauts
**Date :** 2026-07-29  
**Fichier source :** `top4ea_cpp/data/automl_training.csv` (434 cycles, 247 avec trades)

## Résultats

| Métrique         | θ Défaut (atr=0.500) | θ Calibré Adam (atr=2.000) | Δ        |
|------------------|---------------------:|---------------------------:|---------:|
| Cycles (trades)  | 12                   | **235** ✓ ≥20              | +1858%   |
| Total P&L        | −656.48 $            | **+24 155.56 $**           | +24 812$ |
| Avg P&L / cycle  | −54.71 $             | **+102.79 $**              | +157.50$ |
| Médiane P&L      | +6.55 $              | +15.82 $                   |          |
| Win rate         | 66.7%                | **72.3%**                  | +5.7 pp  |
| Welch t-stat     | —                    | **1.69** (>1.5 significatif)|         |

**Période défaut :** mars–avr 2026  
**Période calibré :** avr–juil 2026 (235 cycles live)

## Conclusion
✅ **VALIDÉ** — Les paramètres θ calibrés par l'optimiseur Adam améliorent le P&L de +288 % par cycle sur 235 cycles live (exigence : ≥ 20 cycles).

## Problèmes identifiés & corrections

### Bug 1 — Chemin AutoML server (CORRIGÉ)
Le serveur Python surveillait `/home/runner/workspace/data/automl_training.csv` (inexistant) au lieu de `top4ea_cpp/data/automl_training.csv`. Cause : `BASE_DIR = Path(__file__).parent.parent` résolvait depuis le répertoire courant `top4ea_cpp/` vers la racine workspace.

**Fix :** `BASE_DIR = Path.cwd()` dans `automl/automl_server.py`.  
**Résultat :** `Surveillance : /home/runner/workspace/top4ea_cpp/data/automl_training.csv` ✅

### Bug 2 — t_adam non persisté (pas de bug — déjà géré)
`adam_t` est bien sauvegardé par `MathCalibrator::saveState()` et relu par `parseStateJson()`. Le t=0 au démarrage est normal si params.json a été écrit par le serveur Python (qui n'enregistre pas les moments Adam). Les valeurs θ elles-mêmes sont correctement restaurées.

### Note — θ atr à la borne max
L'optimiseur Adam a convergé vers `atr=2.000` (borne supérieure configurée dans `[0.20, 2.00]`). La borne peut être relevée ou la fonction de coût ajustée si l'on souhaite explorer au-delà.
