//+------------------------------------------------------------------+
//| MuukBridge.mq5 -- connects MetaTrader 5 to the Muuk app.         |
//|                                                                  |
//| Attach it to the gold chart (XAUUSD / XAUUSDc / ...) of the MT5  |
//| account the bot should trade. It connects to the Muuk app on     |
//| this PC (127.0.0.1:5200), sends prices, the account and fills,   |
//| and places / cancels / closes the orders the bot asks for --     |
//| only on this chart's symbol, and only with its own magic number, |
//| so manual trades and other EAs are never touched.                |
//|                                                                  |
//| Needs: Tools > Options > Expert Advisors > "Allow algorithmic    |
//| trading", and 127.0.0.1 in "Allow WebRequest for listed URL"     |
//| (MT5 uses that list for sockets too). A HEDGING account.         |
//+------------------------------------------------------------------+
#property copyright "Muuk"
#property version   "1.00"
#property description "Bridge between MetaTrader 5 and the Muuk range bot (see docs/mt5.md)."

input string BridgeHost = "127.0.0.1";  // the Muuk app (always this PC)
input int    BridgePort = 5200;         // connection.mt5.eaPort in the Muuk config
input long   MagicNumber = 770077;      // marks the bot's orders and positions
input int    TimerMs = 25;              // how often prices and commands are checked
input int    SlippagePoints = 50;       // market orders: accepted deviation

int    g_sock = INVALID_HANDLE;
string g_rx = "";
double g_lastBid = 0.0;
double g_lastAsk = 0.0;
ulong  g_lastAccountMs = 0;
ulong  g_lastConnectTryMs = 0;
bool   g_helloSent = false;

// Order ticket -> the bot's ClOrdID (also kept in the order comment).
ulong  g_mapTicket[];
string g_mapId[];

//+------------------------------------------------------------------+
int OnInit()
{
   EventSetMillisecondTimer(MathMax(10, TimerMs));
   Connect();
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason)
{
   EventKillTimer();
   if(g_sock != INVALID_HANDLE) SocketClose(g_sock);
   g_sock = INVALID_HANDLE;
}

void OnTick()  { SendTickIfChanged(); }

void OnTimer()
{
   if(g_sock == INVALID_HANDLE || !SocketIsConnected(g_sock))
   {
      if(g_sock != INVALID_HANDLE) { SocketClose(g_sock); g_sock = INVALID_HANDLE; }
      if(GetTickCount64() - g_lastConnectTryMs > 2000) Connect();
      return;
   }
   ReadCommands();
   SendTickIfChanged();
   if(GetTickCount64() - g_lastAccountMs >= 1000) SendAccount();
}

//+------------------------------------------------------------------+
//| Connection                                                       |
//+------------------------------------------------------------------+
void Connect()
{
   g_lastConnectTryMs = GetTickCount64();
   g_sock = SocketCreate();
   if(g_sock == INVALID_HANDLE) { Print("MuukBridge: SocketCreate failed ", GetLastError()); return; }
   if(!SocketConnect(g_sock, BridgeHost, BridgePort, 1000))
   {
      const int err = GetLastError();
      if(err == 4014)
         Print("MuukBridge: sockets are not allowed -- add 127.0.0.1 to Tools > Options > Expert Advisors > Allow WebRequest");
      SocketClose(g_sock);
      g_sock = INVALID_HANDLE;
      return;
   }
   g_rx = "";
   g_helloSent = false;
   g_lastBid = 0.0;
   Print("MuukBridge: connected to Muuk on ", BridgeHost, ":", BridgePort);
   SendHello();
   SendAccount();
   SendTickIfChanged();
}

bool SendLine(string line)
{
   if(g_sock == INVALID_HANDLE) return false;
   uchar data[];
   const int n = StringToCharArray(line + "\n", data, 0, WHOLE_ARRAY, CP_UTF8) - 1;  // without the trailing 0
   if(n <= 0) return false;
   if(SocketSend(g_sock, data, n) != n)
   {
      SocketClose(g_sock);
      g_sock = INVALID_HANDLE;
      return false;
   }
   return true;
}

