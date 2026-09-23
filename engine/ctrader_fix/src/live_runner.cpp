#include "muuk/ctrader_fix/live_runner.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

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

LiveRunner::LiveRunner(LiveConfig config) : config_(std::move(config)) {}

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

    std::scoped_lock lock(engineMutex_);
    const SymbolSpec& spec = config_.strategy.symbol;
    // Before the engine exists, config_.strategy.range may still be
    // rewritten by auto-centering (under this same lock); once it exists,
    // the engine's own range is the one in force (seed mode re-anchors it).
    const RangeConfig range = engine_ ? engine_->currentRange() : config_.strategy.range;
    snap.rangeLower = range.lowerBound.toDouble(spec);
    snap.rangeUpper = range.upperBound.toDouble(spec);
    snap.symbolId = symbolId_;
    if (engine_) {
        snap.status = engine_->status();
        snap.activeOrderCount = engine_->activeOrderCount();
        snap.seeding = engine_->seeding();
        const BasketPnlResult pnl = engine_->lastBasketPnl();
        snap.lastGrossPnlMoney = pnl.grossPnlMoney;
        snap.lastNetPnlMoney = pnl.netPnlMoney;
    }
    snap.connected = engine_ != nullptr && running_.load();
    snap.bid = latestBid_;
    snap.ask = latestAsk_;

    const bool haveQuote = latestBid_.has_value() && latestAsk_.has_value();
    const Price bid = haveQuote ? Price::fromDouble(*latestBid_, spec) : Price{};
    const Price ask = haveQuote ? Price::fromDouble(*latestAsk_, spec) : Price{};
    snap.positions.reserve(openLegs_.size());
    for (const auto& leg : openLegs_) {
        const double lots = leg.volume.toDouble(spec);
        double pnl = 0.0;
        if (haveQuote) {
            const int64_t diffTicks =
                (leg.side == OrderSide::BuyStop) ? bid.ticks - leg.price.ticks : leg.price.ticks - ask.ticks;
            pnl = static_cast<double>(diffTicks) * spec.tickValue * lots;
        }
        snap.positions.push_back(PositionInfo{leg.id, leg.side, lots, leg.price.toDouble(spec), pnl});
    }
    snap.pendingOrders.reserve(placedOrderInfo_.size());
    for (const auto& [orderId, leg] : placedOrderInfo_) {
        snap.pendingOrders.push_back(
            PendingOrderInfo{orderId, leg.side, leg.volume.toDouble(spec), leg.price.toDouble(spec)});
    }
    std::sort(snap.pendingOrders.begin(), snap.pendingOrders.end(),
              [](const PendingOrderInfo& a, const PendingOrderInfo& b) { return a.id < b.id; });

    snap.closingInProgress = !pendingCloseClOrdIds_.empty();
    snap.closeFailed = basketCloseFailed_;
    if (cooldownActive_) {
        const auto left = std::chrono::ceil<std::chrono::seconds>(cooldownUntil_ - std::chrono::steady_clock::now());
        snap.cooldownRemainingSeconds = static_cast<int>(std::max<std::chrono::seconds::rep>(0, left.count()));
    }
    snap.basketsClosed = basketsClosed_;
    snap.realizedPnl = realizedPnl_;
    snap.settings = settingsFromConfig(config_);
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

        // Subscribed before the engine is constructed: if auto-centering
        // is enabled, a real tick needs to arrive first so there's a live
        // price to center on. If it isn't, this ordering is harmless —
        // onQuoteMessage/maybeEmitTick are no-ops until engine_ exists.
        quoteSession_->send(buildMarketDataRequest("md-" + config_.connection.symbolName, symbolId_, true));
        log("subscribed to quotes");

        const bool autoRange = config_.connection.autoRangeWidthPoints > 0.0;
        if (autoRange || config_.strategy.seedOffsetPoints > 0.0) {
            waitForFirstQuote();
        }

        bool started = false;
        {
            std::scoped_lock lock(engineMutex_);
            if (autoRange) centerRangeOnLatestQuote();
            engine_ = std::make_unique<StrategyEngine>(config_.strategy);
            // Hand the engine the live quote before start(): it's Idle, so
            // this only records the tick -- but seed orders are placed off
            // that tick's bid/ask, and without it they'd fall back to the
            // range midpoint and could land inside the spread.
            if (latestBid_ && latestAsk_) (void)engine_->onTick(latestTick());
            if (config_.connection.autoContinue) {
                for (const auto& cmd : engine_->start()) handleCommand(cmd);
                started = true;
            }
        }
        log(started ? "engine is now active" : "connected -- waiting for Open range");

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
            // The basket-close market orders aren't tracked by the engine
            // (they have no corresponding PendingOrder) -- they're matched
            // by ClOrdID here instead, and only once *every* leg's closing
            // order has filled is the next cycle allowed to start. See
            // maybeRestartAfterBasketClose().
            if (info.clOrdId && pendingCloseClOrdIds_.count(*info.clOrdId) > 0) {
                std::scoped_lock lock(engineMutex_);
                pendingCloseClOrdIds_.erase(*info.clOrdId);
                log("[basket] closing trade filled (ClOrdID=" + *info.clOrdId + "), " +
                    std::to_string(pendingCloseClOrdIds_.size()) + " leg(s) still outstanding");
                if (pendingCloseClOrdIds_.empty() && engine_) {
                    log("[basket] all legs flat, next cycle may now start");
                    maybeRestartAfterBasketClose();
                }
                return;
            }

            const auto orderId = tryParseOrderId(info.clOrdId);
            if (!orderId || !info.avgPx) {
                log("[trade] ignoring unmatched/incomplete fill report (ClOrdID=" + info.clOrdId.value_or("?") +
                    ")");
                return;
            }
            const Price fillPrice = Price::fromDouble(*info.avgPx, config_.strategy.symbol);
            std::scoped_lock lock(engineMutex_);
            // Every real (non-close) fill becomes its own tracked open leg
            // -- see openLegs_'s comment for why this can't just be a net
            // aggregate on a hedging-enabled account.
            if (const auto it = placedOrderInfo_.find(*orderId); it != placedOrderInfo_.end()) {
                OpenLeg filled = it->second;
                filled.price = fillPrice;
                openLegs_.push_back(filled);
                placedOrderInfo_.erase(it);
            } else if (const auto cit = cancelledOrderInfo_.find(*orderId); cit != cancelledOrderInfo_.end()) {
                // The cancel lost the race: this order filled anyway after
                // LiveRunner had already asked the broker to cancel it
                // (typically a seed order right at the moment its sibling
                // seed fills first). It's real, unexpected exposure --
                // close it immediately rather than silently letting it
                // ride as an extra open position until the next basket
                // close, which is what looked like "the bot opened 2
                // positions when the range started."
                OpenLeg surplus = cit->second;
                surplus.price = fillPrice;
                log("[order] ClOrdID=" + std::to_string(*orderId) +
                    " filled after its cancel was already sent -- closing this surplus leg immediately");
                cancelledOrderInfo_.erase(cit);
                if (tradeSession_) closeLegs({surplus});
            }
            if (!engine_) return;  // shouldn't happen: fills only follow orders the engine itself placed
            for (const auto& cmd : engine_->onOrderConfirmed(OrderConfirmedEvent{*orderId, fillPrice})) {
                handleCommand(cmd);
            }
            return;
        }

        if (info.execType == ExecType::Rejected) {
            log("[trade] order rejected (ClOrdID=" + info.clOrdId.value_or("?") +
                "): " + info.text.value_or("no reason given"));

            if (info.clOrdId && pendingCloseClOrdIds_.count(*info.clOrdId) > 0) {
                // One of the closing trades was rejected: that leg's
                // position is still open at the broker. Don't wait for a
                // fill that will never come, but do NOT start a new cycle
                // even once every other leg's close confirms -- that would
                // pile a fresh basket on top of unflattened exposure,
                // exactly the bug this mechanism exists to prevent. Needs
                // operator attention (close the leftover position
                // manually, then restart the bot).
                std::scoped_lock lock(engineMutex_);
                pendingCloseClOrdIds_.erase(*info.clOrdId);
                basketCloseFailed_ = true;
                log("[basket] CRITICAL: a closing trade was rejected (ClOrdID=" + *info.clOrdId +
                    ") -- position is still open at the broker; next cycle will NOT start automatically. "
                    "Close it manually, then restart the bot.");
                return;
            }

            const auto orderId = tryParseOrderId(info.clOrdId);
            if (!orderId) return;
            std::scoped_lock lock(engineMutex_);
            // This order is done -- no longer genuinely outstanding either
            // way (it may have already moved to cancelledOrderInfo_ if
            // LiveRunner itself requested the cancel that's now bouncing
            // back as a reject-of-the-order rather than a fill).
            placedOrderInfo_.erase(*orderId);
            cancelledOrderInfo_.erase(*orderId);
            if (!engine_) return;
            for (const auto& cmd :
                 engine_->onOrderRejected(OrderRejectedEvent{*orderId, RejectReason::BrokerRejected})) {
                handleCommand(cmd);
            }
            return;
        }

        if (info.execType == ExecType::Cancelled || info.execType == ExecType::Expired) {
            // Keeps placedOrderInfo_/cancelledOrderInfo_ accurate as
            // "genuinely still outstanding at the broker" -- needed for
            // cancelAllPendingOrders() to only ever act on orders that are
            // actually still live, and for the race-condition check above
            // to only fire for a cancel that's still genuinely in flight.
            if (const auto orderId = tryParseOrderId(info.clOrdId)) {
                std::scoped_lock lock(engineMutex_);
                placedOrderInfo_.erase(*orderId);
                cancelledOrderInfo_.erase(*orderId);
            }
        }

        // New/Cancelled/Expired/OrderStatus: for a normal strategy order,
        // this is informational only -- the engine already updated its own
        // state when it emitted the Place/Cancel command, and a
        // broker-side cancel/expiry confirmation doesn't need to be
        // replayed back into it. But for one of THIS basket's closing
        // orders specifically, log it clearly regardless -- a close order
        // stuck in "New" (acknowledged but never actually filling) with no
        // trace in the log is exactly the kind of silent stall that made
        // this hard to debug live: was the order rejected, still working,
        // or did the broker's FIX dialect simply not treat a plain Market
        // order as a close on this hedging account? The raw OrdStatus/
        // ExecType are logged here (not just engine/core's coarser
        // ExecType enum) so nothing is swallowed.
        if (info.clOrdId && pendingCloseClOrdIds_.count(*info.clOrdId) > 0) {
            log("[basket] closing order status update (ClOrdID=" + *info.clOrdId +
                ", OrdStatus=" + msg.getOptionalString(tag::OrdStatus).value_or("?") +
                ", ExecType=" + msg.getOptionalString(tag::ExecType).value_or("?") +
                ", LeavesQty=" + msg.getOptionalString(tag::LeavesQty).value_or("?") +
                ", CumQty=" + msg.getOptionalString(tag::CumQty).value_or("?") + ")");
        }
        return;
    }

    if (type == msgtype::OrderCancelReject) {
        const OrderCancelRejectInfo info = parseOrderCancelReject(msg);
        log("[trade] cancel request rejected (OrigClOrdID=" + info.origClOrdId.value_or("?") +
            "): " + info.text.value_or("no reason given"));
        // The order is still genuinely live -- move it back so
        // cancelAllPendingOrders() (and the pending-orders list) still
        // sees it, and so a later fill for it is treated as a normal
        // fill rather than the cancel-lost-the-race surplus-leg case.
        if (const auto orderId = tryParseOrderId(info.origClOrdId)) {
            std::scoped_lock lock(engineMutex_);
            if (const auto it = cancelledOrderInfo_.find(*orderId); it != cancelledOrderInfo_.end()) {
                placedOrderInfo_.insert({*orderId, it->second});
                cancelledOrderInfo_.erase(it);
            }
        }
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
        bool haveFullQuote = false;
        {
            std::scoped_lock lock(engineMutex_);
            for (const auto& entry : entries) {
                if (entry.kind == MDEntryKind::Bid) {
                    latestBid_ = entry.price;
                } else {
                    latestAsk_ = entry.price;
                }
            }
            haveFullQuote = latestBid_.has_value() && latestAsk_.has_value();
            maybeEmitTick();
        }
        if (haveFullQuote) {
            std::scoped_lock lock(firstQuoteMutex_);
            if (!firstQuoteReceived_) {
                firstQuoteReceived_ = true;
                firstQuoteCv_.notify_all();
            }
        }
        return;
    }

    if (type == msgtype::MarketDataRequestReject) {
        log("[quote] market data subscription was rejected: " + msg.getOptionalString(tag::Text).value_or("no reason given"));
        return;
    }

    log("[quote] unhandled message type: " + type);
}

