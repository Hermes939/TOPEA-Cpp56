//+------------------------------------------------------------------+
//|  TOP5EA.mq4                                                      |
//|  v7.00 – Pure Hedge Multi-Paire Adaptative                       |
//|  Instrument : XAUUSDm                                            |
//+------------------------------------------------------------------+
#property copyright "TOP5EA"
#property version   "7.00"
#property strict

//=== PARAMÈTRES ====================================================
extern string TradingSymbol       = "XAUUSDm";
extern double BaseLot             = 0.10;
extern double LotMultiplier       = 1.15;
extern double MaxLot              = 10.00;      // cap absolu
extern int    GridGap             = 500;        // distance initiale SS/BS (points)
extern int    TrailDistance       = 150;        // trailing serré (points)
extern int    RecoveryOffset      = 500;        // offset outside recovery (points = 50 pips)
extern double BasketTarget        = 7.0;
extern int    MaxRecovery         = 20;
extern int    Magic               = 10400;
extern int    Slippage            = 3;
extern double DailyTargetPercent  = 10.0;       // 0 = désactivé
extern int    MinMoveAfterTrigger = 150;        // Option B : points depuis double déclenchement
extern int    ATRPeriod           = 14;         // Option C
extern double ATRMultiplier       = 2.5;        // Option C

//=== CONSTANTES ====================================================
#define MAX_PAIRS  20
#define TRAIL_STEP  2   // points minimum avant de bouger un stop

//=== ÉTATS =========================================================
enum EAState { STATE_INIT, STATE_ACTIVE, STATE_CLOSING };
EAState  g_state        = STATE_INIT;
datetime g_lastAction   = 0;
int      g_orderCounter = 0;

//=== STRUCTURE MULTI-PAIRES ========================================
// Chaque hedge pair = 1 SELLSTOP + 1 BUYSTOP
// Indépendants, trailent simultanément selon règle ratchet
int    g_numPairs             = 0;
int    g_pairSSTicket [MAX_PAIRS];  // ticket SELLSTOP (même ticket après déclenchement)
int    g_pairBSTicket [MAX_PAIRS];  // ticket BUYSTOP  (même ticket après déclenchement)
double g_pairSSLevel  [MAX_PAIRS];  // dernier prix SS connu
double g_pairBSLevel  [MAX_PAIRS];  // dernier prix BS connu
double g_pairLot      [MAX_PAIRS];  // lot de la paire
int    g_pairRecovLvl [MAX_PAIRS];  // niveau (0=initial, 1=recovery1, ...)
bool   g_pairSSFired  [MAX_PAIRS];  // SS devenu SELL marché
bool   g_pairBSFired  [MAX_PAIRS];  // BS devenu BUY  marché
double g_pairSSTrigPx [MAX_PAIRS];  // prix de déclenchement SS
double g_pairBSTrigPx [MAX_PAIRS];  // prix de déclenchement BS
bool   g_pairRecovDone[MAX_PAIRS];  // recovery hedge déjà posé pour cette paire

//=== OBJECTIF JOURNALIER ==========================================
double   g_dailyStartBalance  = 0;
datetime g_dailyResetTime     = 0;
bool     g_dailyTargetReached = false;

//==================================================================
//  UTILITAIRES
//==================================================================
double NP(double p)     { return NormalizeDouble(p, Digits); }
double MinDist()        { return MarketInfo(TradingSymbol, MODE_STOPLEVEL)*Point + 2*Point; }
double HalfGapPx()      { return (GridGap  / 2.0) * Point; }
double TrailDistPx()    { return TrailDistance * Point; }
double RecovOffPx()     { return RecoveryOffset * Point; }

bool IsPendingSS(int t) {
   if(t<=0) return false;
   if(!OrderSelect(t,SELECT_BY_TICKET)) return false;
   return (OrderType()==OP_SELLSTOP && OrderCloseTime()==0);
}
bool IsPendingBS(int t) {
   if(t<=0) return false;
   if(!OrderSelect(t,SELECT_BY_TICKET)) return false;
   return (OrderType()==OP_BUYSTOP && OrderCloseTime()==0);
}
bool IsFiredSell(int t) {
   if(t<=0) return false;
   if(!OrderSelect(t,SELECT_BY_TICKET)) return false;
   return (OrderType()==OP_SELL && OrderCloseTime()==0);
}
bool IsFiredBuy(int t) {
   if(t<=0) return false;
   if(!OrderSelect(t,SELECT_BY_TICKET)) return false;
   return (OrderType()==OP_BUY && OrderCloseTime()==0);
}