void ReadCommands()
{
   uint len = SocketIsReadable(g_sock);
   while(len > 0)
   {
      uchar buf[];
      const int got = SocketRead(g_sock, buf, len, 50);
      if(got <= 0) break;
      g_rx += CharArrayToString(buf, 0, got, CP_UTF8);
      len = SocketIsReadable(g_sock);
   }
   int at = StringFind(g_rx, "\n");
   while(at >= 0)
   {
      const string line = StringSubstr(g_rx, 0, at);
      g_rx = StringSubstr(g_rx, at + 1);
      if(StringLen(line) > 0) HandleCommand(line);
      at = StringFind(g_rx, "\n");
   }
}

//+------------------------------------------------------------------+
//| Tiny JSON helpers (the gateway sends flat objects)               |
//+------------------------------------------------------------------+
int JsonValueStart(const string json, const string key)
{
   const string pattern = "\"" + key + "\":";
   const int at = StringFind(json, pattern);
   if(at < 0) return -1;
   int i = at + StringLen(pattern);
   while(i < StringLen(json) && StringGetCharacter(json, i) == ' ') i++;
   return i;
}

string JsonStr(const string json, const string key)
{
   const int i = JsonValueStart(json, key);
   if(i < 0) return "";
   if(StringGetCharacter(json, i) != '"')  // a number written without quotes
   {
      int j = i;
      while(j < StringLen(json) && StringGetCharacter(json, j) != ',' && StringGetCharacter(json, j) != '}') j++;
      return StringSubstr(json, i, j - i);
   }
   string out = "";
   for(int j = i + 1; j < StringLen(json); j++)
   {
      const ushort c = StringGetCharacter(json, j);
      if(c == '\\' && j + 1 < StringLen(json)) { j++; out += ShortToString(StringGetCharacter(json, j)); continue; }
      if(c == '"') break;
      out += ShortToString(c);
   }
   return out;
}

double JsonNum(const string json, const string key) { return StringToDouble(JsonStr(json, key)); }
bool   JsonBool(const string json, const string key) { return JsonStr(json, key) == "true"; }

string Esc(const string s)
{
   string out = s;
   StringReplace(out, "\\", "\\\\");
   StringReplace(out, "\"", "\\\"");
   return out;
}

string Num(const double v, const int digits) { return DoubleToString(v, digits); }

//+------------------------------------------------------------------+
//| Messages to Muuk                                                 |
//+------------------------------------------------------------------+
double MarginPerLot()
{
   // Margin for 1 lot at the current price, in the account's money, as MT5
   // itself works it out (leverage, per-symbol margin rates, cent accounts).
   double margin1 = 0.0;
   const double ask = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   if(ask > 0.0 && OrderCalcMargin(ORDER_TYPE_BUY, _Symbol, 1.0, ask, margin1)) return margin1;
   return 0.0;
}

double HedgedMarginPercent()
{
   if((bool)SymbolInfoInteger(_Symbol, SYMBOL_MARGIN_HEDGED_USE_LEG)) return 100.0;
   const double contract = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_CONTRACT_SIZE);
   const double hedged = SymbolInfoDouble(_Symbol, SYMBOL_MARGIN_HEDGED);
   if(contract <= 0.0) return -1.0;
   return MathMin(100.0, MathMax(0.0, hedged / contract * 100.0));
}

