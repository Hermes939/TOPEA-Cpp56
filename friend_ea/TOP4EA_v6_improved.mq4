//+------------------------------------------------------------------+
//|  TOP4EA_v6_improved.mq4                                          |
//|  v6.03 – Base originale + améliorations optionnelles (inactives) |
//|                                                                  |
//|  AMÉLIORATIONS DISPONIBLES (toutes désactivées par défaut) :     |
//|   1. UseATRGrid        – GridGap dynamique basé sur l'ATR        |
//|   2. UseKellyLot       – Plafonnage Kelly des lots de recovery   |
//|   3. UsePnLVelocity    – Fermeture prédictive par vélocité P&L   |
//|   4. UseVolatilityRegime – Ajustement du target selon volatilité |
//|   5. UseSpreadGuard    – Anti-slippage EWMA du spread            |
//|   6. UseCascadeGuard   – Garde cascade (remplace AntiCascadeSec) |
//|                                                                  |
//|  Activer une option : passer son extern bool à true              |
//+------------------------------------------------------------------+
#property copyright "TOP4EA"
#property version   "6.03-improved"
#property strict

//=================================================================
//  PARAMÈTRES ORIGINAUX (inchangés)
//=================================================================
extern string TradingSymbol      = "XAUUSDm";
extern double BaseLot            = 0.10;
extern double LotMultiplier      = 1.15;
extern int    GridGap            = 500;
extern double BasketTarget       = 7.0;
extern int    AntiCascadeSec     = 3;
extern int    MaxRecovery        = 20;
extern int    Magic              = 10300;
extern int    Slippage           = 3;
extern double DailyTargetPercent = 10.0;

//=================================================================
//  AMÉLIORATIONS — INTERRUPTEURS (toutes inactives par défaut)
//=================================================================

//--- [1] GridGap dynamique basé sur l'ATR
extern bool   UseATRGrid          = false;
extern int    ATR_Period          = 14;       // Période ATR
extern double ATR_Multiplier      = 1.5;      // GridGap = ATR × multiplier
extern int    ATR_GridMin         = 200;      // plancher points
extern int    ATR_GridMax         = 1500;     // plafond points

//--- [2] Plafonnage Kelly des lots de recovery
extern bool   UseKellyLot         = false;
extern double Kelly_WinRate       = 0.55;     // taux de gain estimé [0.3 – 0.7]
extern double Kelly_Fraction      = 0.25;     // fraction Kelly à utiliser (prudence)
extern double Kelly_MaxLot        = 5.0;      // plafond absolu en lots

//--- [3] Fermeture prédictive par vélocité P&L (EWMA)
extern bool   UsePnLVelocity      = false;
extern double Velocity_Alpha      = 0.30;     // lissage EWMA [0.1 – 0.5]
extern double Velocity_Threshold  = -0.50;    // $/tick : fermeture si vélocité < seuil
extern int    Velocity_MinOrders  = 2;        // ordres de marché minimum requis

//--- [4] Régime de volatilité (ajuste BasketTarget × facteur)
extern bool   UseVolatilityRegime = false;
extern double Vol_LowThreshold    = 0.5;      // ATR_ratio < 0.5 → régime Low
extern double Vol_HighThreshold   = 1.5;      // ATR_ratio > 1.5 → régime High
extern double Vol_LowFactor       = 0.7;      // target × 0.7 en vol. basse
extern double Vol_HighFactor      = 1.4;      // target × 1.4 en vol. haute

//--- [5] Garde spread EWMA anti-slippage
extern bool   UseSpreadGuard      = false;
extern double Spread_MaxMult      = 2.5;      // bloqué si spread > 2.5× spread moyen EWMA
extern double Spread_EwmaAlpha    = 0.05;     // lissage très lent pour spread de référence

//--- [6] CascadeGuard : remplace AntiCascadeSec par 3 conditions
extern bool   UseCascadeGuard     = false;
extern double Cascade_DispatchPct = 0.50;     // déplacement min = 50% du GridGap
extern double Cascade_SpreadMult  = 2.5;      // même logique que UseSpreadGuard [2]
extern double Cascade_SpreadAlpha = 0.05;     // lissage EWMA spread pour CascadeGuard

//=================================================================
//  VARIABLES ORIGINALES
//=================================================================
enum EAState { STATE_INIT, STATE_GRID_ACTIVE, STATE_RECOVERY, STATE_CLOSING };