int CountMarketOrders() {
   int cnt=0;
   for(int i=OrdersTotal()-1;i>=0;i--)
      if(OrderSelect(i,SELECT_BY_POS,MODE_TRADES))
         if(OrderMagicNumber()==Magic && OrderSymbol()==TradingSymbol)
            if(OrderType()==OP_BUY || OrderType()==OP_SELL) cnt++;
   return cnt;
}
bool HasMarketOrders() { return CountMarketOrders()>0; }

double BasketProfit() {
   double t=0;
   for(int i=OrdersTotal()-1;i>=0;i--)
      if(OrderSelect(i,SELECT_BY_POS,MODE_TRADES))
         if(OrderMagicNumber()==Magic && OrderSymbol()==TradingSymbol)
            if(OrderType()==OP_BUY || OrderType()==OP_SELL)
               t += OrderProfit()+OrderSwap()+OrderCommission();
   return t;
}

double RecovLot(int level) {
   double l = BaseLot;
   for(int i=0;i<level;i++) l *= LotMultiplier;
   return NormalizeDouble(MathMin(MathMax(l,0.01),MaxLot),2);
}

bool CloseOrderRobust(int ticket) {
   for(int a=0;a<3;a++) {
      if(!OrderSelect(ticket,SELECT_BY_TICKET)) return false;
      RefreshRates();
      double price=(OrderType()==OP_BUY)?Bid:Ask;
      if(OrderClose(ticket,OrderLots(),price,Slippage,clrBlue)) return true;
      Sleep(50);
   }
   return false;
}

//==================================================================
//  FILTRE ATR (Option C)
//  Bloque recovery si volatilité explosive (news, spike)
//==================================================================
bool ATRFilterOK() {
   double atrNow = iATR(TradingSymbol,0,ATRPeriod,0);
   double atrAvg = 0;
   for(int i=1;i<=ATRPeriod;i++) atrAvg += iATR(TradingSymbol,0,ATRPeriod,i);
   atrAvg /= ATRPeriod;
   if(atrAvg<=0) return true;
   bool ok = (atrNow <= ATRMultiplier * atrAvg);
   if(!ok) Print("ATR BLOQUÉ curr=",atrNow," seuil=",ATRMultiplier*atrAvg);
   return ok;
}

//==================================================================
//  SAFE SEND
//==================================================================
int SafeSend(int type, double lot, double price, string comment, color clr) {
   price = NP(price);
   if(MarketInfo(TradingSymbol,MODE_MARGINREQUIRED)*lot > AccountFreeMargin()) {
      Print("Marge insuffisante lot=",lot); return -1;
   }
   string fc = comment+"#"+IntegerToString(++g_orderCounter);
   int t = OrderSend(TradingSymbol,type,lot,price,Slippage,0,0,fc,Magic,0,clr);
   if(t<0) Print("ERR OrderSend type=",type," err=",GetLastError()," px=",price);
   else    Print("OK t=",t," type=",type," lot=",lot," px=",price);
   return t;
}

//==================================================================
//  INIT ARRAYS
//==================================================================
void InitPairArrays() {
   g_numPairs=0;
   for(int i=0;i<MAX_PAIRS;i++) {
      g_pairSSTicket[i]=-1; g_pairBSTicket[i]=-1;
      g_pairSSLevel[i]=0;   g_pairBSLevel[i]=0;
      g_pairLot[i]=0;       g_pairRecovLvl[i]=0;
      g_pairSSFired[i]=false; g_pairBSFired[i]=false;
      g_pairSSTrigPx[i]=0;  g_pairBSTrigPx[i]=0;
      g_pairRecovDone[i]=false;
   }
}

//==================================================================
//  COMMENT ENCODING / PARSING
//  Format : "Lxx_SS" ou "Lxx_BS"  (xx = niveau 00-20)
//==================================================================
string MakeComment(int level, bool isSS) {
   return "L"+IntegerToString(level)+(isSS?"_SS":"_BS");
}

int ParseLevel(string comment) {
   int lPos = StringFind(comment,"L");
   int uPos = StringFind(comment,"_");
   if(lPos<0 || uPos<=lPos+1) return 0;
   return (int)StringToInteger(StringSubstr(comment,lPos+1,uPos-lPos-1));
}

