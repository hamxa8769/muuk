#pragma once

#include <optional>
#include <string>
#include <vector>

#include "muuk/ctrader_fix/fix_message.hpp"

namespace muuk::ctrader_fix {

// --- Session-level message builders ----------------------------------------
// These build only the application-specific fields; FixSession::send() adds
// the standard header (SenderCompID/TargetCompID/TargetSubID/SenderSubID/
// MsgSeqNum/SendingTime) uniformly for every outgoing message.

[[nodiscard]] FixMessage buildLogon(const std::string& username, const std::string& password,
                                     int heartBtIntSeconds);
[[nodiscard]] FixMessage buildLogout(std::optional<std::string> text = std::nullopt);
[[nodiscard]] FixMessage buildHeartbeat(std::optional<std::string> testReqId = std::nullopt);
[[nodiscard]] FixMessage buildTestRequest(const std::string& testReqId);

// --- Market data ------------------------------------------------------------

[[nodiscard]] FixMessage buildMarketDataRequest(const std::string& mdReqId, const std::string& symbolId,
                                                 bool subscribe);

enum class MDEntryKind { Bid, Offer };

struct MDEntry {
    MDEntryKind kind;
    double price = 0.0;
};

// Parses a MarketDataSnapshotFullRefresh (35=W): all entries present.
[[nodiscard]] std::vector<MDEntry> parseMarketDataSnapshot(const FixMessage& msg);

// --- Symbol discovery --------------------------------------------------------

// Requests the full instrument list (Symbol(55) omitted -> "give me
// everything"); the response's repeating group is then filtered client-side
// for the symbol name we care about. See docs/ctrader-fix.md for why: tag 55
// in the *request* is for the reverse lookup (id -> name), not name -> id.
[[nodiscard]] FixMessage buildSecurityListRequest(const std::string& securityReqId);

struct SecurityListEntry {
    std::string symbolId;
    std::string symbolName;
    std::optional<int> symbolDigits;
};

[[nodiscard]] std::vector<SecurityListEntry> parseSecurityList(const FixMessage& msg);

// --- Orders -------------------------------------------------------------------

// Every pending order this engine ever places is a Stop order (OrdType=3);
// direction comes from `side` (1=Buy, 2=Sell) per the FIX spec — there is no
// separate "Buy Stop"/"Sell Stop" order type.
[[nodiscard]] FixMessage buildNewOrderSingle(const std::string& clOrdId, const std::string& symbolId, int side,
                                              double orderQty, double stopPx);

// A Market order (OrdType=1), used only to close an open position; the
// strategy's own entries are always the Stop orders above.
//
// `positionId` goes in PosMaintRptID(721): per the cTrader FIX spec, "a
// position ID where this order should be placed. If not set, a new
// position will be created." On a hedging account an opposite-side order
// *without* it therefore opens a brand-new position instead of closing
// anything -- it must carry the id of the position being closed (taken
// from tag 721 of the ExecutionReport that opened it, or from a Position
// Report). Empty = omit the tag (only correct on a netting account).
[[nodiscard]] FixMessage buildMarketOrderSingle(const std::string& clOrdId, const std::string& symbolId, int side,
                                                 double orderQty, const std::string& positionId = {});

// Request For Positions (35=AN) without PosMaintRptID: asks for every
// open position on the account. Answered by one Position Report per
// position (or a single "none found" report).
[[nodiscard]] FixMessage buildRequestForPositions(const std::string& posReqId);

struct PositionReportInfo {
    std::string posReqId;
    bool anyPositions = false;          // PosReqResult(728) == 0
    int totalReports = 0;               // TotalNumPosReports(727)
    std::optional<std::string> positionId;
    std::optional<std::string> symbolId;
    double longQty = 0.0;               // wire units, same as OrderQty
    double shortQty = 0.0;
    std::optional<double> settlPrice;   // average open price
};

[[nodiscard]] PositionReportInfo parsePositionReport(const FixMessage& msg);

[[nodiscard]] FixMessage buildOrderCancelRequest(const std::string& clOrdId, const std::string& origClOrdId,
                                                  std::optional<std::string> orderId = std::nullopt);

enum class ExecType : char {
    New = '0',
    Cancelled = '4',
    Rejected = '8',
    Expired = 'C',
    Trade = 'F',
    OrderStatus = 'I',
    Unknown = '?',
};

enum class OrdStatus : char {
    New = '0',
    PartiallyFilled = '1',
    Filled = '2',
    Cancelled = '4',
    Rejected = '8',
    Expired = 'C',
    Unknown = '?',
};

struct ExecutionReportInfo {
    std::string orderId;
    std::optional<std::string> clOrdId;
    ExecType execType = ExecType::Unknown;
    OrdStatus ordStatus = OrdStatus::Unknown;
    std::optional<double> avgPx;
    std::optional<double> lastQty;
    std::optional<double> leavesQty;
    std::optional<double> cumQty;
    std::optional<std::string> text;
    // Only if the broker includes them -- not relied on for P/L (the
    // configured rate is used); logged so the two can be compared.
    std::optional<double> commission;
    std::optional<std::string> commType;
    // PosMaintRptID(721): the id of the position this fill opened (or
    // closed). Needed to close that exact position later on a hedging
    // account -- see buildMarketOrderSingle.
    std::optional<std::string> positionId;
};

[[nodiscard]] ExecutionReportInfo parseExecutionReport(const FixMessage& msg);

struct OrderCancelRejectInfo {
    std::string orderId;
    std::string clOrdId;
    std::optional<std::string> origClOrdId;
    OrdStatus ordStatus = OrdStatus::Unknown;
    std::optional<std::string> text;
};

[[nodiscard]] OrderCancelRejectInfo parseOrderCancelReject(const FixMessage& msg);

// Current UTC time formatted as a FIX UTCTimestamp (YYYYMMDD-HH:MM:SS),
// used for TransactTime. Declared here so tests can verify the format
// without depending on wall-clock time.
[[nodiscard]] std::string nowUtcTimestamp();

}  // namespace muuk::ctrader_fix