EAState  g_state        = STATE_INIT;
int      g_ssTicket     = -1;
int      g_bsTicket     = -1;
double   g_ssLevel      = 0;
double   g_bsLevel      = 0;
int      g_sellRecov    = 0;
int      g_buyRecov     = 0;
datetime g_lastTrigger  = 0;
datetime g_lastAction   = 0;
int      g_orderCounter = 0;

double   g_dailyStartBalance  = 0;
datetime g_dailyResetTime     = 0;
bool     g_dailyTargetReached = false;

//=================================================================
//  VARIABLES DES AMÉLIORATIONS
//=================================================================

//--- [1] ATR Grid
int    g_atrHandle       = INVALID_HANDLE;
int    g_effectiveGrid   = 500;              // GridGap effectif (static ou ATR)

//--- [2] Kelly
// (pas de variable persistante, calculé à la volée)

//--- [3] Vélocité P&L
double g_prevBasketProfit = 0;
double g_pnlVelocityEwma  = 0;
bool   g_velocityInitialized = false;

//--- [4] Régime de volatilité
double g_volBasket        = 1.0;            // facteur basket target actuel
// (régime calculé via ATR handle partagé)

//--- [5] Spread Guard
double g_spreadEwma5      = 0;              // EWMA spread pour UseSpreadGuard
bool   g_spreadInit5      = false;

//--- [6] Cascade Guard
double g_spreadEwma6      = 0;              // EWMA spread pour UseCascadeGuard
bool   g_spreadInit6      = false;
double g_lastTriggerPrice = 0;              // prix au moment du dernier trigger
int    g_whipsawSide      = 0;              // +1=BS déclenché, -1=SS déclenché

//=================================================================
//  UTILITAIRES ORIGINAUX
//=================================================================
double Pip()        { return (Digits == 5 || Digits == 3) ? Point * 10 : Point; }
double NP(double p) { return NormalizeDouble(p, Digits); }
double MinDist()    { return MarketInfo(TradingSymbol, MODE_STOPLEVEL) * Point + 2 * Point; }

//--- HalfGap adaptatif : utilise l'ATR si activé, sinon GridGap fixe
double HalfGap() {
   if(UseATRGrid) return (g_effectiveGrid / 2.0) * Point;
   return (GridGap / 2.0) * Point;
}

int CountOrders(int type) {
   int cnt = 0;
   for(int i = OrdersTotal()-1; i >= 0; i--)
      if(OrderSelect(i, SELECT_BY_POS, MODE_TRADES))
         if(OrderMagicNumber() == Magic && OrderSymbol() == TradingSymbol)
            if(OrderType() == type) cnt++;
   return cnt;
}

int CountMarketOrders() { return CountOrders(OP_BUY) + CountOrders(OP_SELL); }
bool HasMarketOrders()  { return CountMarketOrders() > 0; }

bool TicketStillPending(int ticket) {
   if(ticket <= 0) return false;
   for(int i = OrdersTotal()-1; i >= 0; i--)
      if(OrderSelect(i, SELECT_BY_POS, MODE_TRADES))
         if(OrderTicket() == ticket &&
            (OrderType() == OP_BUYSTOP || OrderType() == OP_SELLSTOP))
            return true;
   return false;
}

double BasketProfit() {
   double total = 0;
   for(int i = OrdersTotal()-1; i >= 0; i--)
      if(OrderSelect(i, SELECT_BY_POS, MODE_TRADES))
         if(OrderMagicNumber() == Magic && OrderSymbol() == TradingSymbol)
            if(OrderType() == OP_BUY || OrderType() == OP_SELL)
               total += OrderProfit() + OrderSwap() + OrderCommission();
   return total;
}

//--- Lot de recovery : original puis plafonnage Kelly si activé
double RecovLot(int count) {
   double l = BaseLot;
   for(int i = 0; i < count; i++) l *= LotMultiplier;
   l = NormalizeDouble(MathMax(l, 0.01), 2);

   if(UseKellyLot) {
      double b        = LotMultiplier - 1.0;  // gain net par unité (approximation)
      double p        = MathMax(0.3, MathMin(0.7, Kelly_WinRate));
      double q        = 1.0 - p;
      double kelly    = (b * p - q) / b;      // fraction Kelly
      double fraction = MathMax(0.0, kelly) * Kelly_Fraction;
      double balance  = AccountBalance() > 0 ? AccountBalance() : 1;
      double maxLotKelly = NormalizeDouble(fraction * balance / 1000.0, 2);
      maxLotKelly = MathMax(maxLotKelly, BaseLot);
      maxLotKelly = MathMin(maxLotKelly, Kelly_MaxLot);
      l = MathMin(l, maxLotKelly);
      l = NormalizeDouble(MathMax(l, 0.01), 2);
   }
   return l;
}