bool CommentIsSS(string comment) { return (StringFind(comment,"_SS")>=0); }

//==================================================================
//  PLACE HEDGE PAIR (générique)
//  Initial : level=0, ssTarget=Bid-HalfGap, bsTarget=Ask+HalfGap
//  Recovery : level>0, prix calculés dans PlaceRecoveryHedge
//==================================================================
bool PlaceHedgePair(int level, double ssTarget, double bsTarget) {
   if(g_numPairs>=MAX_PAIRS) { Print("MAX_PAIRS atteint"); return false; }
   RefreshRates();
   double minD = MinDist();
   if(Bid-ssTarget < minD)   ssTarget = NP(Bid-minD);
   if(bsTarget-Ask < minD)   bsTarget = NP(Ask+minD);
   ssTarget = NP(ssTarget);
   bsTarget = NP(bsTarget);

   double lot = RecovLot(level);
   int ssT = SafeSend(OP_SELLSTOP, lot, ssTarget, MakeComment(level,true),  clrCrimson);
   int bsT = SafeSend(OP_BUYSTOP,  lot, bsTarget, MakeComment(level,false), clrDodgerBlue);

   if(ssT>0 && bsT>0) {
      int idx = g_numPairs;
      g_pairSSTicket[idx]=ssT;   g_pairBSTicket[idx]=bsT;
      g_pairSSLevel[idx]=ssTarget; g_pairBSLevel[idx]=bsTarget;
      g_pairLot[idx]=lot;        g_pairRecovLvl[idx]=level;
      g_pairSSFired[idx]=false;  g_pairBSFired[idx]=false;
      g_pairSSTrigPx[idx]=0;     g_pairBSTrigPx[idx]=0;
      g_pairRecovDone[idx]=false;
      g_numPairs++;
      Print("HedgePair L",level," SS=",ssTarget," BS=",bsTarget," lot=",lot);
      return true;
   }
   // Rollback si un seul côté placé
   if(ssT>0 && bsT<0) OrderDelete(ssT);
   if(bsT>0 && ssT<0) OrderDelete(bsT);
   Print("ERREUR placement HedgePair L",level);
   return false;
}

void PlaceInitialHedge() {
   RefreshRates();
   double ss = NP(Bid - HalfGapPx());
   double bs = NP(Ask + HalfGapPx());
   PlaceHedgePair(0, ss, bs);
}

//==================================================================
//  PLACE RECOVERY HEDGE
//  Conditions : les DEUX stops du parent déclenchés
//             + basket négatif
//             + MinMove respecté (Option B)
//             + ATR ok (Option C)
//  Placement  : outside du parent → SS en dessous, BS au-dessus
//==================================================================
void PlaceRecoveryHedge(int parentIdx) {
   if(g_numPairs>=MAX_PAIRS)              { Print("MAX_PAIRS atteint"); return; }
   if(!ATRFilterOK())                     return;
   int newLevel = g_pairRecovLvl[parentIdx]+1;
   if(newLevel>MaxRecovery)               { Print("MaxRecovery atteint"); return; }

   // Utiliser les prix de déclenchement enregistrés
   double ssTrig = g_pairSSTrigPx[parentIdx];
   double bsTrig = g_pairBSTrigPx[parentIdx];
   if(ssTrig<=0) ssTrig = g_pairSSLevel[parentIdx];
   if(bsTrig<=0) bsTrig = g_pairBSLevel[parentIdx];

   // Outside : SS plus bas que le SS parent, BS plus haut que le BS parent
   double ssNew = NP(ssTrig - RecovOffPx());
   double bsNew = NP(bsTrig + RecovOffPx());

   if(PlaceHedgePair(newLevel, ssNew, bsNew))
      g_pairRecovDone[parentIdx] = true;
}

