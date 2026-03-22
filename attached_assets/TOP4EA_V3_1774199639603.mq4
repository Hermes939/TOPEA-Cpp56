//+------------------------------------------------------------------+
//|  TOP4EA.mq4                                                      |
//|  v6.04 – Fermeture basket adaptative ultra-rapide                |
//+------------------------------------------------------------------+
#property copyright "TOP4EA"
#property version   "6.04"
#property strict

//--- Paramètres
extern string TradingSymbol      = "XAUUSDm";
extern double BaseLot            = 0.10;
extern double LotMultiplier      = 1.15;
extern int    GridGap            = 500;
extern double BasketTarget       = 7.0;
extern int    AntiCascadeSec     = 3;
extern int    MaxRecovery        = 20;
extern int    Magic              = 10300;
extern int    Slippage           = 3;
extern double DailyTargetPercent = 10.0;   // 0 = désactivé

//--- Machine à états
enum EAState {
   STATE_INIT,
   STATE_GRID_ACTIVE,
   STATE_RECOVERY,
   STATE_CLOSING
};

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

//--- Gestion objectif journalier
double   g_dailyStartBalance  = 0;
datetime g_dailyResetTime     = 0;
bool     g_dailyTargetReached = false;

//+------------------------------------------------------------------+
//| Utilitaires                                                      |
//+------------------------------------------------------------------+
double Pip()    { return (Digits == 5 || Digits == 3) ? Point * 10 : Point; }
double NP(double p) { return NormalizeDouble(p, Digits); }
double MinDist(){ return MarketInfo(TradingSymbol, MODE_STOPLEVEL) * Point + 2 * Point; }
double HalfGap(){ return (GridGap / 2.0) * Point; }

int CountOrders(int type) {
   int cnt = 0;
   for(int i = OrdersTotal()-1; i >= 0; i--)
      if(OrderSelect(i, SELECT_BY_POS, MODE_TRADES))
         if(OrderMagicNumber() == Magic && OrderSymbol() == TradingSymbol)
            if(OrderType() == type) cnt++;
   return cnt;
}

int CountMarketOrders() {
   return CountOrders(OP_BUY) + CountOrders(OP_SELL);
}

bool HasMarketOrders() { return CountMarketOrders() > 0; }

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