bool CloseOrderRobust(int ticket) {
   for(int attempt = 0; attempt < 3; attempt++) {
      if(ticket <= 0) return false;
      if(!OrderSelect(ticket, SELECT_BY_TICKET)) return false;
      RefreshRates();
      double price = (OrderType() == OP_BUY) ? Bid : Ask;
      if(OrderClose(ticket, OrderLots(), price, Slippage, clrBlue)) return true;
      Sleep(50);
   }
   return false;
}

//=================================================================
//  AMELIORATION [5] — Vérification du spread avant envoi d'ordre
//=================================================================
bool SpreadOK() {
   if(!UseSpreadGuard) return true;
   double curSpread = (Ask - Bid) / Point;
   if(!g_spreadInit5) {
      g_spreadEwma5   = curSpread;
      g_spreadInit5   = true;
      return true;
   }
   g_spreadEwma5 = Spread_EwmaAlpha * curSpread + (1.0 - Spread_EwmaAlpha) * g_spreadEwma5;
   if(g_spreadEwma5 <= 0) return true;
   bool ok = (curSpread <= Spread_MaxMult * g_spreadEwma5);
   if(!ok) Print("SpreadGuard BLOQUÉ : spread=", curSpread, " pts, moyenne=", g_spreadEwma5, " pts");
   return ok;
}

int SafeSend(int type, double lot, double price, string comment, color clr) {
   price = NP(price);

   if(!SpreadOK()) return -1;  // [5] Garde spread

   double marginReq  = MarketInfo(TradingSymbol, MODE_MARGINREQUIRED);
   double freeMargin = AccountFreeMargin();
   if(marginReq * lot > freeMargin) {
      Print("Marge insuffisante pour lot ", lot, " (req: ", marginReq*lot, ", libre: ", freeMargin, ")");
      return -1;
   }
   string fullComment = comment + " #" + IntegerToString(++g_orderCounter);
   int t = OrderSend(TradingSymbol, type, lot, price, Slippage, 0, 0, fullComment, Magic, 0, clr);
   if(t < 0) Print("ERREUR OrderSend type=", type, " err=", GetLastError(), " price=", price);
   else       Print("OK ticket=", t, " type=", type, " lot=", lot, " price=", price);
   return t;
}

//=================================================================
//  AMELIORATION [1] — Mise à jour du GridGap ATR
//=================================================================
void UpdateATRGrid() {
   if(!UseATRGrid) { g_effectiveGrid = GridGap; return; }

   double atrArr[];
   ArraySetAsSeries(atrArr, true);
   if(CopyBuffer(g_atrHandle, 0, 1, 1, atrArr) <= 0) { g_effectiveGrid = GridGap; return; }

   double atrPoints = atrArr[0] / Point;
   int    newGrid   = (int)MathRound(atrPoints * ATR_Multiplier);
   newGrid = (int)MathMax(ATR_GridMin, MathMin(ATR_GridMax, newGrid));
   if(newGrid != g_effectiveGrid) {
      Print("ATRGrid mis à jour : ", g_effectiveGrid, " → ", newGrid, " pts");
      g_effectiveGrid = newGrid;
   }
}

//=================================================================
//  AMELIORATION [4] — Régime de volatilité → facteur basket
//=================================================================
void UpdateVolatilityRegime() {
   if(!UseVolatilityRegime) { g_volBasket = 1.0; return; }

   double atrNow[], atrPrev[];
   ArraySetAsSeries(atrNow,  true);
   ArraySetAsSeries(atrPrev, true);
   if(CopyBuffer(g_atrHandle, 0, 1, 1, atrNow) <= 0)  { g_volBasket = 1.0; return; }
   if(CopyBuffer(g_atrHandle, 0, 1, 20, atrPrev) <= 0) { g_volBasket = 1.0; return; }

   double avg = 0;
   for(int i = 0; i < ArraySize(atrPrev); i++) avg += atrPrev[i];
   avg /= ArraySize(atrPrev);

   if(avg <= 0) { g_volBasket = 1.0; return; }
   double ratio = atrNow[0] / avg;

   string regime;
   if(ratio < Vol_LowThreshold)        { g_volBasket = Vol_LowFactor;  regime = "LOW";    }
   else if(ratio > Vol_HighThreshold)   { g_volBasket = Vol_HighFactor; regime = "HIGH";   }
   else                                 { g_volBasket = 1.0;            regime = "MEDIUM"; }

   static string lastRegime = "";
   if(regime != lastRegime) {
      Print("VolatilityRegime → ", regime, " | ratio=", DoubleToStr(ratio,2), " | facteur=", g_volBasket);
      lastRegime = regime;
   }
}