//==================================================================
//  TRAIL ALL PAIRS — Règle ratchet Pure Hedge
//
//  SS ratchet UPWARD  : suit le prix qui monte (s'éloigne du SS)
//    → bouge si target > ssLevel + TRAIL_STEP*Point
//  BS ratchet DOWNWARD: suit le prix qui descend (s'éloigne du BS)
//    → bouge si target < bsLevel - TRAIL_STEP*Point
//
//  Distance trailing : TrailDistPx() (serrée, par défaut 150 pts)
//  Si stop disparu sans avoir firé → recréation immédiate
//==================================================================
void TrailAllPairs() {
   RefreshRates();
   double minD  = MinDist();
   double step  = TRAIL_STEP * Point;

   for(int i=0;i<g_numPairs;i++) {

      //--- Trail SELLSTOP si encore pending
      if(!g_pairSSFired[i]) {
         if(IsPendingSS(g_pairSSTicket[i])) {
            double ssTarget = NP(Bid - TrailDistPx());
            if(Bid-ssTarget < minD) ssTarget = NP(Bid-minD);
            // Ratchet : SS ne monte que
            if(ssTarget > g_pairSSLevel[i]+step) {
               if(OrderModify(g_pairSSTicket[i],ssTarget,0,0,0,clrOrangeRed))
                  g_pairSSLevel[i] = ssTarget;
            }
         } else {
            // SS disparu sans déclencher → recréer
            double ssTarget = NP(Bid - TrailDistPx());
            if(Bid-ssTarget < minD) ssTarget = NP(Bid-minD);
            Print("Pair[",i,"] SS disparu → recréation @ ",ssTarget);
            int newT = SafeSend(OP_SELLSTOP, g_pairLot[i], ssTarget,
                                MakeComment(g_pairRecovLvl[i],true), clrCrimson);
            if(newT>0) { g_pairSSTicket[i]=newT; g_pairSSLevel[i]=ssTarget; }
         }
      }

      //--- Trail BUYSTOP si encore pending
      if(!g_pairBSFired[i]) {
         if(IsPendingBS(g_pairBSTicket[i])) {
            double bsTarget = NP(Ask + TrailDistPx());
            if(bsTarget-Ask < minD) bsTarget = NP(Ask+minD);
            // Ratchet : BS ne descend que
            if(bsTarget < g_pairBSLevel[i]-step) {
               if(OrderModify(g_pairBSTicket[i],bsTarget,0,0,0,clrCornflowerBlue))
                  g_pairBSLevel[i] = bsTarget;
            }
         } else {
            // BS disparu sans déclencher → recréer
            double bsTarget = NP(Ask + TrailDistPx());
            if(bsTarget-Ask < minD) bsTarget = NP(Ask+minD);
            Print("Pair[",i,"] BS disparu → recréation @ ",bsTarget);
            int newT = SafeSend(OP_BUYSTOP, g_pairLot[i], bsTarget,
                                MakeComment(g_pairRecovLvl[i],false), clrDodgerBlue);
            if(newT>0) { g_pairBSTicket[i]=newT; g_pairBSLevel[i]=bsTarget; }
         }
      }
   }
}

//==================================================================
//  DETECT FIRED STOPS
//  En MT4 : quand un stop se déclenche, le ticket reste le même
//  mais le type passe de OP_SELLSTOP → OP_SELL (ou BUYSTOP→BUY)
//==================================================================
void DetectFired() {
   for(int i=0;i<g_numPairs;i++) {
      if(!g_pairSSFired[i] && g_pairSSTicket[i]>0) {
         if(IsFiredSell(g_pairSSTicket[i])) {
            g_pairSSFired[i]=true;
            if(OrderSelect(g_pairSSTicket[i],SELECT_BY_TICKET))
               g_pairSSTrigPx[i]=OrderOpenPrice();
            Print("Pair[",i,"] L",g_pairRecovLvl[i]," SS DÉCLENCHÉ @ ",g_pairSSTrigPx[i]);
         }
      }
      if(!g_pairBSFired[i] && g_pairBSTicket[i]>0) {
         if(IsFiredBuy(g_pairBSTicket[i])) {
            g_pairBSFired[i]=true;
            if(OrderSelect(g_pairBSTicket[i],SELECT_BY_TICKET))
               g_pairBSTrigPx[i]=OrderOpenPrice();
            Print("Pair[",i,"] L",g_pairRecovLvl[i]," BS DÉCLENCHÉ @ ",g_pairBSTrigPx[i]);
         }
      }
   }
}

