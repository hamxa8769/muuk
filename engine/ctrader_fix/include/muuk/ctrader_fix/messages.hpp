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
