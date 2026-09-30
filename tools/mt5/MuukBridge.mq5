//+------------------------------------------------------------------+
//| MuukBridge.mq5 -- the Muuk app's hands inside MetaTrader 5.      |
//|                                                                  |
//| You don't attach this by hand: when a Muuk config says           |
//| "platform": "mt5", the app installs it, compiles it and starts   |
//| MT5 with it on a chart (see docs/mt5.md). It connects to the app |
//| over a local named pipe (no MT5 permission needed), sends prices,|
//| the account and every fill, and places / cancels / closes the    |
//| orders the bot asks for -- only on the gold symbol and only with |
//| its own magic number, so manual trades and other EAs are never   |
//| touched.                                                         |
//+------------------------------------------------------------------+
#property copyright "Muuk"
#property version   "2.00"
#property description "Bridge between MetaTrader 5 and the Muuk range bot (see docs/mt5.md)."

input string PipeName = "MuukBridge_5200";  // connection.mt5.eaPort in the Muuk config: MuukBridge_<eaPort>
input string TradeSymbol = "";              // empty = the chart's symbol if it's gold, else the account's gold symbol
input long   MagicNumber = 770077;          // marks the bot's orders and positions
input int    TimerMs = 25;                  // how often the app is polled
input int    SlippagePoints = 50;           // market orders: accepted deviation

int    g_pipe = INVALID_HANDLE;
string g_sym = "";
int    g_digits = 2;
string g_out[];            // lines for the app's next poll
string g_tick = "";        // the latest price line (only the newest is sent)
double g_lastBid = 0.0;
double g_lastAsk = 0.0;
ulong  g_lastAccountMs = 0;
ulong  g_lastConnectTryMs = 0;

// Order ticket -> the bot's ClOrdID (also kept in the order comment).
ulong  g_mapTicket[];
string g_mapId[];

// A one-line status in MQL5\Files\MuukBridge.status (MT5 writes its own logs late).
void WriteStatus(const string text)
{
   const int h = FileOpen("MuukBridge.status", FILE_WRITE | FILE_TXT | FILE_ANSI);
   if(h == INVALID_HANDLE) return;
   FileWriteString(h, TimeToString(TimeLocal(), TIME_DATE | TIME_SECONDS) + " " + text + "\r\n");
   FileClose(h);
}

//+------------------------------------------------------------------+
bool IsGold(const string name)
{
   string u = name;
   StringToUpper(u);
   return StringFind(u, "XAUUSD") == 0 || StringFind(u, "GOLD") == 0;
}

bool Exists(const string name)
{
   bool custom = false;
   return name != "" && SymbolExist(name, custom);
}

string FindGoldSymbol()
{
   if(Exists(TradeSymbol) && SymbolSelect(TradeSymbol, true)) return TradeSymbol;
   if(IsGold(_Symbol) && Exists(_Symbol)) return _Symbol;
   string best = "";
   for(int i = 0; i < SymbolsTotal(false); i++)
   {
      const string name = SymbolName(i, false);
      if(!IsGold(name)) continue;
      if(SymbolInfoInteger(name, SYMBOL_TRADE_MODE) == SYMBOL_TRADE_MODE_DISABLED) continue;
      if(best == "" || StringLen(name) < StringLen(best)) best = name;  // XAUUSD before XAUUSD.x
   }
   if(best != "") { SymbolSelect(best, true); return best; }
   return "";  // no gold on this account
}

int OnInit()
{
   g_sym = FindGoldSymbol();
   if(g_sym == "")
   {
      Print("MuukBridge: no gold symbol found on this account (chart ", _Symbol, ")");
      WriteStatus("no gold symbol on this account (chart " + _Symbol + ")");
   }
   else
   {
      g_digits = (int)SymbolInfoInteger(g_sym, SYMBOL_DIGITS);
      Print("MuukBridge: trading ", g_sym, ", waiting for the Muuk app on pipe ", PipeName);
      WriteStatus("trading " + g_sym + ", waiting for pipe " + PipeName);
   }
   EventSetMillisecondTimer(MathMax(10, TimerMs));
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason)
{
   EventKillTimer();
   Drop();
}

void OnTick() {}

