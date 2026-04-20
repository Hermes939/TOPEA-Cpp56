# TOP4EA — Moteur de Trading C++20

## Architecture

Système de trading algorithmique 100% C++20 connecté à MetaTrader 4 **et MetaTrader 5** via MetaApi REST.
Transformé depuis les scripts MQL4 originaux (v6.03 / v6.04). Compatible MT4/MT5 depuis v3.1.

## Structure du projet

```
top4ea_cpp/
├── include/
│   ├── types.hpp           — Types partagés, enums, structs (PlatformType MT4/MT5)
│   ├── math_core.hpp       — ATR, EWMA, Kelly, vélocité P&L
│   ├── metaapi_bridge.hpp  — Connexion MetaApi REST MT4/MT5 (header)
│   ├── grid_engine.hpp     — Calcul niveaux SS/BS adaptatifs
│   ├── basket_manager.hpp  — Surveillance et fermeture basket
│   ├── recovery_engine.hpp — Machine recovery + anti-cascade
│   ├── top4ea_dll.h        — API C exportée (MT4 et MT5 DLL)
│   └── top4ea_engine.hpp   — Moteur principal (orchestrateur)
├── src/
│   ├── main.cpp            — Point d'entrée + chargement config
│   ├── metaapi_bridge.cpp  — Bridge REST MetaApi MT4/MT5
│   ├── grid_engine.cpp     — GridGap ATR dynamique
│   ├── basket_manager.cpp  — Pipeline 4 phases fermeture
│   ├── recovery_engine.cpp — Recovery / anti-cascade
│   ├── top4ea_dll.cpp      — API DLL exportée C
│   └── top4ea_engine.cpp   — Machine à états principale
├── top4ea.conf             — Configuration (tous paramètres)
└── Makefile                — Build Make (utilisé en prod)
```

## Workflow

**Commande :** `cd top4ea_cpp && make all && ./build/top4ea top4ea.conf`

## Compatibilité MT4 / MT5

Le système détecte automatiquement la plateforme depuis `accountInformation.platform` de MetaApi.
Le paramètre `platform` dans `top4ea.conf` peut forcer `mt4`, `mt5`, ou `auto` (défaut).

| Comportement          | MT4                    | MT5                             |
|-----------------------|------------------------|---------------------------------|
| Slippage ordres marché | `"slippage": N`       | `"deviation": N` (en points)   |
| Mode de remplissage   | absent                 | `"fillingMode": "ORDER_FILLING_FOK"` |
| Modifier position ouverte | `ORDER_MODIFY`    | `POSITION_MODIFY` + `positionId` |
| Modifier ordre pending | `ORDER_MODIFY`        | `ORDER_MODIFY` (identique)      |
| Longueur commentaire  | libre                  | tronqué à 31 caractères        |
| Détection automatique | `platform=auto`        | `platform=auto`                 |

## Connexion MetaApi

Variables d'environnement requises :
- `METAAPI_TOKEN` : token d'authentification
- `METAAPI_ACCOUNT_ID` : ID du compte MT4 ou MT5
- `METAAPI_REGION` : région (défaut : `new-york`)

## Améliorations mathématiques vs MQL4 original

| Fonctionnalité | MQL4 original | C++20 |
|---|---|---|
| GridGap | Fixe (500 pts) | ATR dynamique (clampé min/max) |
| Lot recovery | Géométrique simple | Kelly fraction cap |
| Fermeture basket | Seuil fixe | + vélocité EWMA prédictive |
| Détection volatilité | Non | VolRegime Low/Medium/High |
| Anti-slippage | Slippage fixe | Spread EWMA historique |
| Régime marché | Non | ATR percentile 33/66 |
| Compatibilité | MT4 uniquement | MT4 + MT5 auto-détecté |

## Configuration (top4ea.conf)

Tous les paramètres configurables sans recompilation, incluant `platform = auto|mt4|mt5`.

## Système de Verrouillage Profit Trailing (V-SS / V-BS)

Remplace l'ancien système break-even. Fonctionnement :
- **V-SS** (SellStop trailing côté BUY) : s'active quand bid > avgBuy + `vl_activation_pts`. Niveau = bid - `vl_trail_pts`. Ratchet UP strict (ne descend jamais). Déclenchement → ferme toutes les Buy avec profit verrouillé.
- **V-BS** (BuyStop trailing côté SELL) : s'active quand ask < avgSell - `vl_activation_pts`. Niveau = ask + `vl_trail_pts`. Ratchet DOWN strict (ne remonte jamais). Déclenchement → ferme toutes les Sell avec profit verrouillé.

Paramètres `top4ea.conf` : `vl_enabled`, `vl_activation_pts` (défaut 50 pts), `vl_trail_pts` (défaut 30 pts).

## DeepSeek AI — Intégration active (market_intelligence.cpp)

L'IA DeepSeek était présente mais n'agissait pas réellement (signal affiché, aucun effet opérationnel). 4 fixes appliqués :

| Fix | Problème | Solution |
|-----|----------|----------|
| [A] Orientation active | DS ignoré sur TRAIL_NATURAL/OBSERVE (cas les plus fréquents) | DS ≥60% confiance oriente vers TRAIL_AGGRESSIVE dans sa direction |
| [B] Seuils abaissés | LOCK=85%, désaccord=90% → DS à 65% était ignoré | LOCK→70%, désaccord→70% |
| [C] Déblocage mutuel recovery | DS SELL bloque BS + struct.Bull bloque SS → tout bloqué | DS ≥70% contrebalance la structure seule, débloque recovery dans sa direction |
| [D] Biais trail réel | DS n'impactait jamais les distances SS/BS | dsTrailSSBias/dsTrailBSBias appliqués dans trailGrid() (max ±20%) |

## Bugs corrigés (session courante)

| Bug | Cause | Fix |
|-----|-------|-----|
| Duplication SS/BS via MetaApi HTTPS | POST timeout silencieux → ssTicket_=-1 → doublon | Scan `orders_` avant création, restaure ticket si doublon détecté |
| Recovery Martingale ignorée | CascadeGuard/VisualAI bloquait l'incrément du compteur | `forceIncrementSell/Buy()` + appel toujours à `onSellStopTriggered` |
| V-SS/V-BS lot incorrect | `baseLot` au lieu de `totalLots` | Lot = `totalLots` pour fermer tout le côté au déclenchement |
| Break-Even → Verrouillage | Concept erroné (zéro perte) | Redesign complet : trailing ratchet profit lock (V-SS/V-BS) |