TickEvent LiveRunner::latestTick() const {
    return TickEvent{Price::fromDouble(*latestBid_, config_.strategy.symbol),
                     Price::fromDouble(*latestAsk_, config_.strategy.symbol)};
}

void LiveRunner::maybeEmitTick() {
    if (!latestBid_ || !latestAsk_ || !engine_) return;

    for (const auto& cmd : engine_->onTick(latestTick())) handleCommand(cmd);
    maybeRestartAfterBasketClose();
}

bool LiveRunner::accountFlat() const {
    return openLegs_.empty() && placedOrderInfo_.empty() && pendingCloseClOrdIds_.empty();
}

void LiveRunner::prepareNextCycle(bool recenter) {
    if (recenter && latestBid_ && latestAsk_) {
        const SymbolSpec& spec = config_.strategy.symbol;
        RangeConfig& range = config_.strategy.range;
        const int64_t halfWidth = std::max<int64_t>(1, (range.upperBound.ticks - range.lowerBound.ticks) / 2);
        const int64_t mid = Price::fromDouble((*latestBid_ + *latestAsk_) / 2.0, spec).ticks;
        range = RangeConfig{Price{mid - halfWidth}, Price{mid + halfWidth}};
    }
    engine_->reconfigure(config_.strategy);
}

void LiveRunner::maybeRestartAfterBasketClose() {
    // Keep the strategy running continuously: once a basket closes (profit
    // target hit) AND every leg of its real position is confirmed flat at
    // the broker (pendingCloseClOrdIds_ empty -- see the
    // ExecutionReport::Trade handling in onTradeMessage), start the next
    // cycle rather than sitting idle. With seedOffsetPoints>0 this re-seeds
    // around the tick that just triggered the close (see armInitialOrders'
    // comment on lastTick_), so each new cycle re-anchors to live price
    // instead of drifting away from a now-closed cycle's range. Never
    // starts a new cycle while any closing leg is still outstanding, or
    // once basketCloseFailed_ is set -- either would open a fresh range on
    // top of a position that hasn't actually been booked yet.
    if (engine_->status() != EngineStatus::Completed || !accountFlat() || basketCloseFailed_) {
        return;
    }
    if (!config_.connection.autoContinue) {
        return;  // the operator opens the next range by hand (openRange)
    }

    if (config_.connection.basketCooldownSeconds > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!cooldownActive_) {
            // First call to see the basket fully flat -- start the clock
            // from right now, not from whenever it merely decided to close.
            cooldownActive_ = true;
            cooldownUntil_ = now + std::chrono::seconds(config_.connection.basketCooldownSeconds);
            log("[basket] fully flat; next cycle starts after a " +
                std::to_string(config_.connection.basketCooldownSeconds) + "s cooldown");
            return;
        }
        if (now < cooldownUntil_) {
            return;  // still cooling down -- this function gets called again on the next tick
        }
    }

    cooldownActive_ = false;
    prepareNextCycle(config_.connection.autoRangeWidthPoints > 0.0);
    log("basket closed; starting the next cycle");
    for (const auto& cmd : engine_->restart()) handleCommand(cmd);
}