double EffectiveBasketTarget() {
   return BasketTarget * g_volBasket;
}

//=================================================================
//  AMELIORATION [3] — Vélocité P&L et fermeture prédictive
//=================================================================
void UpdatePnLVelocity() {
   if(!UsePnLVelocity) return;
   double curProfit = BasketProfit();
   if(!g_velocityInitialized) {
      g_prevBasketProfit   = curProfit;
      g_pnlVelocityEwma    = 0;
      g_velocityInitialized = true;
      return;
   }
   double delta        = curProfit - g_prevBasketProfit;
   g_pnlVelocityEwma   = Velocity_Alpha * delta + (1.0 - Velocity_Alpha) * g_pnlVelocityEwma;
   g_prevBasketProfit  = curProfit;
}

bool ShouldClosePredictive() {
   if(!UsePnLVelocity) return false;
   if(CountMarketOrders() < Velocity_MinOrders) return false;
   if(BasketProfit() <= 0) return false;
   if(g_pnlVelocityEwma < Velocity_Threshold) {
      Print("PnLVelocity CLOSE : vélocité EWMA = ", DoubleToStr(g_pnlVelocityEwma,4), " $/tick");
      return true;
   }
   return false;
}

//=================================================================
//  AMELIORATION [6] — CascadeGuard : 3 conditions de qualité
//=================================================================
void UpdateCascadeSpread() {
   if(!UseCascadeGuard) return;
   double curSpread = (Ask - Bid) / Point;
   if(!g_spreadInit6) { g_spreadEwma6 = curSpread; g_spreadInit6 = true; return; }
   g_spreadEwma6 = Cascade_SpreadAlpha * curSpread + (1.0 - Cascade_SpreadAlpha) * g_spreadEwma6;
}

bool CascadeGuardOK(int triggerType) {
   if(!UseCascadeGuard) {
      // Comportement original : délai temporel simple
      return (TimeCurrent() - g_lastTrigger >= AntiCascadeSec);
   }

   RefreshRates();
   double curPrice  = (triggerType == OP_SELL) ? Bid : Ask;
   double curSpread = (Ask - Bid) / Point;

   // [1] Déplacement minimum depuis le dernier trigger
   if(g_lastTriggerPrice > 0) {
      int gapUsed = UseATRGrid ? g_effectiveGrid : GridGap;
      double minDisplace = gapUsed * Cascade_DispatchPct * Point;
      if(MathAbs(curPrice - g_lastTriggerPrice) < minDisplace) {
         Print("CascadeGuard [1] BLOQUÉ : déplacement insuffisant (",
               DoubleToStr(MathAbs(curPrice - g_lastTriggerPrice)/Point, 1), " pts / min=",
               DoubleToStr(gapUsed * Cascade_DispatchPct, 1), " pts)");
         return false;
      }
   }

   // [2] Spread dans les limites normales
   if(g_spreadInit6 && g_spreadEwma6 > 0) {
      if(curSpread > Cascade_SpreadMult * g_spreadEwma6) {
         Print("CascadeGuard [2] BLOQUÉ : spread=", DoubleToStr(curSpread,1),
               " pts > ", DoubleToStr(Cascade_SpreadMult,1), "× EWMA=",
               DoubleToStr(g_spreadEwma6,1), " pts");
         return false;
      }
   }

   // [3] Anti-whipsaw : le prix doit tenir son côté depuis le dernier trigger
   if(g_lastTriggerPrice > 0 && g_whipsawSide != 0) {
      if(g_whipsawSide == -1 && curPrice > g_lastTriggerPrice) {
         // SS déclenché mais le prix est remonté → whipsaw
         Print("CascadeGuard [3] BLOQUÉ (whipsaw SS) : prix remonté au-dessus du trigger");
         return false;
      }
      if(g_whipsawSide == +1 && curPrice < g_lastTriggerPrice) {
         // BS déclenché mais le prix est redescendu → whipsaw
         Print("CascadeGuard [3] BLOQUÉ (whipsaw BS) : prix redescendu sous le trigger");
         return false;
      }
   }

   return true;
}