double RecovLot(int count) {
   double l = BaseLot;
   for(int i = 0; i < count; i++) l *= LotMultiplier;
   return NormalizeDouble(MathMax(l, 0.01), 2);
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

int SafeSend(int type, double lot, double price, string comment, color clr) {
   price = NP(price);
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

//+------------------------------------------------------------------+
//| EnsureSellStop                                                   |
//+------------------------------------------------------------------+
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

//+------------------------------------------------------------------+
//| EnsureBuyStop                                                    |
//+------------------------------------------------------------------+
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

//+------------------------------------------------------------------+
//| TRI PAR LOT DÉCROISSANT (bubble sort sur arrays parallèles)     |
//| But : fermer les plus gros lots en premier = impact P&L max     |
//+------------------------------------------------------------------+
void SortByLotDesc(int &tickets[], double &lots[]) {
   int n = ArraySize(tickets);
   for(int i = 0; i < n-1; i++) {
      for(int j = i+1; j < n; j++) {
         if(lots[j] > lots[i]) {
            double tmpL    = lots[i];    lots[i]    = lots[j];    lots[j]    = tmpL;
            int    tmpT    = tickets[i]; tickets[i] = tickets[j]; tickets[j] = tmpT;
         }
      }
   }
}

//+------------------------------------------------------------------+
//| TRI PAR P&L ASCENDANT (losers en premier)                       |
//| But : stopper le saignement avant de fermer les winners         |
//+------------------------------------------------------------------+
void SortByPnLAsc(int &tickets[], double &pnls[], double &lots[]) {
   int n = ArraySize(tickets);
   for(int i = 0; i < n-1; i++) {
      for(int j = i+1; j < n; j++) {
         if(pnls[j] < pnls[i]) {
            double tmpP    = pnls[i];    pnls[i]    = pnls[j];    pnls[j]    = tmpP;
            double tmpL    = lots[i];    lots[i]    = lots[j];    lots[j]    = tmpL;
            int    tmpT    = tickets[i]; tickets[i] = tickets[j]; tickets[j] = tmpT;
         }
      }
   }
}

//+------------------------------------------------------------------+
//| FERMETURE BASKET ADAPTATIVE — PIPELINE 4 PHASES                 |
//|                                                                  |
//| Zone A (2–4 ord)  : CloseBy paires + singles RefreshRates chaque|
//| Zone B (5–15 ord) : CloseBy paires + singles RefreshRates /3    |
//| Zone C (16–30 ord): CloseBy paires + singles RefreshRates /3    |
//|                                                                  |
//| Séquencement singles : P&L ASCENDANT (losers first)             |
//| → Stoppe le saignement, profit winners préservé le plus longtemps|
//| → Impossible de sortir en négatif si CloseBy pairs > 0          |
//+------------------------------------------------------------------+
void CloseBasketAtomic() {
   g_state = STATE_CLOSING;
   int totalOrders = CountMarketOrders();

   // Détermination de la zone pour le RefreshRates adaptatif
   // Zone A : 2-4  → RefreshRates avant chaque single
   // Zone B : 5-15 → RefreshRates tous les 3 singles
   // Zone C : 16+  → RefreshRates tous les 3 singles
   int refreshInterval = (totalOrders <= 4) ? 1 : 3;

   Print(">>> CloseBasketAtomic ZONE ",
         (totalOrders<=4?"A":totalOrders<=15?"B":"C"),
         " | ordres=", totalOrders,
         " | refreshInterval=", refreshInterval);

   //------------------------------------------------------------------
   // PHASE 0 — PURGE STOPS
   // Supprimer tous les pending en un seul scan
   // Aucun nouveau déclenchement possible pendant la fermeture
   //------------------------------------------------------------------
   for(int i = OrdersTotal()-1; i >= 0; i--) {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != Magic || OrderSymbol() != TradingSymbol) continue;
      if(OrderType() == OP_BUYSTOP || OrderType() == OP_SELLSTOP) {
         OrderDelete(OrderTicket());
      }
   }
   g_ssTicket = -1;
   g_bsTicket = -1;

   //------------------------------------------------------------------
   // Collecte des positions de marché avec leur P&L
   //------------------------------------------------------------------
   int    buyTickets[], sellTickets[];
   double buyLots[],    sellLots[];
   double buyPnL[],     sellPnL[];

   ArrayResize(buyTickets,  0); ArrayResize(sellTickets, 0);
   ArrayResize(buyLots,     0); ArrayResize(sellLots,    0);
   ArrayResize(buyPnL,      0); ArrayResize(sellPnL,     0);

   for(int i = OrdersTotal()-1; i >= 0; i--) {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != Magic || OrderSymbol() != TradingSymbol) continue;
      int type = OrderType();
      if(type == OP_BUY) {
         int idx = ArraySize(buyTickets);
         ArrayResize(buyTickets, idx+1);
         ArrayResize(buyLots,    idx+1);
         ArrayResize(buyPnL,     idx+1);
         buyTickets[idx] = OrderTicket();
         buyLots[idx]    = OrderLots();
         buyPnL[idx]     = OrderProfit() + OrderSwap() + OrderCommission();
      } else if(type == OP_SELL) {
         int idx = ArraySize(sellTickets);
         ArrayResize(sellTickets, idx+1);
         ArrayResize(sellLots,    idx+1);
         ArrayResize(sellPnL,     idx+1);
         sellTickets[idx] = OrderTicket();
         sellLots[idx]    = OrderLots();
         sellPnL[idx]     = OrderProfit() + OrderSwap() + OrderCommission();
      }
   }

   int nBuy  = ArraySize(buyTickets);
   int nSell = ArraySize(sellTickets);
   Print("Collecte : ", nBuy, " BUY + ", nSell, " SELL");

   //------------------------------------------------------------------
   // PHASE 1 — NETTING CloseBy
   // Trier BUY[] et SELL[] par lot DÉCROISSANT
   // Matcher paires lot-à-lot (tolérance 0.005)
   // OrderCloseBy() : 1 appel = 2 ordres fermés, δ=0, 0 spread, 0 slippage
   // Profit de la paire VERROUILLÉ instantanément, inattaquable par le prix
   //------------------------------------------------------------------
   SortByLotDesc(buyTickets,  buyLots);
   SortByLotDesc(sellTickets, sellLots);

   bool buyUsed[];  ArrayResize(buyUsed,  nBuy);  ArrayInitialize(buyUsed,  0);
   bool sellUsed[]; ArrayResize(sellUsed, nSell); ArrayInitialize(sellUsed, 0);

   int pairsCount = 0;
   for(int b = 0; b < nBuy; b++) {
      if(buyUsed[b]) continue;
      for(int s = 0; s < nSell; s++) {
         if(sellUsed[s]) continue;
         if(MathAbs(buyLots[b] - sellLots[s]) < 0.005) {
            if(OrderCloseBy(buyTickets[b], sellTickets[s], clrYellow)) {
               buyUsed[b]  = true;
               sellUsed[s] = true;
               pairsCount++;
               Print("CloseBy paire #", pairsCount,
                     " BUY=", buyTickets[b],
                     " SELL=", sellTickets[s],
                     " lot=", buyLots[b]);
               break;
            } else {
               // CloseBy échoué → forcer fermeture individuelle
               Print("CloseBy échoué paire b=", b, " s=", s, " err=", GetLastError());
            }
         }
      }
   }
   Print("Phase 1 terminée : ", pairsCount, " paires CloseBy");

   //------------------------------------------------------------------
   // PHASE 2 — FLUSH SINGLES (ordres impairs restants)
   // Trier par P&L ASCENDANT : losers fermés en premier
   // → Stoppe le saignement immédiatement
   // → Les winners préservés le plus longtemps possible
   // → Impossible de sortir en négatif si Phase 1 a verrouillé le profit
   // RefreshRates() adaptatif selon la zone (A/B/C)
   //------------------------------------------------------------------

   // Collecter les singles BUY restants
   int    singleTickets[];
   double singleLots[];
   double singlePnL[];
   ArrayResize(singleTickets, 0);
   ArrayResize(singleLots,    0);
   ArrayResize(singlePnL,     0);

   for(int b = 0; b < nBuy; b++) {
      if(buyUsed[b]) continue;
      int idx = ArraySize(singleTickets);
      ArrayResize(singleTickets, idx+1);
      ArrayResize(singleLots,    idx+1);
      ArrayResize(singlePnL,     idx+1);
      singleTickets[idx] = buyTickets[b];
      singleLots[idx]    = buyLots[b];
      singlePnL[idx]     = buyPnL[b];
   }
   // Collecter les singles SELL restants
   for(int s = 0; s < nSell; s++) {
      if(sellUsed[s]) continue;
      int idx = ArraySize(singleTickets);
      ArrayResize(singleTickets, idx+1);
      ArrayResize(singleLots,    idx+1);
      ArrayResize(singlePnL,     idx+1);
      singleTickets[idx] = sellTickets[s];
      singleLots[idx]    = sellLots[s];
      singlePnL[idx]     = sellPnL[s];
   }

   // Tri P&L ascendant : losers en premier
   SortByPnLAsc(singleTickets, singlePnL, singleLots);

   int nSingles = ArraySize(singleTickets);
   Print("Phase 2 : ", nSingles, " singles à fermer (triés P&L asc)");

   for(int k = 0; k < nSingles; k++) {
      // RefreshRates adaptatif
      if(k == 0 || k % refreshInterval == 0) RefreshRates();

      int ticket = singleTickets[k];
      if(!OrderSelect(ticket, SELECT_BY_TICKET)) continue;
      if(OrderCloseTime() > 0) continue; // déjà fermé

      double closePrice = (OrderType() == OP_BUY) ? Bid : Ask;
      bool ok = OrderClose(ticket, singleLots[k], closePrice, Slippage,
                           (OrderType()==OP_BUY ? clrBlue : clrRed));
      Print("Single close #", k+1, "/", nSingles,
            " ticket=", ticket,
            " lot=", singleLots[k],
            " pnl=", DoubleToStr(singlePnL[k],2),
            " ok=", ok);
   }

   //------------------------------------------------------------------
   // PHASE 3 — SAFETY NET
   // Attente 200ms puis vérification résiduelle
   // Si ordres encore ouverts → retry ×3 forcé
   //------------------------------------------------------------------
   Sleep(200);
   RefreshRates();

   int residual = 0;
   for(int attempt = 0; attempt < 3; attempt++) {
      residual = 0;
      for(int i = OrdersTotal()-1; i >= 0; i--) {
         if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
         if(OrderMagicNumber() != Magic || OrderSymbol() != TradingSymbol) continue;
         if(OrderType() == OP_BUY || OrderType() == OP_SELL) {
            residual++;
            RefreshRates();
            double price = (OrderType() == OP_BUY) ? Bid : Ask;
            bool ok = OrderClose(OrderTicket(), OrderLots(), price, Slippage, clrMagenta);
            Print("Safety retry #", attempt+1, " ticket=", OrderTicket(), " ok=", ok);
         }
      }
      if(residual == 0) break;
      Sleep(100);
   }
   if(residual > 0)
      Print("ATTENTION : ", residual, " ordres non fermés après Safety Net");

   //------------------------------------------------------------------
   // RESET COMPLET
   //------------------------------------------------------------------
   g_ssTicket    = -1; g_bsTicket  = -1;
   g_sellRecov   = 0;  g_buyRecov  = 0;
   g_lastTrigger = 0;
   g_state       = STATE_INIT;
   Print("=== BASKET FERMÉ — Pipeline adaptatif v6.04 | pairs=", pairsCount,
         " singles=", nSingles, " ===");
}