void LiveRunner::waitForFirstQuote() {
    std::unique_lock lock(firstQuoteMutex_);
    const bool settled =
        firstQuoteCv_.wait_for(lock, std::chrono::seconds(15), [this] { return firstQuoteReceived_; });
    if (!settled) {
        throw std::runtime_error("timed out waiting for the first live quote");
    }
}

void LiveRunner::centerRangeOnLatestQuote() {
    const SymbolSpec& spec = config_.strategy.symbol;
    const double half = config_.connection.autoRangeWidthPoints / 2.0;
    const double mid = (*latestBid_ + *latestAsk_) / 2.0;
    const RangeConfig newRange{Price::fromDouble(mid - half, spec), Price::fromDouble(mid + half, spec)};
    config_.strategy.range = newRange;
    log("auto-centered range: [" + std::to_string(newRange.lowerBound.toDouble(spec)) + ", " +
        std::to_string(newRange.upperBound.toDouble(spec)) + "]");
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
        placedOrderInfo_[place->id] = OpenLeg{place->id, place->side, place->volume, place->price};
        tradeSession_->send(buildNewOrderSingle(clOrdId, symbolId_, side, qty, stopPx));
    } else if (const auto* cancel = std::get_if<CancelOrderCommand>(&cmd)) {
        const std::string cancelClOrdId = "c" + std::to_string(nextCancelReqId_++);
        log("[order] cancelling OrigClOrdID=" + std::to_string(cancel->id));
        // Moved, not erased: the cancel can still lose the race against a
        // fill (see cancelledOrderInfo_'s comment) -- onTradeMessage needs
        // this leg's side/volume/price if that happens.
        if (const auto it = placedOrderInfo_.find(cancel->id); it != placedOrderInfo_.end()) {
            cancelledOrderInfo_.insert({cancel->id, it->second});
            placedOrderInfo_.erase(it);
        }
        tradeSession_->send(buildOrderCancelRequest(cancelClOrdId, std::to_string(cancel->id)));
    } else if (const auto* close = std::get_if<CloseBasketCommand>(&cmd)) {
        log("[basket] target reached, net P/L = " + std::to_string(close->netPnlMoney) + ", " +
            std::to_string(openLegs_.size()) + " leg(s) to flatten");
        ++basketsClosed_;
        realizedPnl_ += close->netPnlMoney;
        closeLegs(std::exchange(openLegs_, {}));
    } else if (const auto* alert = std::get_if<RiskAlertCommand>(&cmd)) {
        log("[risk] ALERT: limit breached (kind=" + std::to_string(static_cast<int>(alert->kind)) +
            ") — no further orders will be armed this cycle");
    }
}

