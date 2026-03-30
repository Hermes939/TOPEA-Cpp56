//+------------------------------------------------------------------+
//|  TOP3EA_ANALYST.mq4                                              |
//|  Dashboard analytique temps réel pour TOP3EA                    |
//|  Affiche : drawdown, ratio, ATR, suggestions GridGap            |
//+------------------------------------------------------------------+
#property copyright "TOP3EA"
#property version   "1.00"
#property strict

extern string TradingSymbol  = "XAUUSDm";
extern int    Magic          = 10300;
extern int    GridGap        = 500;
extern double BasketTarget   = 7.0;
extern int    ATR_Period     = 14;
extern double RiskAlertPct   = 20.0;  // Alerte si marge libre < 20%

//--- Stats session
double g_peakProfit    = 0;
double g_maxDrawdown   = 0;
double g_totalProfit   = 0;
int    g_totalRecovery = 0;
int    g_totalClosings = 0;
int    g_prevOrders    = 0;
datetime g_sessionStart = 0;

//+------------------------------------------------------------------+
double GetATR(int period) {
   double atr = 0;
   for(int i = 1; i <= period; i++) {
      double h = High[i], l = Low[i], pc = Close[i+1];
      double tr = MathMax(h-l, MathMax(MathAbs(h-pc), MathAbs(l-pc)));
      atr += tr;
   }
   return atr / period;
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

int CountType(int type) {
   int cnt = 0;
   for(int i = OrdersTotal()-1; i >= 0; i--)
      if(OrderSelect(i, SELECT_BY_POS, MODE_TRADES))
         if(OrderMagicNumber() == Magic && OrderSymbol() == TradingSymbol)
            if(OrderType() == type) cnt++;
   return cnt;
}

//+------------------------------------------------------------------+
//| Suggère le meilleur GridGap selon ATR                           |
//+------------------------------------------------------------------+
int SuggestGridGap(double atrPoints) {
   // ATR faible  → grille serrée
   // ATR élevé   → grille large pour éviter trop de déclenchements
   if(atrPoints < 200)  return 200;
   if(atrPoints < 400)  return 300;
   if(atrPoints < 700)  return 500;
   if(atrPoints < 1000) return 700;
   return 1000;
}

//+------------------------------------------------------------------+
//| Évalue le risque actuel                                         |
//+------------------------------------------------------------------+
string RiskLevel(double freePct, int recov, double dd) {
   if(freePct < 10 || recov > 15 || dd > 50) return "🔴 CRITIQUE";
   if(freePct < 20 || recov > 10 || dd > 25) return "🟠 ÉLEVÉ";
   if(freePct < 40 || recov > 5  || dd > 10) return "🟡 MODÉRÉ";
   return "🟢 FAIBLE";
}

//+------------------------------------------------------------------+
//| Dessine les niveaux sur le chart                                |
//+------------------------------------------------------------------+
void DrawLevels(double atrPoints) {
   double suggestedHalf = SuggestGridGap(atrPoints) / 2.0 * Point;

   // Ligne niveau SS suggéré
   string nm1 = "ANALYST_SS";
   string nm2 = "ANALYST_BS";
   if(ObjectFind(nm1) < 0) ObjectCreate(nm1, OBJ_HLINE, 0, 0, 0);
   if(ObjectFind(nm2) < 0) ObjectCreate(nm2, OBJ_HLINE, 0, 0, 0);
   ObjectSet(nm1, OBJPROP_PRICE1, NormalizeDouble(Bid - suggestedHalf, Digits));
   ObjectSet(nm1, OBJPROP_COLOR, clrOrangeRed);
   ObjectSet(nm1, OBJPROP_STYLE, STYLE_DOT);
   ObjectSet(nm2, OBJPROP_PRICE1, NormalizeDouble(Ask + suggestedHalf, Digits));
   ObjectSet(nm2, OBJPROP_COLOR, clrDodgerBlue);
   ObjectSet(nm2, OBJPROP_STYLE, STYLE_DOT);
}

//+------------------------------------------------------------------+
void OnTick() {
   RefreshRates();

   double profit   = BasketProfit();
   int    buys     = CountType(OP_BUY);
   int    sells    = CountType(OP_SELL);
   int    total    = buys + sells;
   int    recov    = MathMax(buys, sells);
   double atr      = GetATR(ATR_Period);
   double atrPts   = atr / Point;
   double freePct  = AccountBalance() > 0 ? AccountFreeMargin() / AccountBalance() * 100.0 : 0;
   int    sugGap   = SuggestGridGap(atrPts);

   // Mise à jour stats session
   if(profit > g_peakProfit) g_peakProfit = profit;
   double dd = g_peakProfit - profit;
   if(dd > g_maxDrawdown) g_maxDrawdown = dd;
   if(total > g_prevOrders && g_prevOrders == 0) g_sessionStart = TimeCurrent();
   if(total < g_prevOrders && total == 0) { g_totalClosings++; g_totalProfit += profit; }
   if(recov > g_totalRecovery) g_totalRecovery = recov;
   g_prevOrders = total;

   // Ratio profit/recovery
   double ratio = (g_totalRecovery > 0) ? g_totalProfit / g_totalRecovery : 0;

   // Durée session
   int elapsed = (int)(TimeCurrent() - g_sessionStart);
   string duration = IntegerToString(elapsed/3600) + "h " + IntegerToString((elapsed%3600)/60) + "m";

   // Niveau de risque
   string risk = RiskLevel(freePct, recov, dd);

   // Alerte drawdown
   string alertStr = "";
   if(freePct < RiskAlertPct) alertStr = "⚠️ MARGE CRITIQUE !";
   if(dd > BasketTarget * 3)  alertStr = "⚠️ DRAWDOWN EXCESSIF !";

   // Dessiner niveaux suggérés
   DrawLevels(atrPts);

   // Dashboard
   Comment(
      "╔══════════════════════════════════╗\n"
      "║      TOP3EA ANALYST              ║\n"
      "╠══════════════════════════════════╣\n"
      "║  RISQUE     : " + risk + "\n"
      + (alertStr != "" ? "║  " + alertStr + "\n" : "")
      +"╠══════════════════════════════════╣\n"
      "║  P&L actuel : " + (profit>=0?"+":"") + DoubleToStr(profit,2) + " $\n"
      "║  Drawdown   : " + DoubleToStr(dd,2) + " $ (max: " + DoubleToStr(g_maxDrawdown,2) + "$)\n"
      "║  Peak profit: " + DoubleToStr(g_peakProfit,2) + " $\n"
      "╠══════════════════════════════════╣\n"
      "║  POSITIONS                       \n"
      "║  BUY : " + IntegerToString(buys) + "   SELL : " + IntegerToString(sells) + "\n"
      "║  Recovery actif : " + IntegerToString(recov) + "\n"
      "╠══════════════════════════════════╣\n"
      "║  VOLATILITÉ (ATR14)              \n"
      "║  ATR      : " + DoubleToStr(atrPts,0) + " pts\n"
      "║  GridGap actuel : " + IntegerToString(GridGap) + " pts\n"
      "║  GridGap suggéré: " + IntegerToString(sugGap) + " pts"
      + (sugGap != GridGap ? " ◄ AJUSTER" : " ✓ OK") + "\n"
      "╠══════════════════════════════════╣\n"
      "║  SESSION                         \n"
      "║  Durée   : " + duration + "\n"
      "║  Closings: " + IntegerToString(g_totalClosings) + "\n"
      "║  Ratio P/R: " + DoubleToStr(ratio,2) + " $/recov\n"
      "║  Marge libre: " + DoubleToStr(freePct,1) + " %\n"
      "╚══════════════════════════════════╝"
   );
}

//+------------------------------------------------------------------+
int OnInit() {
   g_sessionStart = TimeCurrent();
   Print("TOP3EA_ANALYST démarré");
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason) {
   Comment("");
   ObjectDelete("ANALYST_SS");
   ObjectDelete("ANALYST_BS");
}
//+------------------------------------------------------------------+
