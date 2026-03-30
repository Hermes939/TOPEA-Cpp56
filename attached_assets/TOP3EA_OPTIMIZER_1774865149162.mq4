//+------------------------------------------------------------------+
//|  TOP3EA_OPTIMIZER.mq4                                            |
//|  Script MQL4 — Lit TOP3EA_LOG.csv et génère recommandations     |
//|  Optimise : GridGap, MaxRecovery, LotMultiplier                 |
//|  Lancer manuellement après une session de trading                |
//+------------------------------------------------------------------+
#property copyright "TOP3EA"
#property version   "1.00"
#property strict
#property show_inputs

extern string LogFile        = "TOP3EA_LOG.csv";
extern int    MinSamples     = 10;    // Minimum d'événements pour analyser
extern double TargetRatio    = 1.5;   // Ratio profit/recovery cible minimum
extern string OutputFile     = "TOP3EA_RECOMMENDATIONS.txt";

//+------------------------------------------------------------------+
//| Structure pour stocker une ligne CSV                            |
//+------------------------------------------------------------------+
struct LogRow {
   datetime timestamp;
   string   eventType;
   double   profit;
   double   drawdown;
   double   peakProfit;
   int      recoveryLevel;
   double   maxLot;
   double   atr;
   int      gridgap;
   double   freeMarginPct;
   int      buys;
   int      sells;
};

//+------------------------------------------------------------------+
//| Parse une ligne CSV                                             |
//+------------------------------------------------------------------+
bool ParseRow(string line, LogRow &row) {
   string parts[];
   int count = StringSplit(line, ',', parts);
   if(count < 17) return false;

   row.timestamp      = StringToTime(parts[0]);
   row.eventType      = parts[1];
   row.profit         = StringToDouble(parts[9]);
   row.drawdown       = StringToDouble(parts[10]);
   row.peakProfit     = StringToDouble(parts[11]);
   row.recoveryLevel  = (int)StringToInteger(parts[12]);
   row.maxLot         = StringToDouble(parts[13]);
   row.atr            = StringToDouble(parts[14]);
   row.gridgap        = (int)StringToInteger(parts[15]);
   row.freeMarginPct  = StringToDouble(parts[16]);
   row.buys           = (int)StringToInteger(parts[7]);
   row.sells          = (int)StringToInteger(parts[8]);
   return true;
}