void LiveRunner::closeLegs(const std::vector<OpenLeg>& legs) {
    // Closes every leg individually, each with its own opposite-direction
    // Market order sized to exactly that leg's volume -- see openLegs_'s
    // comment on why a single net-sized order isn't enough on a
    // hedging-enabled account (this demo account is one: cTrader kept
    // several same-symbol fills as separate positions even when their net
    // exposure was zero). Shared by the automatic CloseBasketCommand path
    // and the manual closeAllPositions() control. Caller must hold
    // engineMutex_ and must have already moved/cleared its own copy of
    // whatever list it passes in (this only sends orders and tracks their
    // ClOrdIDs -- it never mutates openLegs_ itself).
    if (legs.empty()) {
        log("[basket] already flat -- nothing to close");
        return;
    }
    for (const auto& leg : legs) {
        const int side = (leg.side == OrderSide::BuyStop) ? 2 : 1;  // opposite of the open leg
        const double lots = leg.volume.toDouble(config_.strategy.symbol);
        const double qty = lots * 100.0;  // see the lot-hundredths note in the PlaceOrderCommand branch
        const std::string closeClOrdId = "x" + std::to_string(nextCloseReqId_++);
        log("[basket] flattening leg: side=" + std::to_string(side) + " lots=" + std::to_string(lots) +
            " (ClOrdID=" + closeClOrdId + ")");
        pendingCloseClOrdIds_.insert(closeClOrdId);
        tradeSession_->send(buildMarketOrderSingle(closeClOrdId, symbolId_, side, qty));
    }
}