void OnTimer()
{
   if(g_sym == "")  // symbols may arrive a moment after login
   {
      g_sym = FindGoldSymbol();
      if(g_sym == "") return;
      g_digits = (int)SymbolInfoInteger(g_sym, SYMBOL_DIGITS);
      WriteStatus("trading " + g_sym + ", waiting for pipe " + PipeName);
   }
   if(g_pipe == INVALID_HANDLE)
   {
      if(GetTickCount64() - g_lastConnectTryMs > 1000) Connect();
      if(g_pipe == INVALID_HANDLE) return;
   }
   QueueTick();
   if(GetTickCount64() - g_lastAccountMs >= 1000) QueueAccount();
   Exchange();
}

//+------------------------------------------------------------------+
//| Pipe: one frame out, one frame back (4-byte length + JSON lines)  |
//+------------------------------------------------------------------+
int g_lastOpenError = -1;

void Connect()
{
   g_lastConnectTryMs = GetTickCount64();
   ResetLastError();
   g_pipe = FileOpen("\\\\.\\pipe\\" + PipeName, FILE_READ | FILE_WRITE | FILE_BIN);
   if(g_pipe == INVALID_HANDLE)
   {
      // The app isn't running (yet) -- tried again every second.
      const int err = GetLastError();
      if(err != g_lastOpenError)
      {
         g_lastOpenError = err;
         Print("MuukBridge: waiting for the Muuk app (pipe ", PipeName, ", error ", err, ")");
         WriteStatus("waiting for pipe " + PipeName + ", error " + IntegerToString(err) + ", symbol " + g_sym);
      }
      return;
   }
   g_lastOpenError = -1;
   WriteStatus("connected to pipe " + PipeName + ", symbol " + g_sym);
   ArrayResize(g_out, 0);
   g_lastBid = 0.0;
   g_lastAsk = 0.0;
   Print("MuukBridge: connected to the Muuk app");
   QueueHello();
   QueueAccount();
}

void Drop()
{
   if(g_pipe != INVALID_HANDLE) FileClose(g_pipe);
   g_pipe = INVALID_HANDLE;
}

void Send(const string line)
{
   if(g_pipe == INVALID_HANDLE) return;
   const int n = ArraySize(g_out);
   ArrayResize(g_out, n + 1);
   g_out[n] = line;
}

void Exchange()
{
   string payload = "";
   for(int i = 0; i < ArraySize(g_out); i++) payload += g_out[i] + "\n";
   if(g_tick != "") { payload += g_tick + "\n"; g_tick = ""; }
   ArrayResize(g_out, 0);
   uchar data[];
   int n = 0;
   if(payload != "") n = StringToCharArray(payload, data, 0, WHOLE_ARRAY, CP_UTF8) - 1;  // without the trailing 0
   if(FileWriteInteger(g_pipe, n, INT_VALUE) != 4) { Drop(); return; }
   if(n > 0 && FileWriteArray(g_pipe, data, 0, n) != (uint)n) { Drop(); return; }
   FileFlush(g_pipe);
   ResetLastError();
   const int len = FileReadInteger(g_pipe, INT_VALUE);
   if(GetLastError() != 0 || len < 0) { Drop(); return; }
   if(len == 0) return;
   uchar buf[];
   if(FileReadArray(g_pipe, buf, 0, len) != (uint)len) { Drop(); return; }
   string rx = CharArrayToString(buf, 0, len, CP_UTF8);
   int at = StringFind(rx, "\n");
   while(at >= 0)
   {
      const string line = StringSubstr(rx, 0, at);
      rx = StringSubstr(rx, at + 1);
      if(StringLen(line) > 0) HandleCommand(line);
      at = StringFind(rx, "\n");
   }
   if(StringLen(rx) > 0) HandleCommand(rx);
}

//+------------------------------------------------------------------+
//| Tiny JSON helpers (the app sends flat objects)                    |
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
   if(StringGetCharacter(json, i) != '"')  // a number or true/false
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
//| Messages to the app                                               |
//+------------------------------------------------------------------+
double MarginPerLot()
{
   // Margin for 1 lot at the current price, in the account's money, as MT5
   // itself works it out (leverage, per-symbol margin rates, cent accounts).
   double margin1 = 0.0;
   const double ask = SymbolInfoDouble(g_sym, SYMBOL_ASK);
   if(ask > 0.0 && OrderCalcMargin(ORDER_TYPE_BUY, g_sym, 1.0, ask, margin1)) return margin1;
   return 0.0;
}