void RecordTrigger(int triggerType) {
   g_lastTrigger      = TimeCurrent();
   g_lastTriggerPrice = (triggerType == OP_SELL) ? Bid : Ask;
   g_whipsawSide      = (triggerType == OP_SELL) ? -1 : +1;
}

//=================================================================
//  FONCTIONS ORIGINALES (inchangées sauf appels adaptés)
//=================================================================
void EnsureSellStop(double lot, string comment) {
   RefreshRates();
   double minD   = MinDist();
   double target = NP(Bid - HalfGap());
   if(Bid - target < minD) target = NP(Bid - minD);

   if(TicketStillPending(g_ssTicket)) {
      if(target > g_ssLevel + 2*Point) {
         if(OrderModify(g_ssTicket, target, 0, 0, 0, clrOrangeRed)) {
            g_ssLevel = target;
            Print("SS déplacé → ", target);
         }
      }
   } else {
      Print("SS absent → recréation à ", target);
      g_ssTicket = SafeSend(OP_SELLSTOP, lot, target, comment, clrCrimson);
      if(g_ssTicket > 0) g_ssLevel = target;
   }
}

void EnsureBuyStop(double lot, string comment) {
   RefreshRates();
   double minD   = MinDist();
   double target = NP(Ask + HalfGap());
   if(target - Ask < minD) target = NP(Ask + minD);

   if(TicketStillPending(g_bsTicket)) {
      if(target < g_bsLevel - 2*Point) {
         if(OrderModify(g_bsTicket, target, 0, 0, 0, clrCornflowerBlue)) {
            g_bsLevel = target;
            Print("BS déplacé → ", target);
         }
      }
   } else {
      Print("BS absent → recréation à ", target);
      g_bsTicket = SafeSend(OP_BUYSTOP, lot, target, comment, clrDodgerBlue);
      if(g_bsTicket > 0) g_bsLevel = target;
   }
}

void TrailGrid() {
   double ssLot = RecovLot(g_sellRecov);
   double bsLot = RecovLot(g_buyRecov);
   string ssComment = (g_sellRecov > 0) ? "SS_R" + IntegerToString(g_sellRecov) : "SS_GRID";
   string bsComment = (g_buyRecov  > 0) ? "BS_R" + IntegerToString(g_buyRecov)  : "BS_GRID";
   EnsureSellStop(ssLot, ssComment);
   EnsureBuyStop (bsLot, bsComment);
}

void CloseBasketAtomic() {
   g_state = STATE_CLOSING;

   for(int i = OrdersTotal()-1; i >= 0; i--) {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != Magic || OrderSymbol() != TradingSymbol) continue;
      if(OrderType() == OP_BUYSTOP || OrderType() == OP_SELLSTOP)
         OrderDelete(OrderTicket());
   }

   int    buyTickets[], sellTickets[];
   double buyLots[],    sellLots[];
   ArrayResize(buyTickets, 0); ArrayResize(sellTickets, 0);
   ArrayResize(buyLots,    0); ArrayResize(sellLots,    0);

   for(int i = OrdersTotal()-1; i >= 0; i--) {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != Magic || OrderSymbol() != TradingSymbol) continue;
      int type = OrderType();
      if(type == OP_BUY) {
         int idx = ArraySize(buyTickets);
         ArrayResize(buyTickets, idx+1); ArrayResize(buyLots, idx+1);
         buyTickets[idx] = OrderTicket(); buyLots[idx] = OrderLots();
      } else if(type == OP_SELL) {
         int idx = ArraySize(sellTickets);
         ArrayResize(sellTickets, idx+1); ArrayResize(sellLots, idx+1);
         sellTickets[idx] = OrderTicket(); sellLots[idx] = OrderLots();
      }
   }

   RefreshRates();
   for(int b = 0; b < ArraySize(buyTickets); b++)
      if(OrderSelect(buyTickets[b], SELECT_BY_TICKET))
         OrderClose(buyTickets[b], buyLots[b], Bid, Slippage, clrBlue);
   for(int s = 0; s < ArraySize(sellTickets); s++)
      if(OrderSelect(sellTickets[s], SELECT_BY_TICKET))
         OrderClose(sellTickets[s], sellLots[s], Ask, Slippage, clrRed);

   g_ssTicket = -1; g_bsTicket = -1;
   g_sellRecov = 0; g_buyRecov = 0;
   g_lastTrigger = 0;
   g_lastTriggerPrice = 0;
   g_whipsawSide = 0;
   g_velocityInitialized = false;
   g_volBasket = 1.0;
   g_state = STATE_INIT;
   Print("=== BASKET FERMÉ ULTRA-RAPIDE ===");
}