//==================================================================
//  DETECT ORPHANS
//  Orphelin = ordre de marché sans son stop pair pendant
//  → Recréer immédiatement le stop manquant
//  Pure Hedge : jamais un seul côté sans protection
//==================================================================
void DetectOrphans() {
   RefreshRates();
   double minD = MinDist();

   for(int i=0;i<g_numPairs;i++) {
      // SS fired → SELL ouvert. BS doit rester pending.
      if(g_pairSSFired[i] && !g_pairBSFired[i]) {
         if(!IsPendingBS(g_pairBSTicket[i])) {
            double bsTarget = NP(Ask + TrailDistPx());
            if(bsTarget-Ask < minD) bsTarget = NP(Ask+minD);
            Print("ORPHELIN Pair[",i,"] BS manquant → recréation @ ",bsTarget);
            int newT = SafeSend(OP_BUYSTOP, g_pairLot[i], bsTarget,
                                MakeComment(g_pairRecovLvl[i],false), clrDodgerBlue);
            if(newT>0) { g_pairBSTicket[i]=newT; g_pairBSLevel[i]=bsTarget; }
         }
      }
      // BS fired → BUY ouvert. SS doit rester pending.
      if(g_pairBSFired[i] && !g_pairSSFired[i]) {
         if(!IsPendingSS(g_pairSSTicket[i])) {
            double ssTarget = NP(Bid - TrailDistPx());
            if(Bid-ssTarget < minD) ssTarget = NP(Bid-minD);
            Print("ORPHELIN Pair[",i,"] SS manquant → recréation @ ",ssTarget);
            int newT = SafeSend(OP_SELLSTOP, g_pairLot[i], ssTarget,
                                MakeComment(g_pairRecovLvl[i],true), clrCrimson);
            if(newT>0) { g_pairSSTicket[i]=newT; g_pairSSLevel[i]=ssTarget; }
         }
      }
   }
}

//==================================================================
//  CHECK RECOVERY
//  Déclenche un nouveau hedge pair si :
//  1. Les DEUX stops de la paire sont fired
//  2. BasketProfit encore négatif (pas encore sauvé)
//  3. MinMoveAfterTrigger respecté (Option B)
//  4. ATR dans les limites (Option C)
//  5. Recovery pas déjà posé pour cette paire
//==================================================================
void CheckRecovery() {
   if(BasketProfit() >= BasketTarget) return;

   for(int i=0;i<g_numPairs;i++) {
      if(g_pairRecovDone[i])             continue;
      if(!g_pairSSFired[i])              continue;
      if(!g_pairBSFired[i])              continue;

      // Option B : prix doit s'être éloigné d'au moins MinMoveAfterTrigger
      double trigMid = (g_pairSSTrigPx[i]+g_pairBSTrigPx[i])/2.0;
      double movePts = MathAbs(Bid-trigMid)/Point;
      if(movePts < MinMoveAfterTrigger) {
         // Pas encore assez de mouvement
         continue;
      }

      // Option C : ATR
      if(!ATRFilterOK()) continue;

      // Toutes conditions remplies
      PlaceRecoveryHedge(i);
   }
}

//==================================================================
//  RESTORE STATE après redémarrage
//  Scan des ordres ouverts → reconstruction de g_pairs[]
//  via commentaire "Lxx_SS" / "Lxx_BS"
//==================================================================
void RestoreState() {
   InitPairArrays();

   // Collecte par niveau
   for(int lev=0;lev<=MaxRecovery;lev++) {
      int    ssT=-1, bsT=-1;
      double ssL=0,  bsL=0, lot=0;
      bool   ssF=false, bsF=false;
      double ssTrig=0, bsTrig=0;

      for(int i=OrdersTotal()-1;i>=0;i--) {
         if(!OrderSelect(i,SELECT_BY_POS,MODE_TRADES)) continue;
         if(OrderMagicNumber()!=Magic||OrderSymbol()!=TradingSymbol) continue;
         int    type = OrderType();
         string cmt  = OrderComment();
         int    cLev = ParseLevel(cmt);
         if(cLev != lev) continue;

         bool isSS = CommentIsSS(cmt);
         bool fired = (type==OP_SELL || type==OP_BUY);

         if(isSS) {
            ssT=OrderTicket(); ssL=OrderOpenPrice(); lot=OrderLots(); ssF=fired;
            if(fired) ssTrig=OrderOpenPrice();
         } else {
            bsT=OrderTicket(); bsL=OrderOpenPrice(); if(lot<=0) lot=OrderLots(); bsF=fired;
            if(fired) bsTrig=OrderOpenPrice();
         }
      }

      if(ssT>0 || bsT>0) {
         int idx=g_numPairs;
         g_pairSSTicket[idx]=ssT;     g_pairBSTicket[idx]=bsT;
         g_pairSSLevel[idx]=ssL;      g_pairBSLevel[idx]=bsL;
         g_pairLot[idx]=(lot>0?lot:RecovLot(lev));
         g_pairRecovLvl[idx]=lev;
         g_pairSSFired[idx]=ssF;      g_pairBSFired[idx]=bsF;
         g_pairSSTrigPx[idx]=ssTrig;  g_pairBSTrigPx[idx]=bsTrig;
         g_pairRecovDone[idx]=(ssF && bsF); // si les deux fired, recovery à évaluer
         g_numPairs++;
         Print("Restauré pair[",idx,"] L",lev," SS=",ssL," BS=",bsL,
               " ssF=",ssF," bsF=",bsF);
         if(g_numPairs>=MAX_PAIRS) break;
      }
   }

   g_state = STATE_ACTIVE;
   Print("RestoreState : ",g_numPairs," paires restaurées");
}

