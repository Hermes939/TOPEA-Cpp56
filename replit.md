# TOP4EA — Moteur de Trading C++20

## Architecture

Système de trading algorithmique 100% C++20 connecté à MetaTrader 4 via un bridge TCP. Transformé depuis les scripts MQL4 originaux (v6.03 / v6.04).

## Structure du projet

```
top4ea_cpp/
├── include/
│   ├── types.hpp           — Types partagés, enums, structs
│   ├── math_core.hpp       — ATR, EWMA, Kelly, vélocité P&L
│   ├── mt4_bridge.hpp      — Connexion TCP vers MT4 (header)
│   ├── grid_engine.hpp     — Calcul niveaux SS/BS adaptatifs
│   ├── basket_manager.hpp  — Surveillance et fermeture basket
│   ├── recovery_engine.hpp — Machine recovery + anti-cascade
│   └── top4ea_engine.hpp   — Moteur principal (orchestrateur)
├── src/
│   ├── main.cpp            — Point d'entrée + chargement config
│   ├── mt4_bridge.cpp      — Bridge TCP JSON-ligne
│   ├── grid_engine.cpp     — GridGap ATR dynamique
│   ├── basket_manager.cpp  — Pipeline 4 phases fermeture
│   ├── recovery_engine.cpp — Recovery / anti-cascade
│   └── top4ea_engine.cpp   — Machine à états principale
├── bridge/
│   └── TOP4EA_Bridge.mq4   — Script MT4 bridge (côté MT4)
├── top4ea.conf             — Configuration (tous paramètres)
├── CMakeLists.txt          — Build CMake
└── Makefile                — Build Make (utilisé en prod)
```

## Workflow

**Commande :** `cd top4ea_cpp && make all && ./build/top4ea top4ea.conf`

## Améliorations mathématiques vs MQL4 original

| Fonctionnalité | MQL4 original | C++20 |
|---|---|---|
| GridGap | Fixe (500 pts) | ATR dynamique (clampé min/max) |
| Lot recovery | Géométrique simple | Kelly fraction cap |
| Fermeture basket | Seuil fixe | + vélocité EWMA prédictive |
| Détection volatilité | Non | VolRegime Low/Medium/High |
| Anti-slippage | Slippage fixe | Spread EWMA historique |
| Régime marché | Non | ATR percentile 33/66 |

## Configuration (top4ea.conf)

Tous les paramètres configurables sans recompilation.

## Connexion MT4

1. Charger `bridge/TOP4EA_Bridge.mq4` dans MetaTrader 4
2. Configurer `BridgeHost=IP_du_serveur BridgePort=9000`
3. Démarrer le moteur C++20 → le bridge se connecte automatiquement

## Protocole TCP

JSON-ligne (newline-delimited JSON) bidirectionnel :
- MT4 → C++20 : ticks, ordres, compte
- C++20 → MT4 : commandes trade (open/close/modify/delete)