void CloseOneSide(int sideType) {
   if(sideType == OP_BUY) {
      if(TicketStillPending(g_bsTicket)) { OrderDelete(g_bsTicket); g_bsTicket = -1; }
      if(TicketStillPending(g_ssTicket)) { OrderDelete(g_ssTicket); g_ssTicket = -1; }
   } else {
      if(TicketStillPending(g_ssTicket)) { OrderDelete(g_ssTicket); g_ssTicket = -1; }
      if(TicketStillPending(g_bsTicket)) { OrderDelete(g_bsTicket); g_bsTicket = -1; }
   }
   for(int i = OrdersTotal()-1; i >= 0; i--) {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != Magic || OrderSymbol() != TradingSymbol) continue;
      if(OrderType() == sideType) CloseOrderRobust(OrderTicket());
   }
   if(sideType == OP_BUY) { g_buyRecov = 0; g_bsTicket = -1; }
   else                    { g_sellRecov = 0; g_ssTicket = -1; }
   Print("Côté ", (sideType==OP_BUY?"BUY":"SELL"), " fermé proprement (MaxRecovery atteint)");
}

void CheckConsistency() {
   static datetime lastCheck = 0;
   if(TimeCurrent() - lastCheck < 10) return;
   lastCheck = TimeCurrent();

   if(g_ssTicket > 0) {
      if(OrderSelect(g_ssTicket, SELECT_BY_TICKET) && OrderType() == OP_SELLSTOP)
         g_ssLevel = OrderOpenPrice();
      else
         g_ssTicket = -1;
   }
   if(g_bsTicket > 0) {
      if(OrderSelect(g_bsTicket, SELECT_BY_TICKET) && OrderType() == OP_BUYSTOP)
         g_bsLevel = OrderOpenPrice();
      else
         g_bsTicket = -1;
   }
}

void RestoreState() {
   g_ssTicket = -1; g_bsTicket = -1;
   g_sellRecov = 0; g_buyRecov = 0;

   for(int i = OrdersTotal()-1; i >= 0; i--) {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != Magic || OrderSymbol() != TradingSymbol) continue;
      int type = OrderType();
      if(type == OP_SELLSTOP) { g_ssTicket = OrderTicket(); g_ssLevel = OrderOpenPrice(); }
      if(type == OP_BUYSTOP)  { g_bsTicket = OrderTicket(); g_bsLevel = OrderOpenPrice(); }
      if(type == OP_SELL)     g_sellRecov++;
      if(type == OP_BUY)      g_buyRecov++;
   }

   g_state = STATE_GRID_ACTIVE;
   Print("État restauré SS=", g_ssLevel, " BS=", g_bsLevel,
         " sellRecov=", g_sellRecov, " buyRecov=", g_buyRecov);
}

//=================================================================
//  HandleTriggers — avec CascadeGuard intégré
//=================================================================
void HandleTriggers() {
   if(!TicketStillPending(g_ssTicket) && g_ssTicket > 0) {
      if(CascadeGuardOK(OP_SELL)) {
         if(MaxRecovery > 0 && g_sellRecov >= MaxRecovery) {
            Print("MaxRecovery SS atteint → fermeture propre côté BUY");
            CloseOneSide(OP_BUY);
         } else {
            Print(">>> SS DÉCLENCHÉ #", g_sellRecov+1);
            g_sellRecov++;
            RecordTrigger(OP_SELL);
            g_state = STATE_RECOVERY;
         }
         g_ssTicket = -1;
      }
   }

   if(!TicketStillPending(g_bsTicket) && g_bsTicket > 0) {
      if(CascadeGuardOK(OP_BUY)) {
         if(MaxRecovery > 0 && g_buyRecov >= MaxRecovery) {
            Print("MaxRecovery BS atteint → fermeture propre côté SELL");
            CloseOneSide(OP_SELL);
         } else {
            Print(">>> BS DÉCLENCHÉ #", g_buyRecov+1);
            g_buyRecov++;
            RecordTrigger(OP_BUY);
            g_state = STATE_RECOVERY;
         }
         g_bsTicket = -1;
      }
   }
}