double HedgedMarginPercent()
{
   if((bool)SymbolInfoInteger(g_sym, SYMBOL_MARGIN_HEDGED_USE_LEG)) return 100.0;
   const double contract = SymbolInfoDouble(g_sym, SYMBOL_TRADE_CONTRACT_SIZE);
   const double hedged = SymbolInfoDouble(g_sym, SYMBOL_MARGIN_HEDGED);
   if(contract <= 0.0) return -1.0;
   return MathMin(100.0, MathMax(0.0, hedged / contract * 100.0));
}

// What this account really pays per 1 lot, from its own gold deals of the
// last 90 days: commission + fees on opening (in) and on closing (out), as
// a positive cost in the account's money. -1 = no deals to learn from.
void CommissionFromHistory(double &inPerLot, double &outPerLot)
{
   inPerLot = -1.0;
   outPerLot = -1.0;
   if(!HistorySelect(TimeCurrent() - 90 * 86400, TimeCurrent() + 86400)) return;
   double inCost = 0.0, inVol = 0.0, outCost = 0.0, outVol = 0.0;
   const int n = HistoryDealsTotal();
   for(int i = 0; i < n; i++)
   {
      const ulong d = HistoryDealGetTicket(i);
      if(d == 0 || HistoryDealGetString(d, DEAL_SYMBOL) != g_sym) continue;
      const long type = HistoryDealGetInteger(d, DEAL_TYPE);
      if(type != DEAL_TYPE_BUY && type != DEAL_TYPE_SELL) continue;
      const double vol = HistoryDealGetDouble(d, DEAL_VOLUME);
      const double cost = -(HistoryDealGetDouble(d, DEAL_COMMISSION) + HistoryDealGetDouble(d, DEAL_FEE));
      const long entry = HistoryDealGetInteger(d, DEAL_ENTRY);
      if(entry == DEAL_ENTRY_IN) { inCost += cost; inVol += vol; }
      else if(entry == DEAL_ENTRY_OUT || entry == DEAL_ENTRY_OUT_BY) { outCost += cost; outVol += vol; }
   }
   if(inVol > 0.0) inPerLot = MathMax(0.0, inCost / inVol);
   if(outVol > 0.0) outPerLot = MathMax(0.0, outCost / outVol);
}

void QueueHello()
{
   double commIn = -1.0, commOut = -1.0;
   CommissionFromHistory(commIn, commOut);
   const bool hedging = AccountInfoInteger(ACCOUNT_MARGIN_MODE) == ACCOUNT_MARGIN_MODE_RETAIL_HEDGING;
   const long mode = AccountInfoInteger(ACCOUNT_TRADE_MODE);
   string s = "{\"t\":\"hello\"";
   s += ",\"login\":\"" + IntegerToString(AccountInfoInteger(ACCOUNT_LOGIN)) + "\"";
   s += ",\"server\":\"" + Esc(AccountInfoString(ACCOUNT_SERVER)) + "\"";
   s += ",\"company\":\"" + Esc(AccountInfoString(ACCOUNT_COMPANY)) + "\"";
   s += ",\"currency\":\"" + Esc(AccountInfoString(ACCOUNT_CURRENCY)) + "\"";
   s += ",\"leverage\":" + IntegerToString(AccountInfoInteger(ACCOUNT_LEVERAGE));
   s += ",\"hedging\":" + (hedging ? "true" : "false");
   s += ",\"tradeMode\":\"" + (mode == ACCOUNT_TRADE_MODE_DEMO ? "demo" : (mode == ACCOUNT_TRADE_MODE_CONTEST ? "contest" : "real")) + "\"";
   s += ",\"symbol\":\"" + Esc(g_sym) + "\"";
   s += ",\"digits\":" + IntegerToString(g_digits);
   s += ",\"contract\":" + Num(SymbolInfoDouble(g_sym, SYMBOL_TRADE_CONTRACT_SIZE), 4);
   s += ",\"volMin\":" + Num(SymbolInfoDouble(g_sym, SYMBOL_VOLUME_MIN), 4);
   s += ",\"volStep\":" + Num(SymbolInfoDouble(g_sym, SYMBOL_VOLUME_STEP), 4);
   s += ",\"volMax\":" + Num(SymbolInfoDouble(g_sym, SYMBOL_VOLUME_MAX), 4);
   s += ",\"hedgedMargin\":" + Num(HedgedMarginPercent(), 2);
   s += ",\"tickSize\":" + Num(SymbolInfoDouble(g_sym, SYMBOL_TRADE_TICK_SIZE), 8);
   s += ",\"tickValue\":" + Num(SymbolInfoDouble(g_sym, SYMBOL_TRADE_TICK_VALUE), 8);
   s += ",\"marginPerLot\":" + Num(MarginPerLot(), 4);
   s += ",\"magic\":" + IntegerToString(MagicNumber);
   s += ",\"limitOrders\":" + IntegerToString(AccountInfoInteger(ACCOUNT_LIMIT_ORDERS));
   s += ",\"commIn\":" + Num(commIn, 4) + ",\"commOut\":" + Num(commOut, 4);
   s += "}";
   Send(s);
   if(!hedging) Print("MuukBridge: this is a NETTING account -- Muuk needs a hedging account.");
}

