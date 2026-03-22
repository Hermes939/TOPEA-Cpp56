//+------------------------------------------------------------------+
//|  TOP4EA_Bridge.mq4                                               |
//|  Bridge minimal MT4 ↔ C++20 TCP — JSON-ligne                    |
//|  Ce script UNIQUEMENT transmet ordres/ticks — AUCUNE logique EA  |
//+------------------------------------------------------------------+
#property copyright "TOP4EA"
#property version   "1.00"
#property strict

extern string BridgeHost = "127.0.0.1";
extern int    BridgePort = 9000;
extern int    Magic      = 10300;
extern string Symbol_    = "XAUUSDm";

int    g_socket    = INVALID_HANDLE;
bool   g_connected = false;
string g_recvBuf   = "";
datetime g_lastPing = 0;

//+------------------------------------------------------------------+
int OnInit() {
   g_socket = SocketCreate();
   if(g_socket == INVALID_HANDLE) {
      Print("ERREUR: SocketCreate échoué");
      return INIT_FAILED;
   }
   if(!SocketConnect(g_socket, BridgeHost, BridgePort, 3000)) {
      Print("ERREUR: Connexion au C++20 server échouée ", BridgeHost, ":", BridgePort);
      return INIT_FAILED;
   }
   g_connected = true;
   Print("Bridge connecté → C++20 server ", BridgeHost, ":", BridgePort);
   return INIT_SUCCEEDED;
}

//+------------------------------------------------------------------+
void OnDeinit(const int reason) {
   if(g_socket != INVALID_HANDLE) SocketClose(g_socket);
   g_connected = false;
   Print("Bridge déconnecté");
}

//+------------------------------------------------------------------+
void OnTick() {
   if(!g_connected || g_socket == INVALID_HANDLE) return;

   RefreshRates();
   double bid = MarketInfo(Symbol_, MODE_BID);
   double ask = MarketInfo(Symbol_, MODE_ASK);

   // 1. Envoyer tick
   string tickMsg = StringFormat(
      "{\"type\":\"tick\",\"bid\":%.5f,\"ask\":%.5f,\"time\":%d}\n",
      bid, ask, (int)TimeCurrent());
   SendMsg(tickMsg);

   // 2. Envoyer snapshot compte (toutes les 5 secondes)
   static datetime lastAcc = 0;
   if(TimeCurrent() - lastAcc >= 5) {
      lastAcc = TimeCurrent();
      string accMsg = StringFormat(
         "{\"type\":\"account\",\"balance\":%.2f,\"equity\":%.2f,"
         "\"free_margin\":%.2f,\"margin_level\":%.2f,"
         "\"margin_req\":%.2f,\"leverage\":%d,\"currency\":\"%s\"}\n",
         AccountBalance(), AccountEquity(), AccountFreeMargin(),
         AccountFreeMarginCheck(Symbol_, OP_BUY, 0.1),
         MarketInfo(Symbol_, MODE_MARGINREQUIRED),
         AccountLeverage(), AccountCurrency());
      SendMsg(accMsg);
   }

   // 3. Envoyer liste ordres (toutes les 2 secondes)
   static datetime lastOrd = 0;
   if(TimeCurrent() - lastOrd >= 2) {
      lastOrd = TimeCurrent();
      SendOrders();
   }

   // 4. Recevoir commandes du C++20 engine
   ReceiveAndExecute();

   // 5. Ping keepalive
   if(TimeCurrent() - g_lastPing >= 3) {
      SendMsg("{\"type\":\"pong\"}\n");
      g_lastPing = TimeCurrent();
   }
}

//+------------------------------------------------------------------+
void SendOrders() {
   for(int i = 0; i < OrdersTotal(); i++) {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES)) continue;
      if(OrderMagicNumber() != Magic && Magic != 0)  continue;
      if(OrderSymbol() != Symbol_)                    continue;
      double pnl = OrderProfit() + OrderSwap() + OrderCommission();
      string ordMsg = StringFormat(
         "{\"type\":\"order\",\"ticket\":%d,\"type\":%d,"
         "\"lots\":%.2f,\"open\":%.5f,\"sl\":%.5f,\"tp\":%.5f,"
         "\"profit\":%.2f,\"swap\":%.2f,\"commission\":%.2f,"
         "\"comment\":\"%s\"}\n",
         OrderTicket(), OrderType(), OrderLots(), OrderOpenPrice(),
         OrderStopLoss(), OrderTakeProfit(),
         OrderProfit(), OrderSwap(), OrderCommission(),
         OrderComment());
      SendMsg(ordMsg);
   }
}

//+------------------------------------------------------------------+
void ReceiveAndExecute() {
   uchar buf[];
   uint  available = SocketIsReadable(g_socket);
   if(available == 0) return;

   ArrayResize(buf, available);
   uint received = SocketRead(g_socket, buf, available, 100);
   if(received == 0) return;

   g_recvBuf += CharArrayToString(buf, 0, received);

   int nl;
   while((nl = StringFind(g_recvBuf, "\n")) >= 0) {
      string line = StringSubstr(g_recvBuf, 0, nl);
      g_recvBuf   = StringSubstr(g_recvBuf, nl + 1);
      if(StringLen(line) > 0) ExecuteCmd(line);
   }
}