//+------------------------------------------------------------------+
//| Fermeture propre d'un côté quand MaxRecovery atteint            |
//+------------------------------------------------------------------+
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

//+------------------------------------------------------------------+
//| Vérification de cohérence                                        |
//+------------------------------------------------------------------+
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

//+------------------------------------------------------------------+
//| Restauration après redémarrage                                  |
//+------------------------------------------------------------------+
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

//+------------------------------------------------------------------+
//| Gestion des déclenchements                                      |
//+------------------------------------------------------------------+
void HandleTriggers() {
   bool canTrigger = (TimeCurrent() - g_lastTrigger >= AntiCascadeSec);

   if(!TicketStillPending(g_ssTicket) && g_ssTicket > 0) {
      if(canTrigger) {
         if(MaxRecovery > 0 && g_sellRecov >= MaxRecovery) {
            Print("MaxRecovery SS atteint → fermeture propre côté BUY");
            CloseOneSide(OP_BUY);
         } else {
            Print(">>> SS DÉCLENCHÉ #", g_sellRecov+1);
            g_sellRecov++;
            g_lastTrigger = TimeCurrent();
            g_state = STATE_RECOVERY;
         }
         g_ssTicket = -1;
      }
   }

   if(!TicketStillPending(g_bsTicket) && g_bsTicket > 0) {
      if(canTrigger) {
         if(MaxRecovery > 0 && g_buyRecov >= MaxRecovery) {
            Print("MaxRecovery BS atteint → fermeture propre côté SELL");
            CloseOneSide(OP_SELL);
         } else {
            Print(">>> BS DÉCLENCHÉ #", g_buyRecov+1);
            g_buyRecov++;
            g_lastTrigger = TimeCurrent();
            g_state = STATE_RECOVERY;
         }
         g_bsTicket = -1;
      }
   }
}