void LiveRunner::closePositionsLocked() {
    log("[manual] closing all positions (" + std::to_string(openLegs_.size()) + " leg(s))");
    closeLegs(std::exchange(openLegs_, {}));
}

void LiveRunner::cancelPendingLocked() {
    log("[manual] cancelling all pending orders (" + std::to_string(placedOrderInfo_.size()) + " order(s))");
    for (const auto& [orderId, leg] : placedOrderInfo_) {
        const std::string cancelClOrdId = "c" + std::to_string(nextCancelReqId_++);
        log("[manual] cancelling OrigClOrdID=" + std::to_string(orderId));
        cancelledOrderInfo_.insert({orderId, leg});  // see its comment -- a fill can still race the cancel
        tradeSession_->send(buildOrderCancelRequest(cancelClOrdId, std::to_string(orderId)));
    }
    placedOrderInfo_.clear();
}

// For all of the manual "stop the cycle" controls below: the engine's own
// pending-order list and P/L aggregates would otherwise describe orders
// and exposure the operator just removed at the broker. stopCycle() drops
// that state and leaves the engine Idle (onTick/onOrderConfirmed are
// no-ops from Idle), and it stays Idle until openRange() -- this is a
// manual override, not a strategy decision, so trading must not silently
// resume.

bool LiveRunner::openRange() {
    std::scoped_lock lock(engineMutex_);
    if (!engine_ || !tradeSession_) return false;

    if (engine_->status() == EngineStatus::Active) {
        log("[manual] a range is already running -- close it before opening another");
        return false;
    }
    if (basketCloseFailed_) {
        log("[manual] can't open a range: an earlier closing trade was rejected. Close any leftover "
            "position manually, then Disconnect/Connect the bot");
        return false;
    }
    if (!accountFlat()) {
        log("[manual] can't open a range yet: the previous one still has " + std::to_string(openLegs_.size()) +
            " position(s), " + std::to_string(placedOrderInfo_.size()) + " pending order(s) and " +
            std::to_string(pendingCloseClOrdIds_.size()) + " closing order(s) open");
        return false;
    }
    if (!latestBid_ || !latestAsk_) {
        log("[manual] can't open a range: no live quote yet");
        return false;
    }

    if (engine_->status() != EngineStatus::Idle) engine_->stopCycle();  // Completed / HaltedByRisk -> Idle
    cooldownActive_ = false;
    prepareNextCycle(true);
    log("[manual] opening a new range around " + std::to_string((*latestBid_ + *latestAsk_) / 2.0));
    for (const auto& cmd : engine_->start()) handleCommand(cmd);
    return true;
}

