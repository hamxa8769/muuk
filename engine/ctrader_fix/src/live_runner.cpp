#include "muuk/ctrader_fix/live_runner.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"

using namespace muuk::core;

namespace muuk::ctrader_fix {

namespace {
std::optional<OrderId> tryParseOrderId(const std::optional<std::string>& clOrdId) {
    if (!clOrdId) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const unsigned long long v = std::stoull(*clOrdId, &consumed);
        if (consumed != clOrdId->size()) return std::nullopt;
        return static_cast<OrderId>(v);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
}  // namespace

LiveRunner::LiveRunner(LiveConfig config) : config_(std::move(config)), engine_(config_.strategy) {}

LiveRunner::~LiveRunner() { stop(); }

void LiveRunner::stop() {
    running_ = false;
    if (tradeSession_) tradeSession_->stop();
    if (quoteSession_) quoteSession_->stop();
}

void LiveRunner::run() {
    running_ = true;

    FixCredentials quoteCreds;
    quoteCreds.senderCompId = config_.connection.senderCompId();
    quoteCreds.targetCompId = config_.connection.targetCompId;
    quoteCreds.targetSubId = "QUOTE";
    quoteCreds.senderSubId = "QUOTE";
    quoteCreds.username = config_.connection.traderLogin;
    quoteCreds.password = config_.connection.password;
    quoteCreds.heartBtIntSeconds = config_.connection.heartBtIntSeconds;

    FixCredentials tradeCreds = quoteCreds;
    tradeCreds.targetSubId = "TRADE";
    tradeCreds.senderSubId = "TRADE";

    quoteSession_ = std::make_unique<FixSession>(config_.connection.quote.host, config_.connection.quote.port,
                                                  quoteCreds, [this](const FixMessage& m) { onQuoteMessage(m); });
    tradeSession_ = std::make_unique<FixSession>(config_.connection.trade.host, config_.connection.trade.port,
                                                  tradeCreds, [this](const FixMessage& m) { onTradeMessage(m); });

    quoteSession_->setDisconnectHandler([this](const std::string& reason) {
        std::cerr << "[quote session] ended: " << reason << "\n";
        running_ = false;
    });
    tradeSession_->setDisconnectHandler([this](const std::string& reason) {
        std::cerr << "[trade session] ended: " << reason << "\n";
        running_ = false;
    });

    tradeSession_->start();
    quoteSession_->start();
    std::cerr << "logged on to both Quote and Trade sessions\n";

    resolveSymbol();
    std::cerr << "resolved symbol '" << config_.connection.symbolName << "' -> id " << symbolId_ << "\n";

    {
        std::scoped_lock lock(engineMutex_);
        for (const auto& cmd : engine_.start()) handleCommand(cmd);
    }

    quoteSession_->send(buildMarketDataRequest("md-" + config_.connection.symbolName, symbolId_, true));
    std::cerr << "subscribed to quotes; engine is now active\n";

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (tradeSession_) tradeSession_->stop();
    if (quoteSession_) quoteSession_->stop();
}

void LiveRunner::resolveSymbol() {
    if (!config_.connection.symbolId.empty()) {
        // Already known (e.g. from the same cTrader FIX API settings
        // screen the connection details came from) — skip the
        // SecurityListRequest round-trip entirely.
        symbolId_ = config_.connection.symbolId;
        return;
    }

    std::unique_lock lock(symbolMutex_);
    symbolResolved_ = false;
    symbolId_.clear();
    tradeSession_->send(buildSecurityListRequest("sec-1"));

    const bool settled = symbolCv_.wait_for(lock, std::chrono::seconds(10), [this] { return symbolResolved_; });
    if (!settled) {
        throw std::runtime_error("timed out waiting for SecurityList response");
    }
    if (symbolId_.empty()) {
        throw std::runtime_error("symbol '" + config_.connection.symbolName + "' was not found in the broker's SecurityList");
    }
}

void LiveRunner::onTradeMessage(const FixMessage& msg) {
    const std::string& type = msg.msgType();

    if (type == msgtype::SecurityList) {
        const auto entries = parseSecurityList(msg);
        std::scoped_lock lock(symbolMutex_);
        for (const auto& entry : entries) {
            if (entry.symbolName == config_.connection.symbolName) {
                symbolId_ = entry.symbolId;
                break;
            }
        }
        symbolResolved_ = true;
        symbolCv_.notify_all();
        return;
    }

    if (type == msgtype::ExecutionReport) {
        const ExecutionReportInfo info = parseExecutionReport(msg);

        if (info.execType == ExecType::Trade) {
            const auto orderId = tryParseOrderId(info.clOrdId);
            if (!orderId || !info.avgPx) {
                std::cerr << "[trade] ignoring unmatched/incomplete fill report (ClOrdID="
                          << info.clOrdId.value_or("?") << ")\n";
                return;
            }
            const Price fillPrice = Price::fromDouble(*info.avgPx, config_.strategy.symbol);
            std::scoped_lock lock(engineMutex_);
            for (const auto& cmd : engine_.onOrderConfirmed(OrderConfirmedEvent{*orderId, fillPrice})) {
                handleCommand(cmd);
            }
            return;
        }

        if (info.execType == ExecType::Rejected) {
            const auto orderId = tryParseOrderId(info.clOrdId);
            std::cerr << "[trade] order rejected (ClOrdID=" << info.clOrdId.value_or("?")
                       << "): " << info.text.value_or("no reason given") << "\n";
            if (!orderId) return;
            std::scoped_lock lock(engineMutex_);
            for (const auto& cmd :
                 engine_.onOrderRejected(OrderRejectedEvent{*orderId, RejectReason::BrokerRejected})) {
                handleCommand(cmd);
            }
            return;
        }

        // New/Cancelled/Expired/OrderStatus: informational only. The engine
        // already updated its own state when it emitted the Place/Cancel
        // command; a broker-side cancel/expiry confirmation doesn't need
        // to be replayed back into it.
        return;
    }

    if (type == msgtype::OrderCancelReject) {
        const OrderCancelRejectInfo info = parseOrderCancelReject(msg);
        std::cerr << "[trade] cancel request rejected (OrigClOrdID=" << info.origClOrdId.value_or("?")
                   << "): " << info.text.value_or("no reason given") << "\n";
        return;
    }

    std::cerr << "[trade] unhandled message type: " << type << "\n";
}

void LiveRunner::onQuoteMessage(const FixMessage& msg) {
    const std::string& type = msg.msgType();

    if (type == msgtype::MarketDataSnapshotFullRefresh || type == msgtype::MarketDataIncrementalRefresh) {
        const auto entries = parseMarketDataSnapshot(msg);
        std::scoped_lock lock(engineMutex_);
        for (const auto& entry : entries) {
            if (entry.kind == MDEntryKind::Bid) {
                latestBid_ = entry.price;
            } else {
                latestAsk_ = entry.price;
            }
        }
        maybeEmitTick();
        return;
    }

    if (type == msgtype::MarketDataRequestReject) {
        std::cerr << "[quote] market data subscription was rejected: "
                   << msg.getOptionalString(tag::Text).value_or("no reason given") << "\n";
        return;
    }

    std::cerr << "[quote] unhandled message type: " << type << "\n";
}

void LiveRunner::maybeEmitTick() {
    if (!latestBid_ || !latestAsk_) return;

    const TickEvent tick{Price::fromDouble(*latestBid_, config_.strategy.symbol),
                          Price::fromDouble(*latestAsk_, config_.strategy.symbol)};
    for (const auto& cmd : engine_.onTick(tick)) handleCommand(cmd);
}

void LiveRunner::handleCommand(const Command& cmd) {
    if (const auto* place = std::get_if<PlaceOrderCommand>(&cmd)) {
        const std::string clOrdId = std::to_string(place->id);
        const int side = (place->side == OrderSide::BuyStop) ? 1 : 2;
        const double qty = place->volume.toDouble(config_.strategy.symbol);
        const double stopPx = place->price.toDouble(config_.strategy.symbol);
        std::cerr << "[order] placing seq=" << place->sequenceIndex << " side=" << side << " qty=" << qty
                   << " stopPx=" << stopPx << " (ClOrdID=" << clOrdId << ")\n";
        tradeSession_->send(buildNewOrderSingle(clOrdId, symbolId_, side, qty, stopPx));
    } else if (const auto* cancel = std::get_if<CancelOrderCommand>(&cmd)) {
        const std::string cancelClOrdId = "c" + std::to_string(nextCancelReqId_++);
        std::cerr << "[order] cancelling OrigClOrdID=" << cancel->id << "\n";
        tradeSession_->send(buildOrderCancelRequest(cancelClOrdId, std::to_string(cancel->id)));
    } else if (const auto* close = std::get_if<CloseBasketCommand>(&cmd)) {
        std::cerr << "[basket] closed, net P/L = " << close->netPnlMoney << "\n";
    } else if (const auto* alert = std::get_if<RiskAlertCommand>(&cmd)) {
        std::cerr << "[risk] ALERT: limit breached (kind=" << static_cast<int>(alert->kind)
                   << ") — no further orders will be armed this cycle\n";
    }
}

}  // namespace muuk::ctrader_fix
