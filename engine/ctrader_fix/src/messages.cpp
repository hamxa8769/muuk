#include "muuk/ctrader_fix/messages.hpp"

#include <chrono>
#include <ctime>

#include "muuk/ctrader_fix/tags.hpp"

namespace muuk::ctrader_fix {

std::string nowUtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H:%M:%S", &utc);
    return std::string(buf);
}

FixMessage buildLogon(const std::string& username, const std::string& password, int heartBtIntSeconds) {
    FixMessage msg(msgtype::Logon);
    msg.set(tag::EncryptMethod, int64_t{0});
    msg.set(tag::HeartBtInt, int64_t{heartBtIntSeconds});
    msg.set(tag::ResetSeqNumFlag, std::string("Y"));
    if (!username.empty()) msg.set(tag::Username, username);
    if (!password.empty()) msg.set(tag::Password, password);
    return msg;
}

FixMessage buildLogout(std::optional<std::string> text) {
    FixMessage msg(msgtype::Logout);
    if (text) msg.set(tag::Text, *text);
    return msg;
}

FixMessage buildHeartbeat(std::optional<std::string> testReqId) {
    FixMessage msg(msgtype::Heartbeat);
    if (testReqId) msg.set(tag::TestReqID, *testReqId);
    return msg;
}

FixMessage buildTestRequest(const std::string& testReqId) {
    FixMessage msg(msgtype::TestRequest);
    msg.set(tag::TestReqID, testReqId);
    return msg;
}

FixMessage buildMarketDataRequest(const std::string& mdReqId, const std::string& symbolId, bool subscribe) {
    FixMessage msg(msgtype::MarketDataRequest);
    msg.set(tag::MDReqID, mdReqId);
    msg.set(tag::SubscriptionRequestType, std::string(subscribe ? "1" : "2"));
    msg.set(tag::MarketDepth, int64_t{1});  // spot (top of book), not full depth
    msg.set(tag::MDUpdateType, int64_t{1});
    msg.set(tag::NoRelatedSym, int64_t{1});
    msg.set(tag::Symbol, symbolId);
    msg.set(tag::NoMDEntryTypes, int64_t{2});
    msg.set(tag::MDEntryType, std::string("0"));
    msg.set(tag::MDEntryType, std::string("1"));
    return msg;
}

std::vector<MDEntry> parseMarketDataSnapshot(const FixMessage& msg) {
    std::vector<MDEntry> entries;
    const auto types = msg.getAll(tag::MDEntryType);
    const auto prices = msg.getAll(tag::MDEntryPx);
    for (std::size_t i = 0; i < types.size() && i < prices.size(); ++i) {
        const MDEntryKind kind = (types[i] == "1") ? MDEntryKind::Offer : MDEntryKind::Bid;
        entries.push_back(MDEntry{kind, std::stod(prices[i])});
    }
    return entries;
}

FixMessage buildSecurityListRequest(const std::string& securityReqId) {
    FixMessage msg(msgtype::SecurityListRequest);
    msg.set(tag::SecurityReqID, securityReqId);
    msg.set(tag::SecurityListRequestType, int64_t{0});
    return msg;
}

std::vector<SecurityListEntry> parseSecurityList(const FixMessage& msg) {
    std::vector<SecurityListEntry> out;
    const auto ids = msg.getAll(tag::Symbol);
    const auto names = msg.getAll(tag::SymbolName);
    const auto digits = msg.getAll(tag::SymbolDigits);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        SecurityListEntry entry;
        entry.symbolId = ids[i];
        entry.symbolName = (i < names.size()) ? names[i] : std::string{};
        if (i < digits.size()) {
            try {
                entry.symbolDigits = std::stoi(digits[i]);
            } catch (const std::exception&) {
                entry.symbolDigits = std::nullopt;
            }
        }
        out.push_back(std::move(entry));
    }
    return out;
}

FixMessage buildNewOrderSingle(const std::string& clOrdId, const std::string& symbolId, int side,
                                double orderQty, double stopPx) {
    FixMessage msg(msgtype::NewOrderSingle);
    msg.set(tag::ClOrdID, clOrdId);
    msg.set(tag::Symbol, symbolId);
    msg.set(tag::Side, int64_t{side});
    msg.set(tag::TransactTime, nowUtcTimestamp());
    msg.set(tag::OrderQty, orderQty, 2);
    msg.set(tag::OrdType, int64_t{3});  // Stop — see header comment
    msg.set(tag::StopPx, stopPx, 5);
    return msg;
}