//+------------------------------------------------------------------+
//| Analyse principale                                              |
//+------------------------------------------------------------------+
void OnStart() {
   int fh = FileOpen(LogFile, FILE_READ|FILE_CSV|FILE_COMMON);
   if(fh == INVALID_HANDLE) {
      Print("ERREUR: Impossible d'ouvrir ", LogFile, " err=", GetLastError());
      return;
   }

   // --- Variables d'analyse
   int    totalRows       = 0;
   int    recoveryEvents  = 0;
   int    targetHits      = 0;
   double totalProfit     = 0;
   double maxDrawdown     = 0;
   double sumATR          = 0;
   int    atrCount        = 0;
   double sumGridGap      = 0;
   int    maxRecovSeen    = 0;
   double minFreeMargin   = 100;
   double sumFreeMargin   = 0;

   // Pour corrélation ATR / GridGap efficacité
   double atrLow_profit = 0,  atrLow_dd = 0;   int atrLow_cnt = 0;
   double atrMid_profit = 0,  atrMid_dd = 0;   int atrMid_cnt = 0;
   double atrHigh_profit = 0, atrHigh_dd = 0;  int atrHigh_cnt = 0;

   // Pour analyse MaxRecovery
   int    recovDistrib[21]; // compteur par niveau 0-20
   ArrayInitialize(recovDistrib, 0);

   // Sauter l'entête
   FileReadString(fh);

   // Lire toutes les lignes
   while(!FileIsEnding(fh)) {
      string line = FileReadString(fh);
      if(StringLen(line) < 10) continue;

      LogRow row;
      if(!ParseRow(line, row)) continue;
      totalRows++;

      // Stats générales
      if(row.profit > totalProfit) totalProfit = row.profit;
      if(row.drawdown > maxDrawdown) maxDrawdown = row.drawdown;
      if(row.freeMarginPct < minFreeMargin) minFreeMargin = row.freeMarginPct;
      sumFreeMargin += row.freeMarginPct;

      if(row.recoveryLevel > maxRecovSeen) maxRecovSeen = row.recoveryLevel;
      if(row.recoveryLevel <= 20) recovDistrib[row.recoveryLevel]++;

      // ATR stats
      if(row.atr > 0) {
         sumATR += row.atr;
         sumGridGap += row.gridgap;
         atrCount++;

         if(row.atr < 400) {
            atrLow_profit += row.profit; atrLow_dd += row.drawdown; atrLow_cnt++;
         } else if(row.atr < 700) {
            atrMid_profit += row.profit; atrMid_dd += row.drawdown; atrMid_cnt++;
         } else {
            atrHigh_profit += row.profit; atrHigh_dd += row.drawdown; atrHigh_cnt++;
         }
      }

      // Événements
      if(row.eventType == "RECOVERY")    recoveryEvents++;
      if(row.eventType == "TARGET_HIT")  targetHits++;
   }

   FileClose(fh);

   if(totalRows < MinSamples) {
      Print("Pas assez de données (", totalRows, " lignes, minimum ", MinSamples, ")");
      return;
   }

   // --- Calculs dérivés
   double avgATR       = atrCount > 0 ? sumATR / atrCount : 0;
   double avgFreeMargin= totalRows > 0 ? sumFreeMargin / totalRows : 0;
   double ratio        = recoveryEvents > 0 ? totalProfit / recoveryEvents : 0;

   // GridGap optimal selon ATR
   int optimalGridGap;
   if(avgATR < 200)      optimalGridGap = 200;
   else if(avgATR < 400) optimalGridGap = 300;
   else if(avgATR < 700) optimalGridGap = 500;
   else if(avgATR < 1000)optimalGridGap = 700;
   else                  optimalGridGap = 1000;

   // MaxRecovery optimal — niveau où 90% des trades se terminent
   int optimalMaxRecov = maxRecovSeen;
   int cumul = 0;
   for(int i = 0; i <= 20; i++) {
      cumul += recovDistrib[i];
      if(totalRows > 0 && cumul * 1.0 / totalRows >= 0.90) {
         optimalMaxRecov = i + 2; // marge de sécurité +2
         break;
      }
   }

   // LotMultiplier optimal
   // Si drawdown élevé → réduire, si ratio bon → maintenir
   double optimalLotMult;
   if(maxDrawdown > 100)     optimalLotMult = 1.10;
   else if(maxDrawdown > 50) optimalLotMult = 1.15;
   else if(ratio > TargetRatio) optimalLotMult = 1.20;
   else                      optimalLotMult = 1.15;

   // --- Écriture rapport
   int out = FileOpen(OutputFile, FILE_WRITE|FILE_TXT|FILE_COMMON);
   if(out == INVALID_HANDLE) {
      Print("ERREUR: Impossible d'écrire ", OutputFile);
      return;
   }

   string sep = "================================================\n";
   string report = "";
   report += sep;
   report += "   TOP3EA OPTIMIZER — RAPPORT D'ANALYSE\n";
   report += "   Généré le : " + TimeToStr(TimeCurrent(), TIME_DATE|TIME_SECONDS) + "\n";
   report += sep;
   report += "\n[DONNÉES ANALYSÉES]\n";
   report += "  Événements totaux  : " + IntegerToString(totalRows) + "\n";
   report += "  Recovery events    : " + IntegerToString(recoveryEvents) + "\n";
   report += "  Target hits        : " + IntegerToString(targetHits) + "\n";
   report += "\n[PERFORMANCE]\n";
   report += "  Profit cumulé      : " + DoubleToStr(totalProfit, 2) + " $\n";
   report += "  Drawdown maximum   : " + DoubleToStr(maxDrawdown, 2) + " $\n";
   report += "  Ratio P/Recovery   : " + DoubleToStr(ratio, 2) + " $ (cible: " + DoubleToStr(TargetRatio,2) + ")\n";
   report += "  Marge libre min    : " + DoubleToStr(minFreeMargin, 1) + " %\n";
   report += "  Marge libre moy    : " + DoubleToStr(avgFreeMargin, 1) + " %\n";
   report += "\n[VOLATILITÉ]\n";
   report += "  ATR14 moyen        : " + DoubleToStr(avgATR, 0) + " pts\n";
   if(atrLow_cnt > 0)  report += "  ATR faible (<400)  : profit=" + DoubleToStr(atrLow_profit/atrLow_cnt,2) + " dd=" + DoubleToStr(atrLow_dd/atrLow_cnt,2) + "\n";
   if(atrMid_cnt > 0)  report += "  ATR moyen (400-700): profit=" + DoubleToStr(atrMid_profit/atrMid_cnt,2) + " dd=" + DoubleToStr(atrMid_dd/atrMid_cnt,2) + "\n";
   if(atrHigh_cnt > 0) report += "  ATR élevé (>700)   : profit=" + DoubleToStr(atrHigh_profit/atrHigh_cnt,2) + " dd=" + DoubleToStr(atrHigh_dd/atrHigh_cnt,2) + "\n";
   report += "\n[DISTRIBUTION RECOVERY]\n";
   for(int i = 0; i <= MathMin(maxRecovSeen, 20); i++)
      if(recovDistrib[i] > 0)
         report += "  Niveau " + IntegerToString(i) + " : " + IntegerToString(recovDistrib[i]) + " fois\n";
   report += "\n" + sep;
   report += "   ★ RECOMMANDATIONS OPTIMALES ★\n";
   report += sep;
   report += "  GridGap      : " + IntegerToString(optimalGridGap) + " pts"
           + (optimalGridGap != (int)sumGridGap/MathMax(atrCount,1) ? "  ◄ AJUSTER" : "  ✓ OK") + "\n";
   report += "  MaxRecovery  : " + IntegerToString(optimalMaxRecov) + "\n";
   report += "  LotMultiplier: " + DoubleToStr(optimalLotMult, 2) + "\n";
   report += "\n[ÉVALUATION GLOBALE]\n";
   if(ratio >= TargetRatio && maxDrawdown < 50 && minFreeMargin > 20)
      report += "  ✅ Système performant — paramètres bien calibrés\n";
   else if(maxDrawdown > 100 || minFreeMargin < 10)
      report += "  🔴 RISQUE ÉLEVÉ — réduire LotMultiplier et MaxRecovery\n";
   else
      report += "  🟡 Performances correctes — ajustements suggérés ci-dessus\n";
   report += sep;

   FileWriteString(out, report);
   FileClose(out);

   Print("=== RAPPORT GÉNÉRÉ → Common/", OutputFile, " ===");
   Print(report);
}
//+------------------------------------------------------------------+