//=================================================================
//  CheckDailyTarget (original inchangé)
//=================================================================
void CheckDailyTarget() {
   if(DailyTargetPercent <= 0.0) return;

   datetime now = TimeCurrent();
   MqlDateTime today;
   TimeToStruct(now, today);
   today.hour = 0; today.min = 0; today.sec = 0;
   datetime dayStart = StructToTime(today);

   if(g_dailyResetTime < dayStart) {
      g_dailyStartBalance  = AccountBalance();
      g_dailyResetTime     = dayStart;
      g_dailyTargetReached = false;
      g_velocityInitialized = false;
      Print("Objectif journalier reset : nouveau capital = ", g_dailyStartBalance);
   }

   if(g_dailyTargetReached) return;

   double profitToday  = AccountBalance() - g_dailyStartBalance;
   double targetAmount = g_dailyStartBalance * DailyTargetPercent / 100.0;

   if(profitToday >= targetAmount) {
      Print("Objectif journalier atteint : profit = ", profitToday, " (", DailyTargetPercent, "%)");
      if(HasMarketOrders() || TicketStillPending(g_ssTicket) || TicketStillPending(g_bsTicket))
         CloseBasketAtomic();
      g_dailyTargetReached = true;
      Print("EA en pause jusqu'à demain");
   }
}

//=================================================================
//  Dashboard enrichi
//=================================================================
void DrawDashboard() {
   double profit    = BasketProfit();
   double target    = EffectiveBasketTarget();
   double freePct   = AccountBalance() > 0 ? AccountFreeMargin() / AccountBalance() * 100.0 : 0;
   int    mkt       = CountMarketOrders();
   string profStr   = (profit >= 0 ? "+" : "") + DoubleToStr(profit, 2);
   int    gapUsed   = UseATRGrid ? g_effectiveGrid : GridGap;

   // Affichage anti-cascade
   string cdStr;
   if(UseCascadeGuard) {
      cdStr = "CascadeGuard";
   } else {
      int cd = AntiCascadeSec - (int)(TimeCurrent() - g_lastTrigger);
      cdStr  = cd > 0 ? "COOLDOWN " + IntegerToString(cd) + "s" : "PRET";
   }

   string stStr = (g_state==STATE_INIT?"INIT":g_state==STATE_GRID_ACTIVE?"ACTIF":
                   g_state==STATE_RECOVERY?"RECOVERY":"CLOSING");

   // Régime de volatilité
   string volStr = "";
   if(UseVolatilityRegime) {
      string regime = (g_volBasket < 1.0 ? "LOW" : g_volBasket > 1.0 ? "HIGH" : "MEDIUM");
      volStr = "║  Vol.Regime : " + regime + " (×" + DoubleToStr(g_volBasket,2) + ")\n";
   }

   // Vélocité P&L
   string velStr = "";
   if(UsePnLVelocity)
      velStr = "║  PnL Vel.  : " + DoubleToStr(g_pnlVelocityEwma,4) + " $/tick\n";

   // Spread EWMA
   string spStr = "";
   if(UseSpreadGuard)
      spStr = "║  Spread avg: " + DoubleToStr(g_spreadEwma5,1) + " pts\n";

   // Daily target
   string dailyInfo = "";
   if(DailyTargetPercent > 0) {
      double profitToday  = AccountBalance() - g_dailyStartBalance;
      double targetAmount = g_dailyStartBalance * DailyTargetPercent / 100.0;
      double pctToday     = (profitToday / (g_dailyStartBalance > 0 ? g_dailyStartBalance : 1)) * 100.0;
      string status = g_dailyTargetReached
                      ? "ATTEINT"
                      : StringFormat("%.1f%% / %.1f%%", pctToday, DailyTargetPercent);
      dailyInfo = "║  Daily      : " + status + "\n";
   }

   Comment(
      "╔════════════════════════════════════╗\n"
      "║        TOP4EA  v6.03-improved      ║\n"
      "╠════════════════════════════════════╣\n"
      "║  État        : " + stStr + "\n"
      "║  Basket P&L  : " + profStr + " $\n"
      "║  Target      : " + DoubleToStr(target, 2) + " $\n"
      "║  GridGap     : " + IntegerToString(gapUsed) + " pts" + (UseATRGrid?" [ATR]":"") + "\n"
      "╠════════════════════════════════════╣\n"
      "║  SS Recovery : " + IntegerToString(g_sellRecov) + " / " + IntegerToString(MaxRecovery) + "\n"
      "║  BS Recovery : " + IntegerToString(g_buyRecov)  + " / " + IntegerToString(MaxRecovery) + "\n"
      "║  Ordres mkt  : " + IntegerToString(mkt) + "\n"
      "╠════════════════════════════════════╣\n"
      + dailyInfo + volStr + velStr + spStr +
      "║  Marge libre : " + DoubleToStr(freePct,1) + " %\n"
      "║  SS @ " + DoubleToStr(g_ssLevel, Digits) + "\n"
      "║  BS @ " + DoubleToStr(g_bsLevel, Digits) + "\n"
      "║  Trigger     : " + cdStr + "\n"
      "╚════════════════════════════════════╝"
   );
}