//+------------------------------------------------------------------+
//| Gestion objectif journalier                                      |
//+------------------------------------------------------------------+
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

//+------------------------------------------------------------------+
//| Dashboard                                                        |
//+------------------------------------------------------------------+
void DrawDashboard() {
   double profit  = BasketProfit();
   double freePct = AccountBalance() > 0 ? AccountFreeMargin() / AccountBalance() * 100.0 : 0;
   int    mkt     = CountMarketOrders();
   string profStr = (profit >= 0 ? "+" : "") + DoubleToStr(profit, 2);
   int    cd      = AntiCascadeSec - (int)(TimeCurrent() - g_lastTrigger);
   string cdStr   = cd > 0 ? "COOLDOWN " + IntegerToString(cd) + "s" : "PRET";
   string stStr   = (g_state==STATE_INIT     ? "INIT"    :
                     g_state==STATE_GRID_ACTIVE ? "ACTIF" :
                     g_state==STATE_RECOVERY    ? "RECOVERY" : "CLOSING");
   string zoneStr = mkt <= 4 ? "A" : mkt <= 15 ? "B" : "C";

   string dailyInfo = "";
   if(DailyTargetPercent > 0) {
      double profitToday = AccountBalance() - g_dailyStartBalance;
      double pctToday    = (g_dailyStartBalance > 0)
                           ? (profitToday / g_dailyStartBalance) * 100.0 : 0;
      string status = g_dailyTargetReached
                      ? "ATTEINT"
                      : StringFormat("%.1f%% / %.1f%%", pctToday, DailyTargetPercent);
      dailyInfo = "║  Daily     : " + status + "\n";
   }

   Comment(
      "╔═══════════════════════════════╗\n"
      "║         TOP4EA  V3            ║\n"
      "╠═══════════════════════════════╣\n"
      "║  État      : " + stStr + "\n"
      "║  Basket P&L: " + profStr + " $\n"
      "║  Target    : " + DoubleToStr(BasketTarget,2) + " $\n"
      "╠═══════════════════════════════╣\n"
      "║  SS Recovery: " + IntegerToString(g_sellRecov) + " / " + IntegerToString(MaxRecovery) + "\n"
      "║  BS Recovery: " + IntegerToString(g_buyRecov)  + " / " + IntegerToString(MaxRecovery) + "\n"
      "║  Ordres mkt : " + IntegerToString(mkt) + "  [ZONE " + zoneStr + "]\n"
      "╠═══════════════════════════════╣\n"
      + dailyInfo +
      "║  Marge libre: " + DoubleToStr(freePct,1) + " %\n"
      "║  SS @ " + DoubleToStr(g_ssLevel,Digits) + "\n"
      "║  BS @ " + DoubleToStr(g_bsLevel,Digits) + "\n"
      "║  Trigger : " + cdStr + "\n"
      "╚═══════════════════════════════╝"
   );
}