void SendHello()
{
   const bool hedging = AccountInfoInteger(ACCOUNT_MARGIN_MODE) == ACCOUNT_MARGIN_MODE_RETAIL_HEDGING;
   string s = "{\"t\":\"hello\"";
   s += ",\"login\":\"" + IntegerToString(AccountInfoInteger(ACCOUNT_LOGIN)) + "\"";
   s += ",\"server\":\"" + Esc(AccountInfoString(ACCOUNT_SERVER)) + "\"";
   s += ",\"company\":\"" + Esc(AccountInfoString(ACCOUNT_COMPANY)) + "\"";
   s += ",\"currency\":\"" + Esc(AccountInfoString(ACCOUNT_CURRENCY)) + "\"";
   s += ",\"leverage\":" + IntegerToString(AccountInfoInteger(ACCOUNT_LEVERAGE));
   s += ",\"tickSize\":" + Num(SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_SIZE), 8);
   s += ",\"tickValue\":" + Num(SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_VALUE), 8);
   s += ",\"marginPerLot\":" + Num(MarginPerLot(), 4);
   s += ",\"hedging\":" + (hedging ? "true" : "false");
   s += ",\"symbol\":\"" + Esc(_Symbol) + "\"";
   s += ",\"digits\":" + IntegerToString(_Digits);
   s += ",\"contract\":" + Num(SymbolInfoDouble(_Symbol, SYMBOL_TRADE_CONTRACT_SIZE), 4);
   s += ",\"volMin\":" + Num(SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN), 4);
   s += ",\"volStep\":" + Num(SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP), 4);
   s += ",\"volMax\":" + Num(SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX), 4);
   s += ",\"hedgedMargin\":" + Num(HedgedMarginPercent(), 2);
   s += ",\"magic\":" + IntegerToString(MagicNumber);
   s += ",\"limitOrders\":" + IntegerToString(AccountInfoInteger(ACCOUNT_LIMIT_ORDERS));
   const long mode = AccountInfoInteger(ACCOUNT_TRADE_MODE);
   s += ",\"tradeMode\":\"" + (mode == ACCOUNT_TRADE_MODE_DEMO ? "demo" : (mode == ACCOUNT_TRADE_MODE_CONTEST ? "contest" : "real")) + "\"";
   s += "}";
   if(SendLine(s)) g_helloSent = true;
   if(!hedging) Print("MuukBridge: this is a NETTING account -- Muuk needs a hedging account.");
}

void SendTickIfChanged()
{
   if(g_sock == INVALID_HANDLE) return;
   MqlTick t;
   if(!SymbolInfoTick(_Symbol, t) || t.bid <= 0.0 || t.ask <= 0.0) return;
   if(t.bid == g_lastBid && t.ask == g_lastAsk) return;
   g_lastBid = t.bid;
   g_lastAsk = t.ask;
   SendLine("{\"t\":\"tick\",\"bid\":" + Num(t.bid, _Digits) + ",\"ask\":" + Num(t.ask, _Digits) + "}");
}

bool IsOurPosition()
{
   return PositionGetString(POSITION_SYMBOL) == _Symbol && PositionGetInteger(POSITION_MAGIC) == MagicNumber;
}

void SendAccount()
{
   g_lastAccountMs = GetTickCount64();
   string costs = "";
   for(int i = PositionsTotal() - 1; i >= 0; i--)
   {
      const ulong ticket = PositionGetTicket(i);
      if(ticket == 0 || !IsOurPosition()) continue;
      if(costs != "") costs += ",";
      costs += "{\"pos\":\"" + IntegerToString(PositionGetInteger(POSITION_IDENTIFIER)) + "\",\"swap\":" +
               Num(PositionGetDouble(POSITION_SWAP), 2) + ",\"comm\":0}";
   }
   string s = "{\"t\":\"account\"";
   s += ",\"balance\":" + Num(AccountInfoDouble(ACCOUNT_BALANCE), 2);
   s += ",\"equity\":" + Num(AccountInfoDouble(ACCOUNT_EQUITY), 2);
   s += ",\"margin\":" + Num(AccountInfoDouble(ACCOUNT_MARGIN), 2);
   s += ",\"free\":" + Num(AccountInfoDouble(ACCOUNT_MARGIN_FREE), 2);
   s += ",\"marginPerLot\":" + Num(MarginPerLot(), 4);
   s += ",\"tickValue\":" + Num(SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_VALUE), 8);
   s += ",\"costs\":[" + costs + "]}";
   SendLine(s);
}