//=================================================================
//  OnTick
//=================================================================
void OnTick() {
   RefreshRates();

   // Mises à jour des améliorations (chaque tick, légères)
   UpdateATRGrid();            // [1]
   UpdateVolatilityRegime();   // [4]
   UpdateCascadeSpread();      // [6]
   UpdatePnLVelocity();        // [3]

   // Objectif journalier
   CheckDailyTarget();
   if(g_dailyTargetReached) { DrawDashboard(); return; }

   if(TimeCurrent() == g_lastAction) { DrawDashboard(); return; }
   g_lastAction = TimeCurrent();

   // Fermeture standard (target atteint)
   double curTarget = EffectiveBasketTarget();
   if(HasMarketOrders() && BasketProfit() >= curTarget) {
      CloseBasketAtomic();
      DrawDashboard();
      return;
   }

   // [3] Fermeture prédictive par vélocité
   if(ShouldClosePredictive() && BasketProfit() > 0) {
      Print("Fermeture prédictive PnLVelocity");
      CloseBasketAtomic();
      DrawDashboard();
      return;
   }

   if(g_state == STATE_INIT) {
      if(HasMarketOrders() || TicketStillPending(g_ssTicket) || TicketStillPending(g_bsTicket))
         RestoreState();
      else
         g_state = STATE_GRID_ACTIVE;
   }

   CheckConsistency();
   HandleTriggers();
   TrailGrid();
   DrawDashboard();
}

//=================================================================
//  OnInit
//=================================================================
int OnInit() {
   if(MarketInfo(TradingSymbol, MODE_BID) == 0) {
      Print("ERREUR: Symbole invalide : ", TradingSymbol);
      return INIT_FAILED;
   }

   // Création du handle ATR (utilisé par [1] et [4])
   if(UseATRGrid || UseVolatilityRegime) {
      g_atrHandle = iATR(TradingSymbol, 0, ATR_Period);
      if(g_atrHandle == INVALID_HANDLE) {
         Print("ERREUR: Impossible de créer l'indicateur ATR");
         return INIT_FAILED;
      }
      Print("ATR handle créé (période=", ATR_Period, ")");
   }

   g_state        = STATE_INIT;
   g_lastTrigger  = TimeCurrent();
   g_effectiveGrid = GridGap;
   g_volBasket    = 1.0;

   MqlDateTime now;
   TimeToStruct(TimeCurrent(), now);
   now.hour = 0; now.min = 0; now.sec = 0;
   g_dailyResetTime     = StructToTime(now);
   g_dailyStartBalance  = AccountBalance();
   g_dailyTargetReached = false;

   Print("TOP4EA v6.03-improved initialisé");
   Print("ATRGrid=",        UseATRGrid,         " | KellyLot=",      UseKellyLot);
   Print("PnLVelocity=",    UsePnLVelocity,     " | VolRegime=",     UseVolatilityRegime);
   Print("SpreadGuard=",    UseSpreadGuard,     " | CascadeGuard=",  UseCascadeGuard);
   Print("Objectif journalier : ", DailyTargetPercent, "% (capital = ", g_dailyStartBalance, ")");
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason) {
   if(g_atrHandle != INVALID_HANDLE) IndicatorRelease(g_atrHandle);
   Comment("");
}
//+------------------------------------------------------------------+