//==================================================================
//  TRI LOT DÉCROISSANT
//==================================================================
void SortByLotDesc(int &t[], double &l[]) {
   int n=ArraySize(t);
   for(int i=0;i<n-1;i++) for(int j=i+1;j<n;j++) if(l[j]>l[i]) {
      double tL=l[i]; l[i]=l[j]; l[j]=tL;
      int    tT=t[i]; t[i]=t[j]; t[j]=tT;
   }
}

//==================================================================
//  TRI P&L ASCENDANT (losers first → protège le profit)
//==================================================================
void SortByPnLAsc(int &t[], double &p[], double &l[]) {
   int n=ArraySize(t);
   for(int i=0;i<n-1;i++) for(int j=i+1;j<n;j++) if(p[j]<p[i]) {
      double tP=p[i]; p[i]=p[j]; p[j]=tP;
      double tL=l[i]; l[i]=l[j]; l[j]=tL;
      int    tT=t[i]; t[i]=t[j]; t[j]=tT;
   }
}

//==================================================================
//  FERMETURE BASKET ADAPTATIVE — PIPELINE 4 PHASES
//  Zone A (2-4)  : RefreshRates avant chaque single
//  Zone B (5-15) : RefreshRates tous les 3 singles
//  Zone C (16+)  : RefreshRates tous les 3 singles
//  Phase 1       : CloseBy paires (δ=0, 0 spread, profit verrouillé)
//  Phase 2       : Singles triés P&L asc (losers d'abord)
//  Phase 3       : Safety net 200ms + retry ×3
//==================================================================
void CloseBasketAtomic() {
   g_state = STATE_CLOSING;
   int total = CountMarketOrders();
   int ri    = (total<=4)?1:3; // refreshInterval

   Print(">>> CloseBasket ZONE ",(total<=4?"A":total<=15?"B":"C"),
         " ordres=",total);

   // PHASE 0 — PURGE TOUS LES STOPS
   for(int i=OrdersTotal()-1;i>=0;i--) {
      if(!OrderSelect(i,SELECT_BY_POS,MODE_TRADES)) continue;
      if(OrderMagicNumber()!=Magic||OrderSymbol()!=TradingSymbol) continue;
      if(OrderType()==OP_BUYSTOP||OrderType()==OP_SELLSTOP) OrderDelete(OrderTicket());
   }

   // COLLECTE avec P&L
   int    bT[],sT[]; double bL[],sL[],bP[],sP[];
   ArrayResize(bT,0);ArrayResize(sT,0);ArrayResize(bL,0);
   ArrayResize(sL,0);ArrayResize(bP,0);ArrayResize(sP,0);

   for(int i=OrdersTotal()-1;i>=0;i--) {
      if(!OrderSelect(i,SELECT_BY_POS,MODE_TRADES)) continue;
      if(OrderMagicNumber()!=Magic||OrderSymbol()!=TradingSymbol) continue;
      if(OrderType()==OP_BUY) {
         int x=ArraySize(bT); ArrayResize(bT,x+1);ArrayResize(bL,x+1);ArrayResize(bP,x+1);
         bT[x]=OrderTicket();bL[x]=OrderLots();
         bP[x]=OrderProfit()+OrderSwap()+OrderCommission();
      } else if(OrderType()==OP_SELL) {
         int x=ArraySize(sT); ArrayResize(sT,x+1);ArrayResize(sL,x+1);ArrayResize(sP,x+1);
         sT[x]=OrderTicket();sL[x]=OrderLots();
         sP[x]=OrderProfit()+OrderSwap()+OrderCommission();
      }
   }

   SortByLotDesc(bT,bL); SortByLotDesc(sT,sL);
   int nB=ArraySize(bT), nS=ArraySize(sT);
   bool bU[]; ArrayResize(bU,nB); ArrayInitialize(bU,0);
   bool sU[]; ArrayResize(sU,nS); ArrayInitialize(sU,0);

   // PHASE 1 — NETTING CloseBy
   int pairs=0;
   for(int b=0;b<nB;b++) {
      if(bU[b]) continue;
      for(int s=0;s<nS;s++) {
         if(sU[s]) continue;
         if(MathAbs(bL[b]-sL[s])<0.005) {
            if(OrderCloseBy(bT[b],sT[s],clrYellow)) {
               bU[b]=true;sU[s]=true;pairs++;
            }
            break;
         }
      }
   }

   // PHASE 2 — FLUSH SINGLES P&L ascendant
   int    singT[]; double singL[],singP[];
   ArrayResize(singT,0);ArrayResize(singL,0);ArrayResize(singP,0);
   for(int b=0;b<nB;b++) { if(bU[b]) continue;
      int x=ArraySize(singT);ArrayResize(singT,x+1);ArrayResize(singL,x+1);ArrayResize(singP,x+1);
      singT[x]=bT[b];singL[x]=bL[b];singP[x]=bP[b];
   }
   for(int s=0;s<nS;s++) { if(sU[s]) continue;
      int x=ArraySize(singT);ArrayResize(singT,x+1);ArrayResize(singL,x+1);ArrayResize(singP,x+1);
      singT[x]=sT[s];singL[x]=sL[s];singP[x]=sP[s];
   }
   SortByPnLAsc(singT,singP,singL);
   int nSing=ArraySize(singT);
   for(int k=0;k<nSing;k++) {
      if(k==0||k%ri==0) RefreshRates();
      if(!OrderSelect(singT[k],SELECT_BY_TICKET)) continue;
      if(OrderCloseTime()>0) continue;
      double cp=(OrderType()==OP_BUY)?Bid:Ask;
      OrderClose(singT[k],singL[k],cp,Slippage,(OrderType()==OP_BUY?clrBlue:clrRed));
   }

   // PHASE 3 — SAFETY NET
   Sleep(200); RefreshRates();
   for(int a=0;a<3;a++) {
      int res=0;
      for(int i=OrdersTotal()-1;i>=0;i--) {
         if(!OrderSelect(i,SELECT_BY_POS,MODE_TRADES)) continue;
         if(OrderMagicNumber()!=Magic||OrderSymbol()!=TradingSymbol) continue;
         if(OrderType()==OP_BUY||OrderType()==OP_SELL) {
            res++; RefreshRates();
            double cp=(OrderType()==OP_BUY)?Bid:Ask;
            OrderClose(OrderTicket(),OrderLots(),cp,Slippage,clrMagenta);
         }
      }
      if(res==0) break;
      Sleep(100);
   }

   InitPairArrays();
   g_state = STATE_INIT;
   Print("=== BASKET FERMÉ pairs=",pairs," singles=",nSing," ===");
}

