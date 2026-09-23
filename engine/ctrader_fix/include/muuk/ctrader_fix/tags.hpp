#pragma once

// FIX tag numbers used by the cTrader FIX API dialect (see
// docs/ctrader-fix.md for the source spec). Named here once so message
// builders/parsers never spell out a bare magic number.

namespace muuk::ctrader_fix::tag {

inline constexpr int BeginString = 8;
inline constexpr int BodyLength = 9;
inline constexpr int CheckSum = 10;
inline constexpr int MsgType = 35;
inline constexpr int SenderCompID = 49;
inline constexpr int TargetCompID = 56;
inline constexpr int TargetSubID = 57;
inline constexpr int SenderSubID = 50;
inline constexpr int MsgSeqNum = 34;
inline constexpr int SendingTime = 52;

inline constexpr int TestReqID = 112;
inline constexpr int EncryptMethod = 98;
inline constexpr int HeartBtInt = 108;
inline constexpr int ResetSeqNumFlag = 141;
inline constexpr int Username = 553;
inline constexpr int Password = 554;
inline constexpr int Text = 58;
inline constexpr int BeginSeqNo = 7;
inline constexpr int EndSeqNo = 16;
inline constexpr int RefSeqNum = 45;
inline constexpr int SessionRejectReason = 373;
inline constexpr int GapFillFlag = 123;
inline constexpr int NewSeqNo = 36;

inline constexpr int MDReqID = 262;
inline constexpr int SubscriptionRequestType = 263;
inline constexpr int MarketDepth = 264;
inline constexpr int MDUpdateType = 265;
inline constexpr int NoMDEntryTypes = 267;
inline constexpr int MDEntryType = 269;
inline constexpr int NoRelatedSym = 146;
inline constexpr int Symbol = 55;
inline constexpr int NoMDEntries = 268;
inline constexpr int MDEntryPx = 270;
inline constexpr int MDEntrySize = 271;
inline constexpr int MDEntryID = 278;
inline constexpr int MDUpdateAction = 279;
inline constexpr int MDReqRejReason = 281;

inline constexpr int ClOrdID = 11;
inline constexpr int Commission = 12;
inline constexpr int CommType = 13;
inline constexpr int Side = 54;
inline constexpr int TransactTime = 60;
inline constexpr int OrderQty = 38;
inline constexpr int OrdType = 40;
inline constexpr int Price = 44;
inline constexpr int StopPx = 99;
inline constexpr int TimeInForce = 59;
inline constexpr int ExpireTime = 126;
inline constexpr int PosMaintRptID = 721;
inline constexpr int Designation = 494;

inline constexpr int OrderID = 37;
inline constexpr int TotNumReports = 911;
inline constexpr int ExecType = 150;
inline constexpr int OrdStatus = 39;
inline constexpr int AvgPx = 6;
inline constexpr int LeavesQty = 151;
inline constexpr int CumQty = 14;
inline constexpr int LastQty = 32;
inline constexpr int OrdRejReason = 103;
inline constexpr int MassStatusReqID = 584;

inline constexpr int RefMsgType = 372;
inline constexpr int BusinessRejectRefID = 379;
inline constexpr int BusinessRejectReason = 380;

inline constexpr int OrigClOrdID = 41;
inline constexpr int CxlRejResponseTo = 434;

inline constexpr int MassStatusReqType = 585;
inline constexpr int IssueDate = 225;

inline constexpr int SecurityReqID = 320;
inline constexpr int SecurityListRequestType = 559;
inline constexpr int SecurityResponseID = 322;
inline constexpr int SecurityRequestResult = 560;
inline constexpr int SymbolName = 1007;
inline constexpr int SymbolDigits = 1008;

// Request For Positions (AN) / Position Report (AP).
inline constexpr int PosReqID = 710;
inline constexpr int TotalNumPosReports = 727;
inline constexpr int PosReqResult = 728;
inline constexpr int NoPositions = 702;
inline constexpr int LongQty = 704;
inline constexpr int ShortQty = 705;
inline constexpr int SettlPrice = 730;

}  // namespace muuk::ctrader_fix::tag

// MsgType(35) string values.
namespace muuk::ctrader_fix::msgtype {

inline constexpr const char* Heartbeat = "0";
inline constexpr const char* TestRequest = "1";
inline constexpr const char* ResendRequest = "2";
inline constexpr const char* Reject = "3";
inline constexpr const char* SequenceReset = "4";
inline constexpr const char* Logout = "5";
inline constexpr const char* ExecutionReport = "8";
inline constexpr const char* OrderCancelReject = "9";
inline constexpr const char* Logon = "A";
inline constexpr const char* NewOrderSingle = "D";
inline constexpr const char* OrderCancelRequest = "F";
inline constexpr const char* OrderCancelReplaceRequest = "G";
inline constexpr const char* OrderStatusRequest = "H";
inline constexpr const char* MarketDataRequest = "V";
inline constexpr const char* MarketDataSnapshotFullRefresh = "W";
inline constexpr const char* MarketDataIncrementalRefresh = "X";
inline constexpr const char* MarketDataRequestReject = "Y";
inline constexpr const char* SecurityListRequest = "x";
inline constexpr const char* SecurityList = "y";
inline constexpr const char* BusinessMessageReject = "j";
inline constexpr const char* OrderMassStatusRequest = "AF";
inline constexpr const char* RequestForPositions = "AN";
inline constexpr const char* PositionReport = "AP";

}  // namespace muuk::ctrader_fix::msgtype