void QueueTick()
{
   MqlTick t;
   if(!SymbolInfoTick(g_sym, t) || t.bid <= 0.0 || t.ask <= 0.0) return;
   if(t.bid == g_lastBid && t.ask == g_lastAsk) return;
   g_lastBid = t.bid;
   g_lastAsk = t.ask;
   g_tick = "{\"t\":\"tick\",\"bid\":" + Num(t.bid, g_digits) + ",\"ask\":" + Num(t.ask, g_digits) + "}";
}

bool IsOurPosition()
{
   return PositionGetString(POSITION_SYMBOL) == g_sym && PositionGetInteger(POSITION_MAGIC) == MagicNumber;
}

void QueueAccount()
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
   s += ",\"tickValue\":" + Num(SymbolInfoDouble(g_sym, SYMBOL_TRADE_TICK_VALUE), 8);
   s += ",\"costs\":[" + costs + "]}";
   Send(s);
}

void SendReject(const string id, const string text)
{
   Send("{\"t\":\"reject\",\"id\":\"" + Esc(id) + "\",\"text\":\"" + Esc(text) + "\"}");
}

//+------------------------------------------------------------------+
//| Id map                                                            |
//+------------------------------------------------------------------+
void MapPut(const ulong ticket, const string id)
{
   if(ArraySize(g_mapTicket) > 500)  // keep it small: drop the oldest half
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
      if(OrderGetString(ORDER_SYMBOL) != g_sym || OrderGetInteger(ORDER_MAGIC) != MagicNumber) continue;
      if(OrderGetString(ORDER_COMMENT) == id) return t;
      for(int k = ArraySize(g_mapTicket) - 1; k >= 0; k--)
         if(g_mapTicket[k] == t && g_mapId[k] == id) return t;
   }
   return 0;
}

//+------------------------------------------------------------------+
//| Commands from the app                                             |
//+------------------------------------------------------------------+
double NormalizeVolume(double v)
{
   const double step = SymbolInfoDouble(g_sym, SYMBOL_VOLUME_STEP);
   const double vmin = SymbolInfoDouble(g_sym, SYMBOL_VOLUME_MIN);
   const double vmax = SymbolInfoDouble(g_sym, SYMBOL_VOLUME_MAX);
   if(step > 0.0) v = MathRound(v / step) * step;
   return NormalizeDouble(MathMin(vmax, MathMax(vmin, v)), 8);
}

ENUM_ORDER_TYPE_FILLING MarketFilling()
{
   const long mode = SymbolInfoInteger(g_sym, SYMBOL_FILLING_MODE);
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
   req.symbol = g_sym;
   req.volume = NormalizeVolume(JsonNum(line, "volume"));
   req.price = NormalizeDouble(JsonNum(line, "price"), g_digits);
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
   Send("{\"t\":\"ack\",\"id\":\"" + Esc(id) + "\",\"ticket\":\"" + IntegerToString(res.order) + "\"}");
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
   req.symbol = g_sym;
   req.volume = NormalizeVolume(JsonNum(line, "volume"));
   req.type = buy ? ORDER_TYPE_BUY : ORDER_TYPE_SELL;
   req.price = buy ? SymbolInfoDouble(g_sym, SYMBOL_ASK) : SymbolInfoDouble(g_sym, SYMBOL_BID);
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
      const double held = PositionGetDouble(POSITION_VOLUME);
      if(held < req.volume) req.volume = held;
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
   MapPut(res.order, id);  // the fill itself arrives through OnTradeTransaction (the deal comment carries the id too)
}