//==================================================================
//  OBJECTIF JOURNALIER
//==================================================================
void CheckDailyTarget() {
   if(DailyTargetPercent<=0.0) return;
   MqlDateTime td; TimeToStruct(TimeCurrent(),td);
   td.hour=0;td.min=0;td.sec=0;
   datetime dayStart=StructToTime(td);
   if(g_dailyResetTime<dayStart) {
      g_dailyStartBalance=AccountBalance();
      g_dailyResetTime=dayStart;
      g_dailyTargetReached=false;
      Print("Daily reset bal=",g_dailyStartBalance);
   }
   if(g_dailyTargetReached) return;
   if(AccountBalance()-g_dailyStartBalance >= g_dailyStartBalance*DailyTargetPercent/100.0) {
      Print("Objectif journalier atteint");
      CloseBasketAtomic();
      g_dailyTargetReached=true;
   }
}

//==================================================================
//  DASHBOARD
//==================================================================
void DrawDashboard() {
   double profit  = BasketProfit();
   double freePct = AccountBalance()>0?AccountFreeMargin()/AccountBalance()*100.0:0;
   int    mkt     = CountMarketOrders();
   string profStr = (profit>=0?"+":"")+DoubleToStr(profit,2);
   string stStr   = (g_state==STATE_INIT?"INIT":g_state==STATE_ACTIVE?"ACTIF":"CLOSING");
   string zone    = mkt<=4?"A":mkt<=15?"B":"C";

   string pInfo="";
   for(int i=0;i<g_numPairs;i++) {
      string ss = g_pairSSFired[i]?"✓":"○";
      string bs = g_pairBSFired[i]?"✓":"○";
      string rd = g_pairRecovDone[i]?"→R":"";
      pInfo += "║  H"+IntegerToString(g_pairRecovLvl[i])+" SS"+ss
              +" BS"+bs+" lot="+DoubleToStr(g_pairLot[i],2)+rd+"\n";
   }
   string dInfo="";
   if(DailyTargetPercent>0) {
      double pct=(g_dailyStartBalance>0)?
                 (AccountBalance()-g_dailyStartBalance)/g_dailyStartBalance*100.0:0;
      dInfo="║  Daily: "+(g_dailyTargetReached?"ATTEINT":
            StringFormat("%.1f%% / %.1f%%",pct,DailyTargetPercent))+"\n";
   }

   Comment(
      "╔═══════════════════════════════╗\n"
      "║    TOP5EA — XAUUSDm           ║\n"
      "╠═══════════════════════════════╣\n"
      "║  État   : "+stStr+"  [ZONE "+zone+"]\n"
      "║  P&L    : "+profStr+" $\n"
      "║  Target : "+DoubleToStr(BasketTarget,2)+" $\n"
      "║  Ordres : "+IntegerToString(mkt)+" | Pairs: "+IntegerToString(g_numPairs)+"\n"
      "╠═══════════════════════════════╣\n"
      +pInfo+
      "╠═══════════════════════════════╣\n"
      +dInfo+
      "║  Marge  : "+DoubleToStr(freePct,1)+" %\n"
      "║  ATR OK : "+(ATRFilterOK()?"OUI":"NON — BLOQUÉ")+"\n"
      "╚═══════════════════════════════╝"
   );
}

