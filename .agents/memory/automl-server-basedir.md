---
name: AutoML server BASE_DIR path bug
description: When automl_server.py is launched via `cd top4ea_cpp && python3 ../automl/automl_server.py`, BASE_DIR must use Path.cwd() not Path(__file__).parent.parent
---

# AutoML Server BASE_DIR must use Path.cwd()

**Rule:** In `automl/automl_server.py`, set `BASE_DIR = Path.cwd()` instead of `Path(__file__).parent.parent`.

**Why:** The workflow command is `cd top4ea_cpp && python3 ../automl/automl_server.py --mode watch`. When Python runs with a relative path `../automl/automl_server.py`, `__file__` is set to the relative string `../automl/automl_server.py`. Therefore:
- `Path(__file__).parent` → `../automl` (relative)
- `Path(__file__).parent.parent` → `..` → resolves to workspace root (`/home/runner/workspace/`)
- `DATA_DIR` → `/home/runner/workspace/data/` — does NOT exist
- Actual training CSV is at `top4ea_cpp/data/automl_training.csv`

With `Path.cwd()` when CWD is `top4ea_cpp/`:
- `DATA_DIR` → `top4ea_cpp/data/` ✓
- `TRAINING_CSV` → `top4ea_cpp/data/automl_training.csv` ✓

**How to apply:** Any time the automl_server.py launch command changes (e.g., different CWD), verify BASE_DIR still resolves correctly. The fix ties BASE_DIR to the working directory at runtime, which is correct for this project layout.