//+------------------------------------------------------------------+
//| OnTick                                                           |
//+------------------------------------------------------------------+
void OnTick() {
   RefreshRates();

   // 1. Vérifier l'objectif journalier en premier
   CheckDailyTarget();
   if(g_dailyTargetReached) { DrawDashboard(); return; }

   if(TimeCurrent() == g_lastAction) { DrawDashboard(); return; }
   g_lastAction = TimeCurrent();

   // 2. Fermeture basket si target atteint
   if(HasMarketOrders() && BasketProfit() >= BasketTarget) {
      CloseBasketAtomic();
      DrawDashboard();
      return;
   }

   // 3. Initialisation / restauration
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

//+------------------------------------------------------------------+
//| OnInit                                                           |
//+------------------------------------------------------------------+
int OnInit() {
   if(MarketInfo(TradingSymbol, MODE_BID) == 0) {
      Print("ERREUR: Symbole invalide : ", TradingSymbol);
      return INIT_FAILED;
   }
   g_state       = STATE_INIT;
   g_lastTrigger = TimeCurrent();

   MqlDateTime now;
   TimeToStruct(TimeCurrent(), now);
   now.hour = 0; now.min = 0; now.sec = 0;
   g_dailyResetTime     = StructToTime(now);
   g_dailyStartBalance  = AccountBalance();
   g_dailyTargetReached = false;

   Print("TOP4EA V3 initialisé (version 6.04)");
   Print("Pipeline fermeture : Adaptatif Zone A/B/C | CloseBy netting + Singles P&L asc");
   Print("Objectif journalier : ", DailyTargetPercent, "% (capital = ", g_dailyStartBalance, ")");
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason) { Comment(""); }
//+------------------------------------------------------------------+