//+------------------------------------------------------------------+
void ExecuteCmd(const string& cmd) {
   // Extraction JSON simplifiée
   string cmdType = JsonGet(cmd, "cmd");

   if(cmdType == "ping") {
      SendMsg("{\"type\":\"pong\"}\n");
      return;
   }

   if(cmdType == "get_orders") {
      SendOrders();
      SendMsg("{\"type\":\"orders_done\"}\n");
      return;
   }

   if(cmdType == "get_account") {
      string accMsg = StringFormat(
         "{\"type\":\"account\",\"balance\":%.2f,\"equity\":%.2f,"
         "\"free_margin\":%.2f,\"margin_level\":%.2f,"
         "\"margin_req\":%.2f,\"leverage\":%d,\"currency\":\"%s\"}\n",
         AccountBalance(), AccountEquity(), AccountFreeMargin(),
         0.0, MarketInfo(Symbol_, MODE_MARGINREQUIRED),
         AccountLeverage(), AccountCurrency());
      SendMsg(accMsg);
      return;
   }

   if(cmdType == "trade") {
      ExecuteTrade(cmd);
      return;
   }
}

//+------------------------------------------------------------------+
void ExecuteTrade(const string& cmd) {
   int    op      = (int)StringToDouble(JsonGet(cmd, "op"));
   int    type    = (int)StringToDouble(JsonGet(cmd, "type"));
   int    ticket  = (int)StringToDouble(JsonGet(cmd, "ticket"));
   int    ticket2 = (int)StringToDouble(JsonGet(cmd, "t2"));
   double lots    = StringToDouble(JsonGet(cmd, "lots"));
   double price   = StringToDouble(JsonGet(cmd, "price"));
   double sl      = StringToDouble(JsonGet(cmd, "sl"));
   double tp      = StringToDouble(JsonGet(cmd, "tp"));
   int    slip    = (int)StringToDouble(JsonGet(cmd, "slip"));
   int    magic   = (int)StringToDouble(JsonGet(cmd, "magic"));
   string sym     = JsonGet(cmd, "sym");
   string cmt     = JsonGet(cmd, "cmt");
   if(sym == "") sym = Symbol_;

   string resp = "";

   RefreshRates();

   // op: 0=MarketOrder, 1=DeletePending, 2=CloseOrder, 3=CloseBy, 4=ModifyOrder
   if(op == 0) {
      // MarketOrder (type inclut BuyStop/SellStop etc.)
      int t = OrderSend(sym, type, lots, price, slip, sl, tp, cmt, magic, 0,
                        (type==OP_BUY||type==OP_BUYSTOP)?clrBlue:clrRed);
      if(t > 0) resp = StringFormat("{\"ok\":true,\"ticket\":%d,\"price\":%.5f}\n", t, price);
      else       resp = StringFormat("{\"ok\":false,\"ticket\":-1,\"error\":%d}\n", GetLastError());
   }
   else if(op == 1) {
      // DeletePending
      bool ok = OrderDelete(ticket);
      if(ok) resp = StringFormat("{\"ok\":true,\"ticket\":%d}\n", ticket);
      else   resp = StringFormat("{\"ok\":false,\"ticket\":%d,\"error\":%d}\n", ticket, GetLastError());
   }
   else if(op == 2) {
      // CloseOrder
      if(OrderSelect(ticket, SELECT_BY_TICKET)) {
         bool ok = OrderClose(ticket, lots, price, slip,
                              OrderType()==OP_BUY?clrBlue:clrRed);
         if(ok) resp = StringFormat("{\"ok\":true,\"ticket\":%d,\"price\":%.5f}\n", ticket, price);
         else   resp = StringFormat("{\"ok\":false,\"ticket\":%d,\"error\":%d}\n", ticket, GetLastError());
      }
   }
   else if(op == 3) {
      // CloseBy
      bool ok = OrderCloseBy(ticket, ticket2, clrYellow);
      if(ok) resp = StringFormat("{\"ok\":true,\"ticket\":%d}\n", ticket);
      else   resp = StringFormat("{\"ok\":false,\"ticket\":%d,\"error\":%d}\n", ticket, GetLastError());
   }
   else if(op == 4) {
      // ModifyOrder
      if(OrderSelect(ticket, SELECT_BY_TICKET)) {
         bool ok = OrderModify(ticket, price, sl, tp, 0, clrOrange);
         if(ok) resp = StringFormat("{\"ok\":true,\"ticket\":%d,\"price\":%.5f}\n", ticket, price);
         else   resp = StringFormat("{\"ok\":false,\"ticket\":%d,\"error\":%d}\n", ticket, GetLastError());
      }
   }

   if(resp != "") SendMsg(resp);
}

//+------------------------------------------------------------------+
bool SendMsg(const string& msg) {
   if(!g_connected || g_socket == INVALID_HANDLE) return false;
   uchar buf[];
   StringToCharArray(msg, buf, 0, StringLen(msg));
   uint sent = SocketSend(g_socket, buf, ArraySize(buf));
   return (sent == (uint)ArraySize(buf));
}

//+------------------------------------------------------------------+
// Extraction valeur JSON simple (clé scalaire)
string JsonGet(const string& json, const string& key) {
   string search = "\"" + key + "\"";
   int pos = StringFind(json, search);
   if(pos < 0) return "";
   pos = StringFind(json, ":", pos + StringLen(search));
   if(pos < 0) return "";
   pos++;
   // Sauter espaces
   while(pos < StringLen(json) && StringGetCharacter(json, pos) == ' ') pos++;
   if(pos >= StringLen(json)) return "";

   if(StringGetCharacter(json, pos) == '"') {
      pos++;
      int end = StringFind(json, "\"", pos);
      if(end < 0) return "";
      return StringSubstr(json, pos, end - pos);
   }
   int e1 = StringFind(json, ",", pos);
   int e2 = StringFind(json, "}", pos);
   int end = (e1 < 0) ? e2 : (e2 < 0 ? e1 : MathMin(e1, e2));
   if(end < 0) return StringSubstr(json, pos);
   return StringSubstr(json, pos, end - pos);
}
//+------------------------------------------------------------------+