void LiveRunner::closeRange() {
    std::scoped_lock lock(engineMutex_);
    if (!engine_ || !tradeSession_) return;
    log("[manual] operator requested: close range");
    cancelPendingLocked();
    closePositionsLocked();
    engine_->stopCycle();
    cooldownActive_ = false;
}

void LiveRunner::closeAllPositions() {
    std::scoped_lock lock(engineMutex_);
    if (!engine_ || !tradeSession_) return;
    closePositionsLocked();
    engine_->stopCycle();
    cooldownActive_ = false;
}

void LiveRunner::cancelAllPendingOrders() {
    std::scoped_lock lock(engineMutex_);
    if (!engine_ || !tradeSession_) return;
    cancelPendingLocked();
    engine_->stopCycle();
    cooldownActive_ = false;
}

LiveSettings LiveRunner::settings() const {
    std::scoped_lock lock(engineMutex_);
    return settingsFromConfig(config_);
}

void LiveRunner::applySettings(const LiveSettings& settings) {
    std::scoped_lock lock(engineMutex_);
    applySettingsToConfig(config_, settings);
    log("[settings] updated -- range width " + std::to_string(settings.rangeWidth) + ", lots " +
        std::to_string(settings.initialLot) + " +" + std::to_string(settings.lotIncrement) + ", target $" +
        std::to_string(settings.profitTarget) + (settings.autoContinue ? ", auto-continue on" : ", auto-continue off") +
        " (applies from the next range)");
}

void LiveRunner::closePosition(OrderId id) {
    std::scoped_lock lock(engineMutex_);
    if (!tradeSession_) return;
    const auto it = std::find_if(openLegs_.begin(), openLegs_.end(), [id](const OpenLeg& l) { return l.id == id; });
    if (it == openLegs_.end()) return;
    log("[manual] operator requested: close position id=" + std::to_string(id));
    const OpenLeg leg = *it;
    openLegs_.erase(it);
    closeLegs({leg});
}

void LiveRunner::cancelOrder(OrderId id) {
    std::scoped_lock lock(engineMutex_);
    if (!tradeSession_) return;
    const auto it = placedOrderInfo_.find(id);
    if (it == placedOrderInfo_.end()) return;
    log("[manual] operator requested: cancel order id=" + std::to_string(id));
    const std::string cancelClOrdId = "c" + std::to_string(nextCancelReqId_++);
    cancelledOrderInfo_.insert({id, it->second});
    placedOrderInfo_.erase(it);
    tradeSession_->send(buildOrderCancelRequest(cancelClOrdId, std::to_string(id)));
}

}  // namespace muuk::ctrader_fix