void SendReject(const string id, const string text)
{
   SendLine("{\"t\":\"reject\",\"id\":\"" + Esc(id) + "\",\"text\":\"" + Esc(text) + "\"}");
}

//+------------------------------------------------------------------+
//| Id map                                                           |
//+------------------------------------------------------------------+
void MapPut(const ulong ticket, const string id)
{
   const int n = ArraySize(g_mapTicket);
   if(n > 500)  // keep it small: drop the oldest half
   {
      ArrayRemove(g_mapTicket, 0, 250);
      ArrayRemove(g_mapId, 0, 250);
   }
   const int m = ArraySize(g_mapTicket);
   ArrayResize(g_mapTicket, m + 1);
   ArrayResize(g_mapId, m + 1);
   g_mapTicket[m] = ticket;
   g_mapId[m] = id;
}

string IdForOrder(const ulong orderTicket)
{
   for(int i = ArraySize(g_mapTicket) - 1; i >= 0; i--)
      if(g_mapTicket[i] == orderTicket) return g_mapId[i];
   // Placed before a restart of this EA: the id is the order's comment.
   if(HistoryOrderSelect(orderTicket)) return HistoryOrderGetString(orderTicket, ORDER_COMMENT);
   if(OrderSelect(orderTicket)) return OrderGetString(ORDER_COMMENT);
   return "";
}

ulong TicketForId(const string id)
{
   for(int i = OrdersTotal() - 1; i >= 0; i--)
   {
      const ulong t = OrderGetTicket(i);
      if(t == 0) continue;
      if(OrderGetString(ORDER_SYMBOL) != _Symbol || OrderGetInteger(ORDER_MAGIC) != MagicNumber) continue;
      if(OrderGetString(ORDER_COMMENT) == id) return t;
      for(int k = ArraySize(g_mapTicket) - 1; k >= 0; k--)
         if(g_mapTicket[k] == t && g_mapId[k] == id) return t;
   }
   return 0;
}

//+------------------------------------------------------------------+
//| Commands from Muuk                                               |
//+------------------------------------------------------------------+
double NormalizeVolume(double v)
{
   const double step = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP);
   const double vmin = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
   const double vmax = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX);
   if(step > 0.0) v = MathRound(v / step) * step;
   return NormalizeDouble(MathMin(vmax, MathMax(vmin, v)), 8);
}

ENUM_ORDER_TYPE_FILLING MarketFilling()
{
   const long mode = SymbolInfoInteger(_Symbol, SYMBOL_FILLING_MODE);
   if((mode & SYMBOL_FILLING_FOK) != 0) return ORDER_FILLING_FOK;
   if((mode & SYMBOL_FILLING_IOC) != 0) return ORDER_FILLING_IOC;
   return ORDER_FILLING_RETURN;
}

void HandleCommand(const string line)
{
   const string c = JsonStr(line, "c");
   if(c == "stop")           PlaceStop(line);
   else if(c == "market")    PlaceMarket(line);
   else if(c == "cancel")    CancelOrder(line);
   else if(c == "positions") ListPositions(JsonStr(line, "req"));
   else if(c == "orders")    ListOrders(JsonStr(line, "req"));
}