FixMessage buildMarketOrderSingle(const std::string& clOrdId, const std::string& symbolId, int side,
                                   double orderQty, const std::string& positionId) {
    FixMessage msg(msgtype::NewOrderSingle);
    msg.set(tag::ClOrdID, clOrdId);
    msg.set(tag::Symbol, symbolId);
    msg.set(tag::Side, int64_t{side});
    msg.set(tag::TransactTime, nowUtcTimestamp());
    msg.set(tag::OrderQty, orderQty, 2);
    msg.set(tag::OrdType, int64_t{1});  // Market -- cTrader always executes it Immediate-or-Cancel
    if (!positionId.empty()) msg.set(tag::PosMaintRptID, positionId);
    return msg;
}

FixMessage buildRequestForPositions(const std::string& posReqId) {
    FixMessage msg(msgtype::RequestForPositions);
    msg.set(tag::PosReqID, posReqId);
    return msg;
}

FixMessage buildOrderCancelRequest(const std::string& clOrdId, const std::string& origClOrdId,
                                    std::optional<std::string> orderId) {
    FixMessage msg(msgtype::OrderCancelRequest);
    msg.set(tag::OrigClOrdID, origClOrdId);
    if (orderId) msg.set(tag::OrderID, *orderId);
    msg.set(tag::ClOrdID, clOrdId);
    return msg;
}

namespace {

ExecType parseExecType(const std::optional<std::string>& v) {
    if (!v || v->empty()) return ExecType::Unknown;
    switch ((*v)[0]) {
        case '0': return ExecType::New;
        case '4': return ExecType::Cancelled;
        case '8': return ExecType::Rejected;
        case 'C': return ExecType::Expired;
        case 'F': return ExecType::Trade;
        case 'I': return ExecType::OrderStatus;
        default: return ExecType::Unknown;
    }
}

OrdStatus parseOrdStatus(const std::optional<std::string>& v) {
    if (!v || v->empty()) return OrdStatus::Unknown;
    switch ((*v)[0]) {
        case '0': return OrdStatus::New;
        case '1': return OrdStatus::PartiallyFilled;
        case '2': return OrdStatus::Filled;
        case '4': return OrdStatus::Cancelled;
        case '8': return OrdStatus::Rejected;
        case 'C': return OrdStatus::Expired;
        default: return OrdStatus::Unknown;
    }
}

std::optional<double> optionalDouble(const std::optional<std::string>& v) {
    if (!v) return std::nullopt;
    try {
        return std::stod(*v);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

ExecutionReportInfo parseExecutionReport(const FixMessage& msg) {
    ExecutionReportInfo info;
    info.orderId = msg.getString(tag::OrderID);
    info.clOrdId = msg.getOptionalString(tag::ClOrdID);
    info.execType = parseExecType(msg.getOptionalString(tag::ExecType));
    info.ordStatus = parseOrdStatus(msg.getOptionalString(tag::OrdStatus));
    info.avgPx = optionalDouble(msg.getOptionalString(tag::AvgPx));
    info.lastQty = optionalDouble(msg.getOptionalString(tag::LastQty));
    info.leavesQty = optionalDouble(msg.getOptionalString(tag::LeavesQty));
    info.cumQty = optionalDouble(msg.getOptionalString(tag::CumQty));
    info.text = msg.getOptionalString(tag::Text);
    info.commission = optionalDouble(msg.getOptionalString(tag::Commission));
    info.commType = msg.getOptionalString(tag::CommType);
    info.positionId = msg.getOptionalString(tag::PosMaintRptID);
    return info;
}

OrderCancelRejectInfo parseOrderCancelReject(const FixMessage& msg) {
    OrderCancelRejectInfo info;
    info.orderId = msg.getString(tag::OrderID);
    info.clOrdId = msg.getString(tag::ClOrdID);
    info.origClOrdId = msg.getOptionalString(tag::OrigClOrdID);
    info.ordStatus = parseOrdStatus(msg.getOptionalString(tag::OrdStatus));
    info.text = msg.getOptionalString(tag::Text);
    return info;
}

PositionReportInfo parsePositionReport(const FixMessage& msg) {
    PositionReportInfo info;
    info.posReqId = msg.getOptionalString(tag::PosReqID).value_or("");
    info.anyPositions = msg.getOptionalString(tag::PosReqResult).value_or("2") == "0";
    info.totalReports = static_cast<int>(optionalDouble(msg.getOptionalString(tag::TotalNumPosReports)).value_or(0.0));
    info.positionId = msg.getOptionalString(tag::PosMaintRptID);
    info.symbolId = msg.getOptionalString(tag::Symbol);
    info.longQty = optionalDouble(msg.getOptionalString(tag::LongQty)).value_or(0.0);
    info.shortQty = optionalDouble(msg.getOptionalString(tag::ShortQty)).value_or(0.0);
    info.settlPrice = optionalDouble(msg.getOptionalString(tag::SettlPrice));
    return info;
}

}  // namespace muuk::ctrader_fix