//==================================================================
//  OnTick
//==================================================================
void OnTick() {
   RefreshRates();

   // 1. Objectif journalier
   CheckDailyTarget();
   if(g_dailyTargetReached) { DrawDashboard(); return; }

   if(TimeCurrent()==g_lastAction) { DrawDashboard(); return; }
   g_lastAction=TimeCurrent();

   // 2. Basket target → fermeture atomique
   if(HasMarketOrders() && BasketProfit()>=BasketTarget) {
      CloseBasketAtomic();
      DrawDashboard();
      return;
   }

   // 3. Init : restaurer ou placer hedge initial
   if(g_state==STATE_INIT) {
      bool hasAny=false;
      for(int i=OrdersTotal()-1;i>=0;i--)
         if(OrderSelect(i,SELECT_BY_POS,MODE_TRADES))
            if(OrderMagicNumber()==Magic && OrderSymbol()==TradingSymbol)
               { hasAny=true; break; }
      if(hasAny) RestoreState();
      else { PlaceInitialHedge(); g_state=STATE_ACTIVE; }
      return;
   }

   // 4. Détecter déclenchements
   DetectFired();

   // 5. Détecter et corriger orphelins
   DetectOrphans();

   // 6. Vérifier si recovery nécessaire
   CheckRecovery();

   // 7. Trailer toutes les paires (ratchet serré)
   TrailAllPairs();

   DrawDashboard();
}

//==================================================================
//  OnInit
//==================================================================
int OnInit() {
   if(MarketInfo(TradingSymbol,MODE_BID)==0) {
      Print("ERREUR symbole invalide : ",TradingSymbol);
      return INIT_FAILED;
   }
   InitPairArrays();
   g_state=STATE_INIT; g_orderCounter=0;
   MqlDateTime now; TimeToStruct(TimeCurrent(),now);
   now.hour=0;now.min=0;now.sec=0;
   g_dailyResetTime=StructToTime(now);
   g_dailyStartBalance=AccountBalance();
   g_dailyTargetReached=false;
   Print("TOP5EA XAUUSDm v7.00 initialisé");
   Print("GridGap=",GridGap," Trail=",TrailDistance," RecovOff=",RecoveryOffset);
   Print("MaxLot=",MaxLot," MinMove=",MinMoveAfterTrigger," ATR ",ATRPeriod,"×",ATRMultiplier);
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason) { Comment(""); }
//+------------------------------------------------------------------+