void PlaceStop(const string line)
{
   const string id = JsonStr(line, "id");
   const bool buy = JsonBool(line, "buy");
   MqlTradeRequest req;
   MqlTradeResult res;
   ZeroMemory(req);
   ZeroMemory(res);
   req.action = TRADE_ACTION_PENDING;
   req.symbol = _Symbol;
   req.volume = NormalizeVolume(JsonNum(line, "volume"));
   req.price = NormalizeDouble(JsonNum(line, "price"), _Digits);
   req.type = buy ? ORDER_TYPE_BUY_STOP : ORDER_TYPE_SELL_STOP;
   req.type_filling = ORDER_FILLING_RETURN;
   req.type_time = ORDER_TIME_GTC;
   req.magic = MagicNumber;
   req.comment = id;
   if(!OrderSend(req, res) || (res.retcode != TRADE_RETCODE_PLACED && res.retcode != TRADE_RETCODE_DONE))
   {
      SendReject(id, "MT5 " + IntegerToString(res.retcode) + ": " + res.comment);
      return;
   }
   MapPut(res.order, id);
   SendLine("{\"t\":\"ack\",\"id\":\"" + Esc(id) + "\",\"ticket\":\"" + IntegerToString(res.order) + "\"}");
}

void PlaceMarket(const string line)
{
   const string id = JsonStr(line, "id");
   const bool buy = JsonBool(line, "buy");
   const string pos = JsonStr(line, "pos");
   MqlTradeRequest req;
   MqlTradeResult res;
   ZeroMemory(req);
   ZeroMemory(res);
   req.action = TRADE_ACTION_DEAL;
   req.symbol = _Symbol;
   req.volume = NormalizeVolume(JsonNum(line, "volume"));
   req.type = buy ? ORDER_TYPE_BUY : ORDER_TYPE_SELL;
   req.price = buy ? SymbolInfoDouble(_Symbol, SYMBOL_ASK) : SymbolInfoDouble(_Symbol, SYMBOL_BID);
   req.deviation = SlippagePoints;
   req.type_filling = MarketFilling();
   req.magic = MagicNumber;
   req.comment = id;
   if(pos != "")
   {
      // A close: aimed at that position (hedging account), never a new one.
      const ulong ticket = (ulong)StringToInteger(pos);
      if(!PositionSelectByTicket(ticket))
      {
         SendReject(id, "INCORRECT_POSITION_ID: Unknown position. PositionId=" + pos);
         return;
      }
      req.position = ticket;
      req.volume = PositionGetDouble(POSITION_VOLUME) < req.volume ? PositionGetDouble(POSITION_VOLUME) : req.volume;
   }
   bool ok = OrderSend(req, res);
   if(!ok && res.retcode == TRADE_RETCODE_INVALID_FILL)
   {
      req.type_filling = req.type_filling == ORDER_FILLING_FOK ? ORDER_FILLING_IOC : ORDER_FILLING_RETURN;
      ok = OrderSend(req, res);
   }
   if(!ok || (res.retcode != TRADE_RETCODE_DONE && res.retcode != TRADE_RETCODE_PLACED &&
              res.retcode != TRADE_RETCODE_DONE_PARTIAL))
   {
      SendReject(id, "MT5 " + IntegerToString(res.retcode) + ": " + res.comment);
      return;
   }
   MapPut(res.order, id);  // the fill itself arrives through OnTradeTransaction
}

void CancelOrder(const string line)
{
   const string id = JsonStr(line, "id");
   const string orig = JsonStr(line, "orig");
   const ulong ticket = TicketForId(orig);
   if(ticket == 0)
   {
      SendLine("{\"t\":\"cancel_reject\",\"id\":\"" + Esc(id) + "\",\"orig\":\"" + Esc(orig) +
               "\",\"text\":\"order not found (already filled or cancelled)\"}");
      return;
   }
   MqlTradeRequest req;
   MqlTradeResult res;
   ZeroMemory(req);
   ZeroMemory(res);
   req.action = TRADE_ACTION_REMOVE;
   req.order = ticket;
   if(!OrderSend(req, res) || res.retcode != TRADE_RETCODE_DONE)
   {
      SendLine("{\"t\":\"cancel_reject\",\"id\":\"" + Esc(id) + "\",\"orig\":\"" + Esc(orig) + "\",\"text\":\"MT5 " +
               IntegerToString(res.retcode) + ": " + Esc(res.comment) + "\"}");
      return;
   }
   SendLine("{\"t\":\"cancelled\",\"id\":\"" + Esc(orig) + "\"}");
}