void CancelOrder(const string line)
{
   const string id = JsonStr(line, "id");
   const string orig = JsonStr(line, "orig");
   const ulong ticket = TicketForId(orig);
   if(ticket == 0)
   {
      Send("{\"t\":\"cancel_reject\",\"id\":\"" + Esc(id) + "\",\"orig\":\"" + Esc(orig) +
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
      Send("{\"t\":\"cancel_reject\",\"id\":\"" + Esc(id) + "\",\"orig\":\"" + Esc(orig) + "\",\"text\":\"MT5 " +
           IntegerToString(res.retcode) + ": " + Esc(res.comment) + "\"}");
      return;
   }
   Send("{\"t\":\"cancelled\",\"id\":\"" + Esc(orig) + "\"}");
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
              Num(PositionGetDouble(POSITION_VOLUME), 4) + ",\"price\":" + Num(PositionGetDouble(POSITION_PRICE_OPEN), g_digits) + "}";
   }
   Send("{\"t\":\"positions\",\"req\":\"" + Esc(reqId) + "\",\"list\":[" + list + "]}");
}

void ListOrders(const string reqId)
{
   string list = "";
   for(int i = OrdersTotal() - 1; i >= 0; i--)
   {
      const ulong t = OrderGetTicket(i);
      if(t == 0) continue;
      if(OrderGetString(ORDER_SYMBOL) != g_sym || OrderGetInteger(ORDER_MAGIC) != MagicNumber) continue;
      const long type = OrderGetInteger(ORDER_TYPE);
      if(type != ORDER_TYPE_BUY_STOP && type != ORDER_TYPE_SELL_STOP) continue;
      string id = IdForOrder(t);
      if(id == "") id = OrderGetString(ORDER_COMMENT);
      if(list != "") list += ",";
      list += "{\"id\":\"" + Esc(id) + "\",\"ticket\":\"" + IntegerToString(t) + "\",\"buy\":" +
              (type == ORDER_TYPE_BUY_STOP ? "true" : "false") + ",\"volume\":" +
              Num(OrderGetDouble(ORDER_VOLUME_CURRENT), 4) + ",\"price\":" + Num(OrderGetDouble(ORDER_PRICE_OPEN), g_digits) + "}";
   }
   Send("{\"t\":\"orders\",\"req\":\"" + Esc(reqId) + "\",\"list\":[" + list + "]}");
}

//+------------------------------------------------------------------+
//| Fills: every deal of this EA's magic on the gold symbol           |
//+------------------------------------------------------------------+
void OnTradeTransaction(const MqlTradeTransaction& trans, const MqlTradeRequest& request, const MqlTradeResult& result)
{
   if(trans.type != TRADE_TRANSACTION_DEAL_ADD || trans.deal == 0) return;
   if(!HistoryDealSelect(trans.deal)) return;
   if(HistoryDealGetString(trans.deal, DEAL_SYMBOL) != g_sym) return;
   if(HistoryDealGetInteger(trans.deal, DEAL_MAGIC) != MagicNumber) return;
   const long dealType = HistoryDealGetInteger(trans.deal, DEAL_TYPE);
   if(dealType != DEAL_TYPE_BUY && dealType != DEAL_TYPE_SELL) return;
   const ulong order = (ulong)HistoryDealGetInteger(trans.deal, DEAL_ORDER);
   string id = IdForOrder(order);
   if(id == "") id = HistoryDealGetString(trans.deal, DEAL_COMMENT);
   if(id == "") return;  // not one of the bot's orders
   // What this deal really cost: commission + fee (positive = charged).
   const double cost = -(HistoryDealGetDouble(trans.deal, DEAL_COMMISSION) + HistoryDealGetDouble(trans.deal, DEAL_FEE));
   Send("{\"t\":\"fill\",\"id\":\"" + Esc(id) + "\",\"price\":" + Num(HistoryDealGetDouble(trans.deal, DEAL_PRICE), g_digits) +
        ",\"volume\":" + Num(HistoryDealGetDouble(trans.deal, DEAL_VOLUME), 4) + ",\"pos\":\"" +
        IntegerToString(HistoryDealGetInteger(trans.deal, DEAL_POSITION_ID)) + "\",\"buy\":" +
        (dealType == DEAL_TYPE_BUY ? "true" : "false") + ",\"comm\":" + Num(cost, 4) + "}");
   QueueAccount();
}
//+------------------------------------------------------------------+
