#include "muuk/ctrader_fix/live_runner.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
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

void LiveRunner::log(const std::string& line) const {
    if (logHandler_) {
        logHandler_(line);
    } else {
        std::cerr << line << "\n";
    }
}

LiveSnapshot LiveRunner::snapshot() const {
    LiveSnapshot snap;
    snap.symbolName = config_.connection.symbolName;
    snap.rangeLower = config_.strategy.range.lowerBound.toDouble(config_.strategy.symbol);
    snap.rangeUpper = config_.strategy.range.upperBound.toDouble(config_.strategy.symbol);

    std::scoped_lock lock(engineMutex_);
    snap.status = engine_.status();
    snap.symbolId = symbolId_;
    snap.activeOrderCount = engine_.activeOrderCount();
    const BasketPnlResult pnl = engine_.lastBasketPnl();
    snap.lastGrossPnlMoney = pnl.grossPnlMoney;
    snap.lastNetPnlMoney = pnl.netPnlMoney;
    snap.bid = latestBid_;
    snap.ask = latestAsk_;
    return snap;
}

void LiveRunner::stop() {
    // run() calls this itself on the way out (both on a clean stop and on
    // a startup failure), and a caller can also call it directly at any
    // time (e.g. a disconnect handler setting running_=false races with
    // an operator clicking Stop). Only the first caller tears the
    // sessions down; everyone else waits for that to finish instead of
    // racing on the same FixSession objects (concurrent stop() calls on
    // the same session used to be a real, intermittent crash here).
    if (stopping_.exchange(true)) {
        std::unique_lock lock(stopMutex_);
        stopCv_.wait(lock, [this] { return stopped_; });
        return;
    }

    running_ = false;
    if (tradeSession_) tradeSession_->stop();
    if (quoteSession_) quoteSession_->stop();

    {
        std::scoped_lock lock(stopMutex_);
        stopped_ = true;
    }
    stopCv_.notify_all();
}

void LiveRunner::run() {
    running_ = true;

    try {
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
                                                       quoteCreds,
                                                       [this](const FixMessage& m) { onQuoteMessage(m); });
        tradeSession_ = std::make_unique<FixSession>(config_.connection.trade.host, config_.connection.trade.port,
                                                       tradeCreds,
                                                       [this](const FixMessage& m) { onTradeMessage(m); });

        quoteSession_->setDisconnectHandler([this](const std::string& reason) {
            log("[quote session] ended: " + reason);
            running_ = false;
        });
        tradeSession_->setDisconnectHandler([this](const std::string& reason) {
            log("[trade session] ended: " + reason);
            running_ = false;
        });

        tradeSession_->start();
        quoteSession_->start();
        log("logged on to both Quote and Trade sessions");

        resolveSymbol();
        log("resolved symbol '" + config_.connection.symbolName + "' -> id " + symbolId_);

        {
            std::scoped_lock lock(engineMutex_);
            for (const auto& cmd : engine_.start()) handleCommand(cmd);
        }

        quoteSession_->send(buildMarketDataRequest("md-" + config_.connection.symbolName, symbolId_, true));
        log("subscribed to quotes; engine is now active");

        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    } catch (...) {
        // Ensure both sessions are always torn down before an exception
        // propagates — otherwise a caller that catches it (rather than
        // letting it destroy this LiveRunner) would leave whichever
        // sessions did start still connected in the background.
        stop();
        throw;
    }

    stop();
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
                log("[trade] ignoring unmatched/incomplete fill report (ClOrdID=" + info.clOrdId.value_or("?") +
                    ")");
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
            log("[trade] order rejected (ClOrdID=" + info.clOrdId.value_or("?") +
                "): " + info.text.value_or("no reason given"));
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
        log("[trade] cancel request rejected (OrigClOrdID=" + info.origClOrdId.value_or("?") +
            "): " + info.text.value_or("no reason given"));
        return;
    }

    if (type == msgtype::BusinessMessageReject) {
        std::ostringstream oss;
        oss << "[trade] BusinessMessageReject: refMsgType=" << msg.getOptionalString(tag::RefMsgType).value_or("?")
            << " reason=" << msg.getOptionalString(tag::BusinessRejectReason).value_or("?")
            << " text=" << msg.getOptionalString(tag::Text).value_or("no reason given");
        log(oss.str());
        return;
    }

    log("[trade] unhandled message type: " + type);
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
        log("[quote] market data subscription was rejected: " + msg.getOptionalString(tag::Text).value_or("no reason given"));
        return;
    }

    log("[quote] unhandled message type: " + type);
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
        const double lots = place->volume.toDouble(config_.strategy.symbol);
        // cTrader's FIX OrderQty(38) is in "lot hundredths" (its
        // platform-wide volume convention), not raw lots -- confirmed
        // empirically: sending OrderQty=2/3 opened 0.02/0.03-lot
        // positions, and the broker's stated minimum ("1.00") turned out
        // to mean 0.01 lot, not 1.00 lot. engine/core's SymbolSpec/Volume
        // stay in real lots throughout (P&L, risk checks); this scaling
        // is purely a wire-format detail of this one broker/dialect, so it
        // belongs here rather than in engine/core.
        const double qty = lots * 100.0;
        const double stopPx = place->price.toDouble(config_.strategy.symbol);
        std::ostringstream oss;
        oss << "[order] placing seq=" << place->sequenceIndex << " side=" << side << " lots=" << lots
            << " stopPx=" << stopPx << " (ClOrdID=" << clOrdId << ")";
        log(oss.str());
        tradeSession_->send(buildNewOrderSingle(clOrdId, symbolId_, side, qty, stopPx));
    } else if (const auto* cancel = std::get_if<CancelOrderCommand>(&cmd)) {
        const std::string cancelClOrdId = "c" + std::to_string(nextCancelReqId_++);
        log("[order] cancelling OrigClOrdID=" + std::to_string(cancel->id));
        tradeSession_->send(buildOrderCancelRequest(cancelClOrdId, std::to_string(cancel->id)));
    } else if (const auto* close = std::get_if<CloseBasketCommand>(&cmd)) {
        log("[basket] closed, net P/L = " + std::to_string(close->netPnlMoney));
    } else if (const auto* alert = std::get_if<RiskAlertCommand>(&cmd)) {
        log("[risk] ALERT: limit breached (kind=" + std::to_string(static_cast<int>(alert->kind)) +
            ") — no further orders will be armed this cycle");
    }
}

}  // namespace muuk::ctrader_fix