void ListPositions(const string reqId)
{
   string list = "";
   for(int i = PositionsTotal() - 1; i >= 0; i--)
   {
      const ulong ticket = PositionGetTicket(i);
      if(ticket == 0 || !IsOurPosition()) continue;
      if(list != "") list += ",";
      list += "{\"pos\":\"" + IntegerToString(PositionGetInteger(POSITION_IDENTIFIER)) + "\",\"buy\":" +
              (PositionGetInteger(POSITION_TYPE) == POSITION_TYPE_BUY ? "true" : "false") + ",\"volume\":" +
              Num(PositionGetDouble(POSITION_VOLUME), 4) + ",\"price\":" + Num(PositionGetDouble(POSITION_PRICE_OPEN), _Digits) + "}";
   }
   SendLine("{\"t\":\"positions\",\"req\":\"" + Esc(reqId) + "\",\"list\":[" + list + "]}");
}

void ListOrders(const string reqId)
{
   string list = "";
   for(int i = OrdersTotal() - 1; i >= 0; i--)
   {
      const ulong t = OrderGetTicket(i);
      if(t == 0) continue;
      if(OrderGetString(ORDER_SYMBOL) != _Symbol || OrderGetInteger(ORDER_MAGIC) != MagicNumber) continue;
      const long type = OrderGetInteger(ORDER_TYPE);
      if(type != ORDER_TYPE_BUY_STOP && type != ORDER_TYPE_SELL_STOP) continue;
      string id = IdForOrder(t);
      if(id == "") id = OrderGetString(ORDER_COMMENT);
      if(list != "") list += ",";
      list += "{\"id\":\"" + Esc(id) + "\",\"ticket\":\"" + IntegerToString(t) + "\",\"buy\":" +
              (type == ORDER_TYPE_BUY_STOP ? "true" : "false") + ",\"volume\":" +
              Num(OrderGetDouble(ORDER_VOLUME_CURRENT), 4) + ",\"price\":" + Num(OrderGetDouble(ORDER_PRICE_OPEN), _Digits) + "}";
   }
   SendLine("{\"t\":\"orders\",\"req\":\"" + Esc(reqId) + "\",\"list\":[" + list + "]}");
}

//+------------------------------------------------------------------+
//| Fills: every deal of this EA's magic on this symbol              |
//+------------------------------------------------------------------+
void OnTradeTransaction(const MqlTradeTransaction& trans, const MqlTradeRequest& request, const MqlTradeResult& result)
{
   if(trans.type != TRADE_TRANSACTION_DEAL_ADD || trans.deal == 0) return;
   if(!HistoryDealSelect(trans.deal)) return;
   if(HistoryDealGetString(trans.deal, DEAL_SYMBOL) != _Symbol) return;
   if(HistoryDealGetInteger(trans.deal, DEAL_MAGIC) != MagicNumber) return;
   const long dealType = HistoryDealGetInteger(trans.deal, DEAL_TYPE);
   if(dealType != DEAL_TYPE_BUY && dealType != DEAL_TYPE_SELL) return;
   const ulong order = (ulong)HistoryDealGetInteger(trans.deal, DEAL_ORDER);
   const string id = IdForOrder(order);
   if(id == "") return;  // not one of the bot's orders
   SendLine("{\"t\":\"fill\",\"id\":\"" + Esc(id) + "\",\"price\":" + Num(HistoryDealGetDouble(trans.deal, DEAL_PRICE), _Digits) +
            ",\"volume\":" + Num(HistoryDealGetDouble(trans.deal, DEAL_VOLUME), 4) + ",\"pos\":\"" +
            IntegerToString(HistoryDealGetInteger(trans.deal, DEAL_POSITION_ID)) + "\",\"buy\":" +
            (dealType == DEAL_TYPE_BUY ? "true" : "false") + "}");
   SendAccount();
}
//+------------------------------------------------------------------+
