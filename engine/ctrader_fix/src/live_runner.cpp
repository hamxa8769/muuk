#include "muuk/ctrader_fix/live_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"

using namespace muuk::core;

namespace muuk::ctrader_fix {

namespace {
// cTrader's FIX OrderQty(38) (and a Position Report's LongQty/ShortQty) is
// in "lot hundredths", not raw lots -- confirmed empirically: OrderQty=2/3
// opened 0.02/0.03-lot positions. engine/core stays in real lots
// throughout; this conversion happens only at the wire boundary.
constexpr double kWireQtyPerLot = 100.0;

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

std::string usdText(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), v < 0.0 ? "-$%.2f" : "$%.2f", std::fabs(v));
    return buf;
}

std::string lotsTextOf(int64_t steps, const muuk::core::SymbolSpec& spec) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(steps) * spec.volumeStep);
    return buf;
}

std::string priceTextOf(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

std::string pctText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f%%", v);
    return buf;
}

std::string accountKey(const LiveConfig& config) {
    return config.connection.environment + "." + config.connection.traderLogin;
}

// Seconds from a localTimestamp() string to now; 0 if it can't be parsed.
double secondsSinceStamp(const std::string& stamp) {
    std::tm tm{};
    std::istringstream in(stamp);
    in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (in.fail()) return 0.0;
    tm.tm_isdst = -1;
    const std::time_t then = std::mktime(&tm);
    if (then == static_cast<std::time_t>(-1)) return 0.0;
    return std::max(0.0, std::difftime(std::time(nullptr), then));
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
    snap.environment = config_.connection.environment;
    snap.brokerUid = config_.connection.brokerUid;
    snap.accountLogin = config_.connection.traderLogin;
    snap.tradeHost = config_.connection.trade.host;

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
    snap.positions.reserve(openLegs_.size());
    for (const auto& leg : openLegs_) {
        PositionInfo info;
        info.id = leg.id;
        info.positionId = leg.positionId;
        info.adopted = leg.adopted;
        info.side = leg.side;
        info.lots = leg.volume.toDouble(spec);
        info.requestedPrice = leg.requestedPrice.toDouble(spec);
        info.entryPrice = leg.price.toDouble(spec);
        info.verified = leg.confirmed;
        if (haveQuote) {
            info.pnl = legPnl(leg);
            info.floatingPnl = info.pnl.net;
            snap.live += info.pnl;
        }
        snap.positions.push_back(info);
    }
    snap.pendingOrders.reserve(placedOrderInfo_.size());
    for (const auto& [orderId, leg] : placedOrderInfo_) {
        PendingOrderInfo info{orderId, leg.side, leg.volume.toDouble(spec), leg.price.toDouble(spec), leg.adopted};
        info.confirmed = leg.confirmed;
        snap.pendingOrders.push_back(info);
    }
    for (const auto& [orderId, leg] : waitingOrders_) {
        PendingOrderInfo info{orderId, leg.side, leg.volume.toDouble(spec), leg.price.toDouble(spec), leg.adopted};
        info.waiting = true;
        snap.pendingOrders.push_back(info);
    }
    std::sort(snap.pendingOrders.begin(), snap.pendingOrders.end(),
              [](const PendingOrderInfo& a, const PendingOrderInfo& b) { return a.id < b.id; });

    snap.closingInProgress = !pendingCloses_.empty();
    snap.closeFailed = basketCloseFailed_;
    snap.tickDecisionUs = tickDecisionUs_.summary();
    snap.fillReactionUs = fillReactionUs_.summary();
    snap.brokerRoundTripMs = brokerRoundTripMs_.summary();
    snap.ticksProcessed = ticksProcessed_;
    snap.sessionStartedAt = sessionStartedAt_;
    snap.statsSince = statsSince_;
    snap.resumeNote = resumeNote_;
    snap.brokerCheckedAt = brokerCheckedAt_;
    {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [at, issue] : brokerIssues_) {
            snap.brokerIssues.push_back(issue);
            if (now - at < std::chrono::minutes(5)) ++snap.brokerIssuesRecent;
        }
    }
    {
        std::scoped_lock infoLock(infoMutex_);
        snap.connectedSince = connectedSince_;
        snap.disconnectReason = disconnectReason_;
    }
    if (clockOffsetKnown_) snap.clockOffsetMs = clockOffsetMs_.load();
    snap.baskets = completedBaskets_;
    snap.currentBasket = currentBasket_;
    snap.syncingPositions = !activePosReqId_.empty();
    if (cooldownActive_) {
        const auto left = std::chrono::ceil<std::chrono::seconds>(cooldownUntil_ - std::chrono::steady_clock::now());
        snap.cooldownRemainingSeconds = static_cast<int>(std::max<std::chrono::seconds::rep>(0, left.count()));
    }
    snap.basketsClosed = basketsClosed_;
    snap.bookedGross = bookedGross_;
    snap.bookedCommission = bookedCommission_;
    snap.realizedPnl = bookedGross_ - bookedCommission_;
    snap.settings = settingsFromConfig(config_);
    snap.market = marketStatus_;
    snap.marketKnown = marketKnown_;
    snap.audit = audit_;
    snap.auditLog.assign(auditLog_.begin(), auditLog_.end());
    snap.dayRealized = dayRealized_;
    snap.dailyLimitHit = dailyLimitHit_;
    snap.locked = locked_;
    snap.lockedLoss = lockedLoss_;
    snap.lockedAt = lockedAt_;
    snap.filledLevel = engine_ ? engine_->filledLevel() : 0;
    if (haveQuote) {
        const BookLots book = bookLotsLocked();
        const double mid = (*latestBid_ + *latestAsk_) / 2.0;
        snap.equityNow = equityEstimateLocked(snap.live.net);
        snap.marginNow = bookMargin(book.buys, book.sells, mid, spec.contractSize, marginRuleLocked());
        if (snap.equityNow >= 0.0 && snap.marginNow > 0.0) {
            snap.marginLevelNow = marginLevelPercent(snap.equityNow, snap.marginNow);
        }
    }
    if (guardLockOrderId_ != 0) {
        const auto it = placedOrderInfo_.find(guardLockOrderId_);
        const auto wt = waitingOrders_.find(guardLockOrderId_);
        const OpenLeg* lk = it != placedOrderInfo_.end() ? &it->second : (wt != waitingOrders_.end() ? &wt->second : nullptr);
        snap.guardLockPending = true;
        if (lk) {
            snap.guardLockPrice = lk->price.toDouble(spec);
            snap.guardLockLots = lk->volume.toDouble(spec);
        }
    }
    snap.escapeOrdersOut = escape_.buyStopId != 0 || escape_.sellStopId != 0;
    snap.escapeTradeOpen = escape_.legId != 0;
    snap.escapeTrailing = escape_.trailing;
    snap.escapeTries = escapeTries_;
    snap.escapePhase = escapePhase_;
    snap.escapeAwaitingDecision = escapeAwaitingDecision_;
    snap.escapeHeld = escapeHeld_;
    snap.marketRead = marketRead_.why;
    snap.marketScore = marketRead_.score;
    snap.platform = config_.connection.platform;
    snap.brokerAccount = brokerAccount_;
    snap.brokerAccountFresh = brokerAccountFreshLocked();
    snap.moneyPerPointPerLot = spec.tickValue / spec.tickSize;
    if (snap.brokerAccountFresh) snap.marginLeverage = marginRuleLocked().leverage;
    if (snap.brokerAccountFresh) {  // MT5's own figures
        snap.equityNow = brokerAccount_.equity;
        snap.marginNow = brokerAccount_.margin;
        snap.marginLevelNow = brokerAccount_.margin > 0.0 ? marginLevelPercent(brokerAccount_.equity, brokerAccount_.margin)
                                                          : (openLegs_.empty() ? -1.0 : 1e9);
    }
    snap.escapeActive = basketRunningLocked() && engine_->escapeActive();
    // Worked out from the positions as they stand (the same formula the
    // engine applies on every quote) -- also right after a resume, before
    // the first new quote (e.g. during the daily break).
    if (engine_) {
        // The engine's own rule (fixed, auto, last lot, break-even), on the positions as they stand.
        muuk::core::BasketPnlResult now;
        now.commissionMoney = snap.live.commission;
        now.spreadCostMoney = snap.live.spread;
        snap.targetNow = engine_->effectiveTargetMoney(now);
    } else {
        snap.targetNow = config_.strategy.autoProfitTarget
                             ? muuk::core::autoTargetMoney(snap.live.commission + 2.0 * snap.live.spread,
                                                           config_.strategy.autoTargetPercent)
                             : config_.strategy.basketProfitTargetMoney;
    }
    return snap;
}

PnlBreakdown LiveRunner::legPnl(const OpenLeg& leg) const {
    const SymbolSpec& spec = config_.strategy.symbol;
    const Price bid = Price::fromDouble(*latestBid_, spec);
    const Price ask = Price::fromDouble(*latestAsk_, spec);
    const double mid = (static_cast<double>(bid.ticks) + static_cast<double>(ask.ticks)) / 2.0;
    const double lotsHeld = leg.volume.toDouble(spec);
    const double moneyPerTick = spec.tickValue * lotsHeld;
    const bool buy = leg.side == OrderSide::BuyStop;

    PnlBreakdown p;
    p.gross = (buy ? mid - static_cast<double>(leg.requestedPrice.ticks)
                   : static_cast<double>(leg.requestedPrice.ticks) - mid) *
              moneyPerTick;
    p.slippage = static_cast<double>(buy ? leg.price.ticks - leg.requestedPrice.ticks
                                         : leg.requestedPrice.ticks - leg.price.ticks) *
                 moneyPerTick;
    p.spread = (buy ? mid - static_cast<double>(bid.ticks) : static_cast<double>(ask.ticks) - mid) * moneyPerTick;
    p.commission = muuk::core::roundTurnCommissionMoney(lotsHeld, leg.price.toDouble(spec), spec,
                                                        config_.strategy.costs);
    if (leg.brokerEntryCommission >= 0.0) {
        // MT5: what the opening really cost, plus the closing at this account's own rate.
        const double outRate = brokerAccount_.commissionOutPerLot >= 0.0  ? brokerAccount_.commissionOutPerLot
                               : brokerAccount_.commissionInPerLot >= 0.0 ? brokerAccount_.commissionInPerLot
                                                                          : -1.0;
        p.commission = leg.brokerEntryCommission + (outRate >= 0.0 ? outRate * lotsHeld : leg.brokerEntryCommission);
    }
    // Swap is only known on MT5 (the bridge reports it per position); a cost here, so charged swap is positive.
    if (const auto sw = positionSwap_.find(leg.positionId); sw != positionSwap_.end()) p.swap = -sw->second;
    p.net = p.gross - p.slippage - p.spread - p.commission - p.swap;
    return p;
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
    {
        std::scoped_lock lock(engineMutex_);
        sessionStartedAt_ = localTimestamp();
    }

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
                                                       [this](const FixMessage& m) {
                                                           try {
                                                               onQuoteMessage(m);
                                                           } catch (const std::exception& ex) {
                                                               log("[quote] couldn't handle a " + m.msgType() +
                                                                   " message: " + ex.what());
                                                           }
                                                       });
        tradeSession_ = std::make_unique<FixSession>(config_.connection.trade.host, config_.connection.trade.port,
                                                       tradeCreds,
                                                       [this](const FixMessage& m) {
                                                           // One malformed message must never take the bot down.
                                                           try {
                                                               onTradeMessage(m);
                                                           } catch (const std::exception& ex) {
                                                               log("[trade] couldn't handle a " + m.msgType() +
                                                                   " message: " + ex.what());
                                                           }
                                                       });

        quoteSession_->setDisconnectHandler([this](const std::string& reason) {
            log("[quote session] ended: " + reason);
            {
                std::scoped_lock infoLock(infoMutex_);
                if (disconnectReason_.empty()) disconnectReason_ = "quote session: " + reason;
            }
            running_ = false;
        });
        tradeSession_->setDisconnectHandler([this](const std::string& reason) {
            log("[trade session] ended: " + reason);
            {
                std::scoped_lock infoLock(infoMutex_);
                if (disconnectReason_.empty()) disconnectReason_ = "trade session: " + reason;
            }
            running_ = false;
        });

        tradeSession_->start();
        quoteSession_->start();
        log("logged on to both Quote and Trade sessions");

        if (config_.connection.isMt5()) {
            // The bridge sends the account right after logon; the symbol's
            // money units come from it, so wait for it (briefly) first.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::scoped_lock lock(engineMutex_);
                    if (brokerAccount_.known) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            BrokerAccountInfo a;
            {
                std::scoped_lock lock(engineMutex_);
                a = brokerAccount_;
            }
            // Never trade an account the config doesn't expect: nothing has been sent yet.
            if (!a.known) {
                throw std::runtime_error("MT5 bridge: no account figures from MetaTrader 5 yet -- is MuukBridge running "
                                         "on the chart? Not trading.");
            }
            if (a.real() && config_.connection.environment != "live") {
                throw std::runtime_error("MT5 account " + a.login + " (" + a.server +
                                         ") is a REAL money account, but this config says demo -- not trading. Log "
                                         "MT5 into the demo account, or set the account type to LIVE if you mean it.");
            }
            if (!a.real() && config_.connection.environment == "live" && !a.tradeMode.empty()) {
                log("[mt5] note: the config says LIVE but MT5 account " + a.login + " is " + a.tradeMode);
            }
        }
        resolveSymbol();
        log("resolved symbol '" + config_.connection.symbolName + "' -> id " + symbolId_);

        // Subscribed before the engine is constructed: if auto-centering
        // is enabled, a real tick needs to arrive first so there's a live
        // price to center on. If it isn't, this ordering is harmless —
        // onQuoteMessage/maybeEmitTick are no-ops until engine_ exists.
        quoteSession_->send(buildMarketDataRequest("md-" + config_.connection.symbolName, symbolId_, true));
        log("subscribed to quotes");

        loadSavedState();

        if (config_.connection.syncPositionsOnConnect) {
            // Ask the broker what is really open -- positions and working
            // orders -- before deciding anything: the saved basket is
            // checked against it (reconcileOnConnectLocked).
            {
                std::scoped_lock lock(posSyncMutex_);
                posSyncDone_ = false;
                orderSyncDone_ = false;
            }
            {
                std::scoped_lock lock(engineMutex_);
                connectSync_ = true;
            }
            syncPositions();
            {
                std::scoped_lock lock(engineMutex_);
                requestOrderStatusLocked();
            }
            std::unique_lock lock(posSyncMutex_);
            const bool gotPositions = posSyncCv_.wait_for(lock, std::chrono::seconds(8), [this] { return posSyncDone_; });
            const bool gotOrders = posSyncCv_.wait_for(lock, std::chrono::seconds(5), [this] { return orderSyncDone_; });
            lock.unlock();
            std::scoped_lock engineLock(engineMutex_);
            if (!gotPositions) {
                activePosReqId_.clear();
                log("[sync] the broker didn't answer the position request -- continuing without it");
            }
            if (!gotOrders) {
                activeMassStatusReqId_.clear();
                log("[sync] the broker didn't answer the order status request -- working orders can't be checked");
            }
            connectSync_ = false;
        }

        bool resumeCandidate = false;
        bool positionsAtBroker = false;
        {
            std::scoped_lock lock(engineMutex_);
            resumeCandidate = loadedState_ && loadedState_->engine.status != EngineStatus::Idle;
            positionsAtBroker = !connectPositions_.empty();
        }
        const bool autoRange = config_.connection.autoRangeWidthPoints > 0.0;
        if (autoRange || config_.strategy.seedOffsetPoints > 0.0 || resumeCandidate) {
            waitForFirstQuote();
        } else if (positionsAtBroker) {
            // Picking up a basket from the broker needs to know where the
            // market is (is its next order already passed?) -- but don't
            // fail the connection over it.
            std::unique_lock lock(firstQuoteMutex_);
            if (!firstQuoteCv_.wait_for(lock, std::chrono::seconds(10), [this] { return firstQuoteReceived_; })) {
                log("[sync] no live quote yet -- continuing without one");
            }
        }

        bool started = false;
        updateMarket();
        {
            std::scoped_lock lock(engineMutex_);
            if (autoRange) centerRangeOnLatestQuote();
            engine_ = std::make_unique<StrategyEngine>(config_.strategy);
            // Hand the engine the live quote before start(): it's Idle, so
            // this only records the tick -- but seed orders are placed off
            // that tick's bid/ask, and without it they'd fall back to the
            // range midpoint and could land inside the spread.
            if (latestBid_ && latestAsk_) (void)engine_->onTick(latestTick());
            reconcileOnConnectLocked();
            stateReady_ = true;
            {
                std::scoped_lock infoLock(infoMutex_);
                connectedSince_ = localTimestamp();
            }
            markDirty();
            if (engine_->status() != EngineStatus::Idle) {
                started = true;  // the saved basket carries on
            } else if (!accountFlat()) {
                log("[sync] " + std::to_string(openLegs_.size()) + " position(s) and " +
                    std::to_string(placedOrderInfo_.size()) +
                    " pending order(s) already at the broker -- no range will open until they're closed "
                    "(Close all positions / Cancel all pending)");
            } else if (holdAutoStart_) {
                log("[resume] not opening a range by itself after an interrupted basket -- press Open range");
            } else if (config_.connection.autoContinue) {
                if (spreadTooWideLocked() || marketBlocksLocked()) {
                    reopenAfterManualClose_ = true;  // starts as soon as the spread / market allows
                    reopenCooldownSeconds_ = 0;
                } else {
                    beginBasket();
                    for (const auto& cmd : engine_->start()) handleCommand(cmd);
                    started = true;
                }
            }
        }
        log(started ? "engine is now active" : "connected -- waiting for Open range");

        flushState();
        int loops = 0;
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (++loops % 50 == 0) markDirty();  // lifetime statistics, every ~10 s
            if (loops % 300 == 0) {  // a heartbeat in the log every minute
                std::scoped_lock lock(engineMutex_);
                if (engine_) {
                    PnlBreakdown live;
                    if (latestBid_ && latestAsk_) {
                        for (const auto& leg : openLegs_) live += legPnl(leg);
                    }
                    const auto rtt = brokerRoundTripMs_.summary();
                    std::ostringstream oss;
                    oss.setf(std::ios::fixed);
                    oss.precision(2);
                    const EngineStatus st = engine_->status();
                    oss << "[status] " << (st == EngineStatus::Active ? "range active" : st == EngineStatus::Completed ? "basket booked" : st == EngineStatus::HaltedByRisk ? "halted by risk" : "idle")
                        << " | " << openLegs_.size() << " position(s), " << (placedOrderInfo_.size() + waitingOrders_.size())
                        << " pending | net $" << live.net << " | " << (ticksProcessed_ - statusTicks_) << " quotes/min";
                    if (rtt.samples > 0) oss << " | round trip " << rtt.avg << " ms avg";
                    statusTicks_ = ticksProcessed_;
                    log(oss.str());
                }
            }
            if (config_.connection.verifySeconds > 0 && loops % (config_.connection.verifySeconds * 5) == 0) {
                // Periodic broker check: positions and working orders.
                bool idle = false;
                {
                    std::scoped_lock lock(engineMutex_);
                    idle = engine_ && activePosReqId_.empty() && activeMassStatusReqId_.empty() && awaitingAck_.empty();
                }
                if (idle) {
                    {
                        std::scoped_lock lock(engineMutex_);
                        manualPositionSync_ = false;
                    }
                    syncPositions();
                    std::scoped_lock lock(engineMutex_);
                    orderStatusRequestedAt_ = std::chrono::steady_clock::now();
                    requestOrderStatusLocked();
                }
            }
            if (loops % 5 == 0) updateMarket();
            if (loops % 5 == 0) {
                {
                    std::scoped_lock lock(engineMutex_);
                    auditLocked();
                }
                if (auditSyncWanted_.exchange(false)) syncPositions();
            }
            if (loops % 5 == 0) {
                std::scoped_lock lock(engineMutex_);
                checkBasketHealthLocked();
                // Cooldowns also end on quiet markets, not only on the next quote.
                if (engine_) maybeRestartAfterBasketClose();
            }
            flushState();
        }
        markDirty();
        flushState();
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
    // Anything from the trade session can change what's open: save it
    // (after every lock below is released -- this is destroyed last).
    struct SaveOnExit {
        LiveRunner* runner;
        ~SaveOnExit() {
            runner->markDirty();
            runner->flushState();
        }
    } saveOnExit{this};

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

        // An answer to the connect-time Order Mass Status Request: one
        // report per working order, collected until all have arrived.
        if (info.massStatusReqId || info.execType == ExecType::OrderStatus) {
            std::scoped_lock lock(engineMutex_);
            if (!activeMassStatusReqId_.empty() &&
                info.massStatusReqId.value_or(activeMassStatusReqId_) == activeMassStatusReqId_) {
                orderReports_.push_back(info);
                if (static_cast<int>(orderReports_.size()) >= std::max(1, info.totNumReports)) {
                    finishOrderSync(std::exchange(orderReports_, {}), true);
                }
            }
            return;
        }

        // Broker round trip: order sent -> the broker's first answer about it.
        if (info.clOrdId) {
            std::scoped_lock lock(engineMutex_);
            if (const auto sent = orderSentAt_.find(*info.clOrdId); sent != orderSentAt_.end()) {
                brokerRoundTripMs_.add(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sent->second).count());
                orderSentAt_.erase(sent);
            }
        }

        // The broker accepted one of the bot's orders: it's really working there.
        if (info.execType == ExecType::New) {
            if (const auto oid = tryParseOrderId(info.clOrdId)) {
                std::scoped_lock lock(engineMutex_);
                if (const auto it = placedOrderInfo_.find(*oid); it != placedOrderInfo_.end()) it->second.confirmed = true;
            }
        }

        if (info.execType == ExecType::Trade && info.commission) {
            // Logged so the configured commission rate can be checked
            // against what the broker actually charges.
            log("[trade] broker-reported commission on ClOrdID=" + info.clOrdId.value_or("?") + ": " +
                std::to_string(*info.commission) + " (CommType=" + info.commType.value_or("?") + ")");
        }

        if (info.execType == ExecType::Trade) {
            // The basket-close market orders aren't tracked by the engine
            // (they have no corresponding PendingOrder) -- they're matched
            // by ClOrdID here instead, and only once *every* leg's closing
            // order has filled is the next cycle allowed to start. See
            // maybeRestartAfterBasketClose().
            if (info.clOrdId) {
                std::scoped_lock lock(engineMutex_);
                if (const auto cit = pendingCloses_.find(*info.clOrdId); cit != pendingCloses_.end()) {
                    const OpenLeg leg = cit->second;
                    pendingCloses_.erase(cit);
                    closeSentAt_.erase(*info.clOrdId);
                    closeRecheck_.erase(*info.clOrdId);
                    closeAttempts_.erase(leg.positionId);
                    closedPositionIds_.insert(leg.positionId);

                    // Book the real result: actual entry fill -> actual exit fill.
                    const SymbolSpec& spec = config_.strategy.symbol;
                    const double lotsClosed = leg.volume.toDouble(spec);
                    std::string booked;
                    if (info.avgPx) {
                        const Price exitPrice = Price::fromDouble(*info.avgPx, spec);
                        const int64_t moveTicks = (leg.side == OrderSide::BuyStop) ? exitPrice.ticks - leg.price.ticks
                                                                                    : leg.price.ticks - exitPrice.ticks;
                        const double gross = static_cast<double>(moveTicks) * spec.tickValue * lotsClosed;
                        // MT5 reports what each deal really cost: book exactly that
                        // (opening + closing); otherwise the configured rate.
                        const bool actual = config_.connection.isMt5() && info.commission && leg.brokerEntryCommission >= 0.0;
                        const double commission =
                            actual ? leg.brokerEntryCommission + std::fabs(*info.commission)
                                   : muuk::core::roundTurnCommissionMoney(lotsClosed, leg.price.toDouble(spec), spec,
                                                                           config_.strategy.costs);
                        bookedGross_ += gross;
                        bookedCommission_ += commission;
                        addRealizedLocked(gross - commission);
                        booked = ", booked " + std::to_string(gross - commission) + " net (gross " +
                                 std::to_string(gross) + ", commission " + std::to_string(commission) + ")";
                        recordLegClosed(leg, exitPrice.toDouble(spec), gross, commission);
                    } else {
                        recordLegClosed(leg, 0.0, 0.0, 0.0);
                    }
                    log("[basket] closing trade filled (ClOrdID=" + *info.clOrdId + ")" + booked + ", " +
                        std::to_string(pendingCloses_.size()) + " leg(s) still outstanding");
                    maybeFinalizeBasket();
                    if (pendingCloses_.empty() && engine_) {
                        log("[basket] all legs flat, next cycle may now start");
                        maybeRestartAfterBasketClose();
                    }
                    return;
                }
            }

            const auto orderId = tryParseOrderId(info.clOrdId);
            if (!orderId || !info.avgPx) {
                log("[trade] ignoring unmatched/incomplete fill report (ClOrdID=" + info.clOrdId.value_or("?") +
                    ")");
                return;
            }
            const Price fillPrice = Price::fromDouble(*info.avgPx, config_.strategy.symbol);
            std::scoped_lock lock(engineMutex_);
            applyFillLocked(*orderId, fillPrice, info.positionId.value_or(""), info.commission);
            return;
        }

        if (info.execType == ExecType::Rejected) {
            log("[trade] order rejected (ClOrdID=" + info.clOrdId.value_or("?") +
                "): " + info.text.value_or("no reason given"));

            if (info.clOrdId && pendingCloses_.count(*info.clOrdId) > 0) {
                // One of the closing trades was rejected. Either the position
                // is already gone (closed in cTrader, stop-out...) or it's
                // still open. The audit settles it at the next position check
                // (asked for right away): gone -> booked as closed at the
                // broker; still open -> closed again, up to 3 tries, and only
                // then does it stop and wait for the operator. No new range
                // opens while this close is outstanding.
                std::scoped_lock lock(engineMutex_);
                closeRecheck_.insert(*info.clOrdId);
                auditSyncWanted_ = true;
                log("[audit] closing trade " + *info.clOrdId + " was rejected -- checking the position with the broker");
                return;
            }

            const auto orderId = tryParseOrderId(info.clOrdId);
            if (!orderId) return;
            std::scoped_lock lock(engineMutex_);
            if (const auto mm = managedMarket_.find(*orderId); mm != managedMarket_.end()) {
                // A lock / re-lock refused: send it again shortly.
                placedOrderInfo_.erase(*orderId);
                ManagedMarket retry = mm->second;
                managedMarket_.erase(mm);
                if (retry.attempts >= 5) {
                    auditEventLocked(8, 2,
                                     "the " + retry.what + " order was refused 5 times (" +
                                         info.text.value_or("no reason given") + ") -- open it by hand in cTrader: " +
                                         std::string(retry.leg.side == OrderSide::BuyStop ? "BUY " : "SELL ") +
                                         lotsTextOf(retry.leg.volume.steps, config_.strategy.symbol) + " lot");
                    return;
                }
                retry.retryDue = true;
                retry.retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                managedMarket_[*orderId] = retry;
                log("[risk] the " + retry.what + " order was refused (try " + std::to_string(retry.attempts) +
                    " of 5) -- sending it again in 0.5 s");
                return;
            }
            if (*orderId == guardLockOrderId_ || *orderId == escape_.buyStopId || *orderId == escape_.sellStopId) {
                placedOrderInfo_.erase(*orderId);
                if (*orderId == guardLockOrderId_) {
                    guardLockOrderId_ = 0;
                    lockNowLocked("the lock stop order was refused (" + info.text.value_or("no reason given") + ")",
                                  latestBid_ && latestAsk_ ? floatingNetLocked() : 0.0);
                } else {
                    (*orderId == escape_.buyStopId ? escape_.buyStopId : escape_.sellStopId) = 0;
                    auditEventLocked(8, 1, "an escape order was refused (" + info.text.value_or("no reason given") + ")");
                }
                return;
            }
            // This order is done -- no longer genuinely outstanding either
            // way (it may have already moved to cancelledOrderInfo_ if
            // LiveRunner itself requested the cancel that's now bouncing
            // back as a reject-of-the-order rather than a fill).
            placedOrderInfo_.erase(*orderId);
            cancelledOrderInfo_.erase(*orderId);
            if (currentBasket_) ++currentBasket_->ordersRejected;
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
                const bool weCancelledIt = cancelledOrderInfo_.erase(*orderId) > 0;
                if (const auto it = placedOrderInfo_.find(*orderId); it != placedOrderInfo_.end()) {
                    const OpenLeg order = it->second;
                    placedOrderInfo_.erase(it);
                    // Not a cancel the bot sent: the broker cancelled/expired
                    // it, or it was cancelled by hand in cTrader. The running
                    // basket still needs it.
                    if (!weCancelledIt && !order.adopted && engine_ && engine_->status() == EngineStatus::Active) {
                        noteMissedLocked();
                        log("[order] MISSED: pending " + std::string(order.side == OrderSide::BuyStop ? "Buy" : "Sell") +
                            " Stop " + std::to_string(order.volume.toDouble(config_.strategy.symbol)) + " lot @ " +
                            std::to_string(order.price.toDouble(config_.strategy.symbol)) + " (ClOrdID=" +
                            std::to_string(*orderId) + ") was " +
                            (info.execType == ExecType::Expired ? "expired" : "cancelled") +
                            " outside the bot -- placing it again");
                        for (const auto& cmd : engine_->replaceOrder(*orderId)) handleCommand(cmd);
                    }
                }
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
        if (info.clOrdId && pendingCloses_.count(*info.clOrdId) > 0) {
            log("[basket] closing order status update (ClOrdID=" + *info.clOrdId +
                ", OrdStatus=" + msg.getOptionalString(tag::OrdStatus).value_or("?") +
                ", ExecType=" + msg.getOptionalString(tag::ExecType).value_or("?") +
                ", LeavesQty=" + msg.getOptionalString(tag::LeavesQty).value_or("?") +
                ", CumQty=" + msg.getOptionalString(tag::CumQty).value_or("?") + ")");
        }
        return;
    }

    if (type == msgtype::MuukAccountInfo) {
        std::scoped_lock lock(engineMutex_);
        onAccountInfoLocked(msg);
        return;
    }

    if (type == msgtype::PositionReport) {
        const PositionReportInfo info = parsePositionReport(msg);
        std::scoped_lock lock(engineMutex_);
        if (activePosReqId_.empty() || info.posReqId != activePosReqId_) return;  // stale or unsolicited
        if (!info.anyPositions) {
            finishPositionSync({});
            return;
        }
        posReports_.push_back(info);
        if (static_cast<int>(posReports_.size()) >= std::max(1, info.totalReports)) {
            const std::vector<PositionReportInfo> reports = std::exchange(posReports_, {});
            finishPositionSync(reports);
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
        // cTrader answers an Order Mass Status Request this way when there
        // are no working orders at all. The live IC Markets server sends it
        // without RefMsgType, just "ORDER_NOT_FOUND:no orders found" as the
        // text, so any of the three marks it.
        {
            const std::string text = msg.getOptionalString(tag::Text).value_or("");
            std::scoped_lock lock(engineMutex_);
            if (!activeMassStatusReqId_.empty() &&
                (msg.getOptionalString(tag::RefMsgType).value_or("") == msgtype::OrderMassStatusRequest ||
                 msg.getOptionalString(tag::BusinessRejectRefID).value_or("") == activeMassStatusReqId_ ||
                 text.rfind("ORDER_NOT_FOUND", 0) == 0)) {
                log("[sync] the broker has no working orders");
                orderReports_.clear();
                finishOrderSync({}, true);
                return;
            }
        }
        // A close for a position the broker no longer has ("INCORRECT_POSITION_ID:
        // Unknown position. PositionId=..."): settle it at the next position check.
        {
            const std::string text = msg.getOptionalString(tag::Text).value_or("");
            const auto at = text.find("PositionId=");
            if (at != std::string::npos) {
                std::string pid = text.substr(at + 11);
                pid = pid.substr(0, pid.find_first_not_of("0123456789"));
                std::scoped_lock lock(engineMutex_);
                for (const auto& [clOrdId, leg] : pendingCloses_) {
                    if (leg.positionId == pid) {
                        closeRecheck_.insert(clOrdId);
                        auditSyncWanted_ = true;
                        log("[audit] the broker says position " + pid + " isn't open (" + text +
                            ") -- checking with the broker");
                    }
                }
            }
        }
        std::ostringstream oss;
        oss << "[trade] BusinessMessageReject: refMsgType=" << msg.getOptionalString(tag::RefMsgType).value_or("?")
            << " reason=" << msg.getOptionalString(tag::BusinessRejectReason).value_or("?")
            << " text=" << msg.getOptionalString(tag::Text).value_or("no reason given");
        log(oss.str());
        return;
    }

    log("[trade] unhandled message type: " + type);
}

void LiveRunner::applyFillLocked(OrderId orderId, Price fillPrice, const std::string& positionId,
                                 std::optional<double> brokerCommission) {
    markDirty();
    // Every real (non-close) fill becomes its own tracked open leg
    // -- see openLegs_'s comment for why this can't just be a net
    // aggregate on a hedging-enabled account.
    if (const auto it = placedOrderInfo_.find(orderId); it != placedOrderInfo_.end()) {
        OpenLeg filled = it->second;
        filled.price = fillPrice;
        filled.positionId = positionId;
        if (filled.positionId.empty()) {
            log("[trade] WARNING: fill for ClOrdID=" + std::to_string(orderId) +
                " carried no position id (tag 721) -- the bot can't close this position itself; "
                "close it in cTrader if needed");
        }
        filled.confirmed = true;  // a fill is the broker's own report
        if (brokerCommission && config_.connection.isMt5()) filled.brokerEntryCommission = std::fabs(*brokerCommission);
        openLegs_.push_back(filled);
        placedOrderInfo_.erase(it);
        recordLegOpened(filled);
        if (currentBasket_) ++currentBasket_->ordersFilled;
        healthRearms_ = 0;
        {
            const SymbolSpec& spec = config_.strategy.symbol;
            const bool buy = filled.side == OrderSide::BuyStop;
            const int64_t slipTicks = buy ? filled.price.ticks - filled.requestedPrice.ticks
                                          : filled.requestedPrice.ticks - filled.price.ticks;
            const double lots = filled.volume.toDouble(spec);
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss.precision(2);
            oss << "[fill] " << (buy ? "BUY " : "SELL ") << lots << " lot @ " << filled.price.toDouble(spec)
                << " (order price " << filled.requestedPrice.toDouble(spec) << "), slippage "
                << (slipTicks > 0 ? "+" : "") << static_cast<double>(slipTicks) * spec.tickSize << " = "
                << (slipTicks > 0 ? "-$" : (slipTicks < 0 ? "+$" : "$"))
                << std::abs(static_cast<double>(slipTicks) * spec.tickValue * lots)
                << (slipTicks > 0 ? " (worse)" : (slipTicks < 0 ? " (better)" : ""));
            log(oss.str());
        }
    } else if (const auto cit = cancelledOrderInfo_.find(orderId); cit != cancelledOrderInfo_.end()) {
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
        surplus.positionId = positionId;
        log("[order] ClOrdID=" + std::to_string(orderId) +
            " filled after its cancel was already sent -- closing this surplus leg immediately");
        cancelledOrderInfo_.erase(cit);
        recordLegOpened(surplus);
        if (tradeSession_) closeLegs({surplus});
    }
    onRecoveryFillLocked(orderId);
    if (!engine_) return;  // shouldn't happen: fills only follow orders the engine itself placed
    const auto reactStart = std::chrono::steady_clock::now();
    for (const auto& cmd : engine_->onOrderConfirmed(OrderConfirmedEvent{orderId, fillPrice})) {
        handleCommand(cmd);
    }
    fillReactionUs_.add(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - reactStart).count());
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
            lastQuoteAt_ = std::chrono::steady_clock::now();
            if (haveFullQuote) bars_.addQuote(utcNowMs(), (*latestBid_ + *latestAsk_) / 2.0);
            maybeEmitTick();
            // Clock check, now and then: how far this PC's clock is from the
            // broker's (a VPS with a drifting clock shows up here).
            if (quoteMessages_++ % 20 == 0) {
                if (const auto sent = msg.getOptionalString(tag::SendingTime)) {
                    if (const auto brokerMs = parseUtcTimestampMs(*sent)) {
                        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count();
                        clockOffsetMs_ = static_cast<double>(nowMs - *brokerMs);
                        clockOffsetKnown_ = true;
                    }
                }
            }
        }
        if (stateDirty_) flushState();  // only when the tick changed something (e.g. closed the basket)
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

    const TickEvent tick = latestTick();
    const auto decideStart = std::chrono::steady_clock::now();
    const CommandBatch commands = engine_->onTick(tick);
    tickDecisionUs_.add(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - decideStart).count());
    ++ticksProcessed_;
    // Deepest drawdown / best point of the running basket.
    if (currentBasket_ && basketRunningLocked() && !openLegs_.empty()) {
        const double net = engine_->lastBasketPnl().netPnlMoney;
        currentBasket_->worstNet = std::min(currentBasket_->worstNet, net);
        currentBasket_->bestNet = std::max(currentBasket_->bestNet, net);
    }
    for (const auto& cmd : commands) handleCommand(cmd);
    maybeLockLocked();
    maybeGuardLocked();
    retryManagedMarketLocked();
    releaseWaitingOrdersLocked();
    maybeRestartAfterBasketClose();
    maybeRefreshStaleEntryLocked();
}

bool LiveRunner::basketRunningLocked() const {
    if (!engine_) return false;
    const EngineStatus st = engine_->status();
    return st == EngineStatus::Active || (st == EngineStatus::HaltedByRisk && !openLegs_.empty());
}

double LiveRunner::lockAtLossMoneyLocked() const {
    double at = config_.connection.lockAtLossMoney > 0.0 ? config_.connection.lockAtLossMoney : 0.0;
    if (config_.connection.lockAtLossPercent > 0.0) {
        const double balance = brokerAccount_.known && brokerAccount_.balance > 0.0 ? brokerAccount_.balance
                                                                                    : config_.connection.accountBalance;
        if (balance > 0.0) {
            const double share = balance * config_.connection.lockAtLossPercent / 100.0;
            at = at > 0.0 ? std::min(at, share) : share;  // whichever is reached first
        }
    }
    return at;
}

void LiveRunner::maybeLockLocked() {
    const double lockAt = lockAtLossMoneyLocked();
    if (lockAt <= 0.0 || locked_ || !engine_ || !tradeSession_ || !latestBid_ || !latestAsk_) return;
    if (openLegs_.empty() || !pendingCloses_.empty()) return;
    if (!basketRunningLocked() && guardLockOrderId_ == 0) return;
    const double net = floatingNetLocked();
    if (net > -lockAt) return;
    lockNowLocked("the basket reached " + usdText(net) + " (lock at -" + usdText(lockAt).substr(1) + ")", net);
}

void LiveRunner::lockNowLocked(const std::string& why, double net) {
    const SymbolSpec& spec = config_.strategy.symbol;
    const BookLots book = bookLotsLocked();
    if (!locked_) {
        locked_ = true;
        lockedLoss_ = net;
        lockedAt_ = localTimestamp();
        lockedSince_ = std::chrono::steady_clock::now();
        if (currentBasket_) {
            currentBasket_->locked = true;
            currentBasket_->lockedLoss = net;
        }
        setCloseReason("locked at " + usdText(net));
    }
    markDirty();
    cancelPendingLocked();  // also a lock stop / escape orders still out
    guardLockOrderId_ = 0;
    escape_.buyStopId = escape_.sellStopId = 0;
    escape_.legId = 0;
    escape_.trailing = false;
    if (basketRunningLocked()) {
        const RangeConfig range = engine_->currentRange();  // the escape orders go outside it
        guardRangeLow_ = range.lowerBound;
        guardRangeHigh_ = range.upperBound;
    }
    if (engine_->status() != EngineStatus::Idle) engine_->stopCycle();
    cooldownActive_ = false;
    reopenAfterManualClose_ = false;
    std::string hedgeText = "already balanced -- nothing to add";
    if (book.netSteps != 0) {
        // One opposite market order for the net lots: a new position on a
        // hedging account (no position id), so buys = sells and the P/L no
        // longer moves with the price.
        OpenLeg hedge;
        hedge.id = nextAdoptedId_++;
        hedge.side = book.netSteps > 0 ? OrderSide::SellStop : OrderSide::BuyStop;
        hedge.volume = Volume{book.netSteps > 0 ? book.netSteps : -book.netSteps};
        hedge.price = hedge.requestedPrice =
            Price::fromDouble(hedge.side == OrderSide::BuyStop ? *latestAsk_ : *latestBid_, spec);
        hedgeText = std::string(hedge.side == OrderSide::BuyStop ? "BUY " : "SELL ") +
                    lotsTextOf(hedge.volume.steps, spec) + " lot at market (ClOrdID=" + std::to_string(hedge.id) + ")";
        sendManagedMarketLocked(hedge, "lock");
    }
    auditEventLocked(8, 2,
                     why + ": pending orders cancelled, hedge " + hedgeText +
                         " -- the loss is frozen" +
                         (config_.connection.recovery.escapeEnabled
                              ? "; the bot reads the market for " +
                                    std::to_string(config_.connection.recovery.escapeAnalyzeMinutes) +
                                    " min, then escapes only with a clear trend"
                              : std::string("; the bot waits for you (Close range to unwind)")));
}

// ---------------------------------------------------------------------------
// Margin guards and recovery (RecoveryConfig). All: caller holds engineMutex_.

LiveRunner::BookLots LiveRunner::bookLotsLocked() const {
    const SymbolSpec& spec = config_.strategy.symbol;
    BookLots b;
    for (const auto& leg : openLegs_) {
        const double lots = leg.volume.toDouble(spec);
        if (leg.side == OrderSide::BuyStop) {
            b.buys += lots;
            b.netSteps += leg.volume.steps;
        } else {
            b.sells += lots;
            b.netSteps -= leg.volume.steps;
        }
    }
    return b;
}

double LiveRunner::floatingNetLocked() const {
    double net = 0.0;
    for (const auto& leg : openLegs_) net += legPnl(leg).net;
    return net;
}

double LiveRunner::netAtMidLocked(double mid) const {
    const SymbolSpec& spec = config_.strategy.symbol;
    const double halfSpread = latestBid_ && latestAsk_ ? (*latestAsk_ - *latestBid_) / 2.0 : 0.0;
    const double perPoint = spec.tickValue / spec.tickSize;  // money per 1.0 price move per lot
    double net = 0.0;
    for (const auto& leg : openLegs_) {
        const double lots = leg.volume.toDouble(spec);
        const double entry = leg.price.toDouble(spec);
        const double move = leg.side == OrderSide::BuyStop ? mid - entry : entry - mid;
        net += (move - halfSpread) * perPoint * lots - roundTurnCommissionMoney(lots, entry, spec, config_.strategy.costs);
    }
    return net;
}

void LiveRunner::onAccountInfoLocked(const FixMessage& msg) {
    auto num = [&](int t, double fallback = 0.0) {
        const auto v = msg.getOptionalString(t);
        if (!v || v->empty()) return fallback;
        try {
            return std::stod(*v);
        } catch (...) {
            return fallback;
        }
    };
    auto str = [&](int t) { return msg.getOptionalString(t).value_or(""); };
    const bool first = !brokerAccount_.known;
    BrokerAccountInfo a;
    a.known = num(tag::MuukBalance) > 0.0 || num(tag::MuukEquity) > 0.0;  // zeros = not heard from MT5 yet
    a.balance = num(tag::MuukBalance);
    a.equity = num(tag::MuukEquity);
    a.margin = num(tag::MuukMargin);
    a.freeMargin = num(tag::MuukFreeMargin);
    a.currency = str(tag::MuukCurrency);
    a.leverage = num(tag::MuukLeverage);
    a.hedgedMarginPercent = num(tag::MuukHedgedMarginPercent, -1.0);
    a.contractSize = num(tag::MuukContractSize);
    a.volumeMin = num(tag::MuukVolumeMin);
    a.volumeStep = num(tag::MuukVolumeStep);
    a.login = str(tag::MuukLogin);
    a.server = str(tag::MuukServer);
    a.company = str(tag::MuukCompany);
    a.hedging = str(tag::MuukHedging) != "N";
    a.symbol = str(tag::MuukSymbol);
    a.tickSize = num(tag::MuukTickSize);
    a.tickValue = num(tag::MuukTickValue);
    a.marginPerLot = num(tag::MuukMarginPerLot);
    a.digits = static_cast<int>(num(tag::MuukDigits));
    a.volumeMax = num(tag::MuukVolumeMax);
    a.limitOrders = static_cast<int>(num(tag::MuukLimitOrders));
    a.tradeMode = str(tag::MuukTradeMode);
    a.commissionInPerLot = num(tag::MuukCommissionInPerLot, -1.0);
    a.commissionOutPerLot = num(tag::MuukCommissionOutPerLot, -1.0);
    // Per-position swap: a repeating group, one id then its swap and commission.
    positionSwap_.clear();
    const auto ids = msg.getAll(tag::MuukCostPositionId);
    const auto swaps = msg.getAll(tag::MuukCostSwap);
    for (std::size_t i = 0; i < ids.size() && i < swaps.size(); ++i) {
        try {
            const double sw = std::stod(swaps[i]);
            positionSwap_[ids[i]] = sw;
            a.swapTotal += sw;
        } catch (...) {
        }
    }
    brokerAccount_ = a;
    brokerAccountAt_ = std::chrono::steady_clock::now();
    if (first && a.known) {
        log("[mt5] account " + a.login + " on " + a.server + " (" + a.company + "): " + a.currency + " " +
            usdText(a.balance).substr(a.balance < 0.0 ? 2 : 1) + ", 1:" + std::to_string(static_cast<int>(a.leverage)) +
            (a.cent() ? " -- a cent account (every amount is in US cents)" : "") + ", symbol " + a.symbol);
    }
    if (!brokerSpecChecked_ && a.known && a.volumeMin > 0.0) {
        brokerSpecChecked_ = true;
        if (!engine_) adoptBrokerSymbolLocked(a);
    }
}

void LiveRunner::adoptBrokerSymbolLocked(const BrokerAccountInfo& a) {
    const SymbolSpec old = config_.strategy.symbol;
    SymbolSpec spec = old;
    if (a.tickSize > 0.0) spec.tickSize = a.tickSize;
    if (a.digits > 0) spec.digits = a.digits;
    if (a.tickValue > 0.0 && a.tickSize > 0.0) {
        spec.tickValue = a.tickValue;
        spec.contractSize = a.tickValue / a.tickSize;  // account money per 1.00 move per lot
    }
    if (a.volumeStep > 0.0) spec.volumeStep = a.volumeStep;
    if (a.volumeMin > 0.0) spec.volumeMin = a.volumeMin;
    if (a.volumeMax > 0.0) spec.volumeMax = a.volumeMax;
    StrategyConfig& st = config_.strategy;
    auto price = [&](Price p) { return Price::fromDouble(p.toDouble(old), spec); };
    auto volume = [&](Volume v) { return Volume::fromDouble(v.toDouble(old), spec); };
    auto ticks = [&](int64_t t) {
        return static_cast<int64_t>(std::llround(static_cast<double>(t) * old.tickSize / spec.tickSize));
    };
    st.range = RangeConfig{price(st.range.lowerBound), price(st.range.upperBound)};
    const double firstLot = st.lotProgression.initialLot.toDouble(old);
    st.lotProgression.initialLot = volume(st.lotProgression.initialLot);
    st.lotProgression.lotIncrement = volume(st.lotProgression.lotIncrement);
    st.riskLimits.maxLot = volume(st.riskLimits.maxLot);
    st.riskLimits.maxSpreadTicks = ticks(st.riskLimits.maxSpreadTicks);
    st.riskLimits.maxSlippageTicks = ticks(st.riskLimits.maxSlippageTicks);
    st.symbol = spec;
    std::string notes;
    // Cent or standard: what the account's currency says (the report and the app count in it).
    config_.connection.accountKind = a.cent() ? "cent" : "standard";
    // Commission: what this account really pays, learned from its own deals.
    if (a.commissionInPerLot >= 0.0 || a.commissionOutPerLot >= 0.0) {
        const double in = a.commissionInPerLot >= 0.0 ? a.commissionInPerLot : a.commissionOutPerLot;
        const double out = a.commissionOutPerLot >= 0.0 ? a.commissionOutPerLot : a.commissionInPerLot;
        st.costs.commissionPerLotRoundTurn = in + out;
        st.costs.commissionPerMillionPerSide = 0.0;
        notes += "; commission " + priceTextOf(in + out) + " " + a.currency + " per lot round turn (" + priceTextOf(in) +
                 " in + " + priceTextOf(out) + " out, from this account's deals)";
    } else {
        notes += "; no gold deals on this account yet -- commission from Settings (" +
                 priceTextOf(st.costs.commissionPerLotRoundTurn) + " per lot, " +
                 priceTextOf(st.costs.commissionPerMillionPerSide) + " per million); the first fills report the real one";
    }
    if (firstLot + 1e-9 < spec.volumeMin) {
        st.lotProgression.initialLot = Volume::fromDouble(spec.volumeMin, spec);
        notes += "; first lot raised to the broker's minimum " + priceTextOf(spec.volumeMin);
    }
    if (a.limitOrders > 0 && st.riskLimits.maxOrders > a.limitOrders - 2) {
        st.riskLimits.maxOrders = std::max(1, a.limitOrders - 2);
        notes += "; max orders " + std::to_string(st.riskLimits.maxOrders) + " (the broker allows " +
                 std::to_string(a.limitOrders) + " pending)";
    }
    log("[mt5] symbol " + a.symbol + " as the broker has it: " + std::to_string(spec.digits) + " digits, lots " +
        priceTextOf(spec.volumeMin) + " min / " + priceTextOf(spec.volumeStep) + " step, P/L in " + a.currency + " (" +
        priceTextOf(spec.contractSize) + " per 1.00 move per lot)" + notes);
}

bool LiveRunner::brokerAccountFreshLocked() const {
    return brokerAccount_.known && std::chrono::steady_clock::now() - brokerAccountAt_ < std::chrono::seconds(15);
}

double LiveRunner::equityEstimateLocked(double floating) const {
    if (brokerAccountFreshLocked()) {
        // MT5's own equity, moved by how far `floating` is from the positions' net now.
        const double now = latestBid_ && latestAsk_ ? floatingNetLocked() : floating;
        return brokerAccount_.equity + (floating - now);
    }
    const double balance = config_.connection.accountBalance;
    if (balance <= 0.0) return -1.0;
    return balance + dayRealized_ + floating;
}

muuk::core::MarginRule LiveRunner::marginRuleLocked() const {
    if (brokerAccountFreshLocked()) {
        // The leverage that reproduces MT5's own margin for 1 lot at this price.
        double leverage = brokerAccount_.leverage;
        if (brokerAccount_.marginPerLot > 0.0 && latestBid_ && latestAsk_) {
            leverage = config_.strategy.symbol.contractSize * (*latestBid_ + *latestAsk_) / 2.0 / brokerAccount_.marginPerLot;
        }
        return muuk::core::MarginRule{
            leverage > 0.0 ? leverage : std::max(1.0, config_.connection.leverage),
            brokerAccount_.hedgedMarginPercent >= 0.0 ? brokerAccount_.hedgedMarginPercent
                                                      : config_.connection.recovery.hedgedMarginPercent};
    }
    return muuk::core::MarginRule{std::max(1.0, config_.connection.leverage),
                                  config_.connection.recovery.hedgedMarginPercent};
}

bool LiveRunner::guardsOnLocked() const {
    const RecoveryConfig& rc = config_.connection.recovery;
    return (config_.connection.accountBalance > 0.0 || brokerAccountFreshLocked()) &&
           (rc.guardLockMarginLevel > 0.0 || rc.guardStopMarginLevel > 0.0);
}

double LiveRunner::breakEvenMoneyLocked() const {
    return std::max(0.01, config_.strategy.escapeProfitMoney);
}

void LiveRunner::resetRecoveryLocked() {
    if (guardLockOrderId_ != 0 || escape_.buyStopId != 0 || escape_.sellStopId != 0 || escape_.legId != 0 ||
        escapeTries_ != 0 || locked_) {
        markDirty();
    }
    guardLockOrderId_ = 0;
    escape_ = EscapeState{};
    escapeTries_ = 0;
    escapeSkipLogged_ = false;
    escapeAwaitingDecision_ = false;
    escapeHeld_ = false;
    escapeExtraTries_ = 0;
    escapePhase_.clear();
    locked_ = false;
}

bool LiveRunner::guardNextOrderLocked(const PlaceOrderCommand& place) {
    const RecoveryConfig& rc = config_.connection.recovery;
    if (!guardsOnLocked() || rc.guardLockMarginLevel <= 0.0 || locked_ || guardLockOrderId_ != 0) return false;
    if (openLegs_.empty() || !latestBid_ || !latestAsk_ || !engine_ || !tradeSession_) return false;
    if (engine_->status() != EngineStatus::Active) return false;
    const SymbolSpec& spec = config_.strategy.symbol;
    const BookLots book = bookLotsLocked();
    const bool buy = place.side == OrderSide::BuyStop;
    const double lot = place.volume.toDouble(spec);
    const double line = place.price.toDouble(spec);
    const double spread = *latestAsk_ - *latestBid_;
    const double newCost = roundTurnCommissionMoney(lot, line, spec, config_.strategy.costs) +
                           spread / 2.0 * spec.tickValue / spec.tickSize * lot;
    const double equity = equityEstimateLocked(netAtMidLocked(line) - newCost);
    const double margin = bookMargin(book.buys + (buy ? lot : 0.0), book.sells + (buy ? 0.0 : lot), line,
                                     spec.contractSize, marginRuleLocked());
    const double level = marginLevelPercent(equity, margin);
    if (level >= rc.guardLockMarginLevel) return false;

    const RangeConfig range = engine_->currentRange();
    guardRangeLow_ = range.lowerBound;
    guardRangeHigh_ = range.upperBound;
    const std::string what = "level " + std::to_string(place.sequenceIndex) + " (" + (buy ? "Buy" : "Sell") +
                             " Stop " + lotsTextOf(place.volume.steps, spec) + " lot @ " + priceTextOf(line) +
                             ") would leave the margin level at about " + pctText(std::max(0.0, level)) + " (guard " +
                             pctText(rc.guardLockMarginLevel) + ")";
    cancelPendingLocked();
    engine_->stopCycle();
    cooldownActive_ = false;
    reopenAfterManualClose_ = false;
    markDirty();
    if (book.netSteps == 0) {
        lockNowLocked(what, floatingNetLocked());
        return true;
    }
    OpenLeg lk;
    lk.id = nextAdoptedId_++;
    lk.side = book.netSteps > 0 ? OrderSide::SellStop : OrderSide::BuyStop;
    lk.volume = Volume{book.netSteps > 0 ? book.netSteps : -book.netSteps};
    lk.price = lk.side == place.side ? place.price
                                     : (lk.side == OrderSide::SellStop ? range.lowerBound : range.upperBound);
    lk.requestedPrice = lk.price;
    if (stopIsMarketable(lk.side, lk.price)) {
        lockNowLocked(what + "; the price is already past that line", floatingNetLocked());
        return true;
    }
    sendStopOrderLocked(lk, 0);
    guardLockOrderId_ = lk.id;
    if (currentBasket_) currentBasket_->guardHeld = true;
    auditEventLocked(8, 1,
                     what + " -- not placed. A lock stop " + (lk.side == OrderSide::BuyStop ? "BUY " : "SELL ") +
                         lotsTextOf(lk.volume.steps, spec) + " lot waits at that line instead" +
                         (rc.closeAtBreakEven
                              ? "; if the price turns back first, the basket closes at " + usdText(breakEvenMoneyLocked())
                              : std::string()));
    return true;
}

void LiveRunner::onRecoveryFillLocked(OrderId orderId) {
    const SymbolSpec& spec = config_.strategy.symbol;
    const bool haveQuote = latestBid_ && latestAsk_;
    if (guardLockOrderId_ != 0 && orderId == guardLockOrderId_) {
        guardLockOrderId_ = 0;
        locked_ = true;
        lockedLoss_ = haveQuote ? floatingNetLocked() : 0.0;
        lockedAt_ = localTimestamp();
        lockedSince_ = std::chrono::steady_clock::now();
        if (currentBasket_) {
            currentBasket_->locked = true;
            currentBasket_->lockedLoss = lockedLoss_;
        }
        setCloseReason("locked by the margin guard");
        markDirty();
        auditEventLocked(8, 2,
                         "the lock stop filled: buys = sells, the loss is frozen at " + usdText(lockedLoss_) +
                             (config_.connection.recovery.escapeEnabled
                                  ? " -- the bot reads the market first, then escapes only with a clear trend"
                                  : " -- close it when you decide (Close range)"));
        return;
    }
    if (const auto mm = managedMarket_.find(orderId); mm != managedMarket_.end()) {
        managedMarket_.erase(mm);
        if (orderId == escape_.relockId) {
            escape_.relockId = 0;
            if (haveQuote) lockedLoss_ = floatingNetLocked();
            lockedSince_ = std::chrono::steady_clock::now();  // a fresh read of the market before any next try
            if (currentBasket_) currentBasket_->lockedLoss = lockedLoss_;
            if (config_.connection.recovery.escapeAutoRetry) {
                auditEventLocked(8, 1, "locked again, frozen at " + usdText(lockedLoss_) +
                                           " -- reading the market again before the next try");
            } else {
                escapeAwaitingDecision_ = true;
                auditEventLocked(8, 2, "the escape didn't work out: locked again, frozen at " + usdText(lockedLoss_) +
                                           ". Your call: Retry escape, Close range, or Keep locked");
            }
        }
        markDirty();
        return;
    }
    if (orderId != 0 && (orderId == escape_.buyStopId || orderId == escape_.sellStopId)) {
        const bool buy = orderId == escape_.buyStopId;
        const OrderId other = buy ? escape_.sellStopId : escape_.buyStopId;
        escape_.buyStopId = escape_.sellStopId = 0;
        if (other != 0) {
            if (const auto it = placedOrderInfo_.find(other); it != placedOrderInfo_.end()) {
                cancelledOrderInfo_.insert({other, it->second});
                placedOrderInfo_.erase(it);
                tradeSession_->send(buildOrderCancelRequest("c" + std::to_string(nextCancelReqId_++), std::to_string(other)));
            }
        }
        escape_.legId = orderId;
        escape_.buy = buy;
        escape_.trailing = false;
        for (const auto& leg : openLegs_) {
            if (leg.id == orderId) escape_.entry = leg.price;
        }
        ++escapeTries_;
        if (currentBasket_) currentBasket_->escapeTries = escapeTries_;
        // Room to breathe: locked again only once the price goes this far against it.
        const RecoveryConfig& rc = config_.connection.recovery;
        const double room = std::max(rc.escapeStopAtr * escape_.atr, rc.escapeBuffer);
        const double entry = escape_.entry.toDouble(spec);
        escape_.stop = buy ? entry - room : entry + room;
        markDirty();
        auditEventLocked(8, 1,
                         std::string("escape ") + (buy ? "BUY" : "SELL") + " filled @ " + priceTextOf(entry) + " (try " +
                             std::to_string(escapeTries_) + " of " +
                             std::to_string(rc.escapeMaxTries + escapeExtraTries_) +
                             ") -- the basket closes once its net is back at break-even; locked again only if the "
                             "price reaches " + priceTextOf(escape_.stop));
    }
}

void LiveRunner::sendManagedMarketLocked(OpenLeg leg, const std::string& what) {
    const SymbolSpec& spec = config_.strategy.symbol;
    const std::string clOrdId = std::to_string(leg.id);
    const double lots = leg.volume.toDouble(spec);
    placedOrderInfo_[leg.id] = leg;
    orderSentAt_[clOrdId] = std::chrono::steady_clock::now();
    ManagedMarket m;
    m.leg = leg;
    m.what = what;
    managedMarket_[leg.id] = m;
    log("[risk] " + what + ": " + (leg.side == OrderSide::BuyStop ? "BUY " : "SELL ") +
        lotsTextOf(leg.volume.steps, spec) + " lot at market (ClOrdID=" + clOrdId + ")");
    tradeSession_->send(
        buildMarketOrderSingle(clOrdId, symbolId_, leg.side == OrderSide::BuyStop ? 1 : 2, lots * kWireQtyPerLot));
    markDirty();
}

void LiveRunner::retryManagedMarketLocked() {
    if (managedMarket_.empty() || !tradeSession_) return;
    const auto now = std::chrono::steady_clock::now();
    std::vector<OrderId> due;
    for (const auto& [id, m] : managedMarket_) {
        if (m.retryDue && now >= m.retryAt) due.push_back(id);
    }
    for (const OrderId id : due) {
        const ManagedMarket m = managedMarket_.at(id);
        managedMarket_.erase(id);
        OpenLeg leg = m.leg;
        leg.id = nextAdoptedId_++;
        if (latestBid_ && latestAsk_) {
            leg.price = leg.requestedPrice =
                Price::fromDouble(leg.side == OrderSide::BuyStop ? *latestAsk_ : *latestBid_, config_.strategy.symbol);
        }
        if (escape_.relockId == id) escape_.relockId = leg.id;
        sendManagedMarketLocked(leg, m.what);
        managedMarket_[leg.id].attempts = m.attempts + 1;
    }
}

void LiveRunner::maybeGuardLocked() {
    if (!engine_ || !tradeSession_ || !latestBid_ || !latestAsk_) return;
    if (openLegs_.empty() || !pendingCloses_.empty()) return;
    const RecoveryConfig& rc = config_.connection.recovery;
    const double net = floatingNetLocked();
    const bool guarded = guardLockOrderId_ != 0 || locked_;

    // Back at break-even while guarded or locked: close it all.
    if (guarded && rc.closeAtBreakEven && managedMarket_.empty() && net >= breakEvenMoneyLocked()) {
        auditEventLocked(8, 1,
                         std::string(locked_ ? "the locked basket" : "the guarded basket") + " is back at " +
                             usdText(net) + " -- closing everything");
        // The real end of this basket, whatever was noted on the way.
        closeReason_ = std::string("break-even: back to ") + usdText(net) +
                       (locked_ ? " after a lock" : " while the margin guard held the next level");
        closeRangeLocked(true, closeReason_);
        return;
    }

    // Margin level now under the stop guard: lock at market.
    if (guardsOnLocked() && rc.guardStopMarginLevel > 0.0 && managedMarket_.empty()) {
        const BookLots book = bookLotsLocked();
        if (book.netSteps != 0) {
            const SymbolSpec& spec = config_.strategy.symbol;
            const double mid = (*latestBid_ + *latestAsk_) / 2.0;
            const double margin = bookMargin(book.buys, book.sells, mid, spec.contractSize, marginRuleLocked());
            const double level = marginLevelPercent(equityEstimateLocked(net), margin);
            if (level < rc.guardStopMarginLevel) {
                lockNowLocked("the margin level is about " + pctText(std::max(0.0, level)) + " (guard " +
                                  pctText(rc.guardStopMarginLevel) + ")",
                              net);
                return;
            }
        }
    }

    if (locked_ && rc.escapeEnabled) manageEscapeLocked(net);
}

void LiveRunner::readMarketLocked() {
    const std::deque<muuk::core::Bar> closed = bars_.closed();
    const int64_t newest = closed.empty() ? -1 : closed.back().startMs;
    if (newest == marketReadBar_ && marketReadBar_ != -1) return;  // read once per finished bar
    marketReadBar_ = newest;
    marketRead_ = muuk::core::readMarket(closed, config_.connection.recovery.escapeLookbackBars);
}

void LiveRunner::setEscapePhaseLocked(const char* key, const std::string& phase) {
    escapePhase_ = phase;
    // The log gets each stage once, not every changing number in it.
    if (escapePhaseKey_ == key) return;
    escapePhaseKey_ = key;
    log("[escape] " + phase);
}

void LiveRunner::retryEscape() {
    std::scoped_lock lock(engineMutex_);
    if (!locked_) return;
    escapeAwaitingDecision_ = false;
    escapeHeld_ = false;
    const int allowed = config_.connection.recovery.escapeMaxTries + escapeExtraTries_;
    if (escapeTries_ >= allowed) ++escapeExtraTries_;
    markDirty();
    auditEventLocked(8, 1, "Retry escape: the bot reads the market again and escapes only with a clear trend");
}

void LiveRunner::keepLocked() {
    std::scoped_lock lock(engineMutex_);
    if (!locked_) return;
    escapeAwaitingDecision_ = false;
    escapeHeld_ = true;
    for (OrderId* id : {&escape_.buyStopId, &escape_.sellStopId}) {
        if (*id == 0) continue;
        if (const auto it = placedOrderInfo_.find(*id); it != placedOrderInfo_.end() && tradeSession_) {
            cancelledOrderInfo_.insert({*id, it->second});
            placedOrderInfo_.erase(it);
            tradeSession_->send(buildOrderCancelRequest("c" + std::to_string(nextCancelReqId_++), std::to_string(*id)));
        }
        *id = 0;
    }
    markDirty();
    auditEventLocked(8, 1, "Keep locked: no more escapes for this basket -- close it when you decide (Close range)");
}

void LiveRunner::addHistoryBars(const std::vector<muuk::core::Bar>& bars) {
    std::scoped_lock lock(engineMutex_);
    for (const auto& b : bars) bars_.addClosed(b);
    marketReadBar_ = -1;
    readMarketLocked();
}

void LiveRunner::manageEscapeLocked(double net) {
    const RecoveryConfig& rc = config_.connection.recovery;
    const SymbolSpec& spec = config_.strategy.symbol;
    if (escape_.relockId != 0 || !managedMarket_.empty()) return;  // a lock / re-lock is still on its way
    readMarketLocked();
    using muuk::core::TrendDirection;

    // --- The escape trade is running.
    if (escape_.legId != 0) {
        const OpenLeg* leg = nullptr;
        for (const auto& l : openLegs_) {
            if (l.id == escape_.legId) leg = &l;
        }
        if (!leg) {  // closed outside the bot
            escape_.legId = 0;
            markDirty();
            return;
        }
        const double entry = escape_.entry.toDouble(spec);
        const double room = std::max(rc.escapeStopAtr * escape_.atr, rc.escapeBuffer);
        if (escape_.stop <= 0.0) escape_.stop = escape_.buy ? entry - room : entry + room;  // e.g. after a restart
        const double favorable = escape_.buy ? *latestBid_ - entry : entry - *latestAsk_;
        // Protect it only after a run bigger than the market's ordinary
        // swing: moving the line to the entry after $1 when a minute
        // typically moves $1.66 just got it knocked out (1 Oct, #36).
        const double protectAfter = std::max(rc.escapeTrailAt, room);
        if (!escape_.trailing && favorable >= protectAfter) {
            escape_.trailing = true;
            escape_.stop = entry;  // from here it can't turn into a loss of its own
            markDirty();
            log("[risk] the escape ran " + priceTextOf(favorable) + " -- its re-lock line moves to its entry " +
                priceTextOf(entry));
        }
        if (escape_.trailing) {
            // Follow the price, `room` behind it: a winning run keeps what it won.
            const double follow = escape_.buy ? *latestBid_ - room : *latestAsk_ + room;
            if (escape_.buy ? follow > escape_.stop : follow < escape_.stop) {
                escape_.stop = follow;
                markDirty();
            }
        }
        setEscapePhaseLocked("running", std::string("escape ") + (escape_.buy ? "BUY " : "SELL ") +
                             lotsTextOf(leg->volume.steps, spec) + " lot running: net " + usdText(net) +
                             ", closes at break-even (" + usdText(breakEvenMoneyLocked()) + "), locked again at " +
                             priceTextOf(escape_.stop));
        const bool hit = escape_.buy ? *latestBid_ <= escape_.stop : *latestAsk_ >= escape_.stop;
        if (!hit) return;
        OpenLeg relock;
        relock.id = nextAdoptedId_++;
        relock.side = escape_.buy ? OrderSide::SellStop : OrderSide::BuyStop;
        relock.volume = leg->volume;
        relock.price = relock.requestedPrice =
            Price::fromDouble(relock.side == OrderSide::BuyStop ? *latestAsk_ : *latestBid_, spec);
        auditEventLocked(8, 1,
                         "the escape reached its re-lock line " + priceTextOf(escape_.stop) + " -- locking again (" +
                             std::to_string(escapeTries_) + " of " +
                             std::to_string(rc.escapeMaxTries + escapeExtraTries_) + " tries used)");
        escape_.legId = 0;
        escape_.trailing = false;
        escape_.stop = 0.0;
        escape_.relockId = relock.id;
        sendManagedMarketLocked(relock, "re-lock");
        return;
    }

    // --- Waiting on the operator, or nothing left to try.
    if (escapeHeld_) {
        setEscapePhaseLocked("held", "kept locked (your choice) -- close it when you decide");
        return;
    }
    if (escapeAwaitingDecision_) {
        setEscapePhaseLocked("await", "the escape didn't work out -- your call: Retry escape, Close range or Keep locked");
        return;
    }
    if (escapeTries_ >= rc.escapeMaxTries + escapeExtraTries_) {
        setEscapePhaseLocked("notries", "no escape tries left -- locked; Retry escape or Close range");
        return;
    }
    const BookLots book = bookLotsLocked();
    if (book.netSteps != 0) return;  // not balanced (yet)

    // --- 1. Let the market settle and read it before anything.
    const auto waited = std::chrono::steady_clock::now() - lockedSince_;
    const auto analyze = std::chrono::minutes(rc.escapeAnalyzeMinutes);
    if (waited < analyze) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(analyze - waited).count();
        char buf[48];
        std::snprintf(buf, sizeof(buf), "reading the market, %lld:%02lld left", static_cast<long long>(left / 60),
                      static_cast<long long>(left % 60));
        setEscapePhaseLocked("analyze", std::string(buf) + " -- " + marketRead_.why);
        return;
    }
    if (!marketRead_.enoughData) {
        setEscapePhaseLocked("collect", "reading the market: " + marketRead_.why);
        return;
    }

    // --- 2. An escape order out: keep it only while the trend still points its way.
    const TrendDirection dir = marketRead_.direction;
    if (escape_.buyStopId != 0 || escape_.sellStopId != 0) {
        const bool up = escape_.buyStopId != 0;
        if ((up && dir == TrendDirection::Up) || (!up && dir == TrendDirection::Down)) {
            const OrderId id = up ? escape_.buyStopId : escape_.sellStopId;
            const auto it = placedOrderInfo_.find(id);
            setEscapePhaseLocked("waiting", std::string("waiting for the ") + (up ? "UP" : "DOWN") + " breakout: " +
                                 (up ? "Buy" : "Sell") + " Stop" +
                                 (it != placedOrderInfo_.end() ? " @ " + priceTextOf(it->second.price.toDouble(spec))
                                                               : std::string()) +
                                 " -- " + marketRead_.why);
            return;
        }
        OrderId& id = up ? escape_.buyStopId : escape_.sellStopId;
        if (const auto it = placedOrderInfo_.find(id); it != placedOrderInfo_.end()) {
            cancelledOrderInfo_.insert({id, it->second});
            placedOrderInfo_.erase(it);
            tradeSession_->send(buildOrderCancelRequest("c" + std::to_string(nextCancelReqId_++), std::to_string(id)));
        }
        id = 0;
        markDirty();
        auditEventLocked(8, 1, std::string("the ") + (up ? "up" : "down") +
                                   " trend faded before the escape order filled -- order taken back, still locked (" +
                                   marketRead_.why + ")");
        return;
    }

    // --- 3. Only with a clear trend, and only its way.
    if (dir == TrendDirection::None) {
        setEscapePhaseLocked("sideways", "no clear direction -- staying locked, no escape into a sideways market (" +
                             marketRead_.why + ")");
        return;
    }
    const double mid = (*latestBid_ + *latestAsk_) / 2.0;
    const double equity = equityEstimateLocked(net);
    muuk::core::ReverseTradeInput in;
    in.spec = spec;
    in.costs = config_.strategy.costs;
    in.margin = marginRuleLocked();
    in.buyLots = book.buys;
    in.sellLots = book.sells;
    in.floating = net;
    in.equity = equity;
    in.price = mid;
    in.spread = *latestAsk_ - *latestBid_;
    in.move = rc.escapeMove;
    in.maxLot = config_.strategy.riskLimits.maxLot.toDouble(spec);
    const muuk::core::ReverseTrade r = muuk::core::reverseTrade(in);
    const double need = rc.guardLockMarginLevel > 0.0 ? rc.guardLockMarginLevel : 100.0;
    if (equity < 0.0 || r.capped || r.marginLevelAfter < need) {
        if (!escapeSkipLogged_) {
            escapeSkipLogged_ = true;
            auditEventLocked(8, 1,
                             "escape needs " + priceTextOf(r.lot) + " lot" +
                                 (equity < 0.0 ? std::string(" but the balance isn't set (Settings)")
                                               : (r.capped ? std::string(", above the max lot")
                                                           : ", margin level would be about " +
                                                                 pctText(std::max(0.0, r.marginLevelAfter)))) +
                                 " -- no escape, the basket stays locked");
        }
        setEscapePhaseLocked("margin", "no escape: not enough margin for " + priceTextOf(r.lot) + " lot -- locked");
        return;
    }
    // Beyond the recent extreme (a real breakout), and never at the price itself.
    const bool up = dir == TrendDirection::Up;
    const double beyond = std::max(rc.escapeBuffer, 0.25 * marketRead_.atr);
    const double line = up ? std::max(marketRead_.recentHigh + beyond, *latestAsk_ + rc.escapeBuffer)
                           : std::min(marketRead_.recentLow - beyond, *latestBid_ - rc.escapeBuffer);
    OpenLeg order;
    order.id = nextAdoptedId_++;
    order.side = up ? OrderSide::BuyStop : OrderSide::SellStop;
    order.volume = Volume::fromDouble(r.lot, spec);
    order.price = order.requestedPrice = Price::fromDouble(line, spec);
    if (stopIsMarketable(order.side, order.price)) return;
    sendStopOrderLocked(order, 0);
    (up ? escape_.buyStopId : escape_.sellStopId) = order.id;
    escape_.atr = marketRead_.atr;
    markDirty();
    auditEventLocked(8, 1,
                     std::string("market reads ") + marketRead_.why + " -- escape " + (up ? "Buy" : "Sell") +
                         " Stop " + lotsTextOf(order.volume.steps, spec) + " lot @ " + priceTextOf(line) +
                         (up ? " (beyond the last " : " (beyond the last ") +
                         std::to_string(rc.escapeLookbackBars) + " minutes' " + (up ? "high" : "low") + "); a " +
                         priceTextOf(rc.escapeMove) + " run wins the " + usdText(net) + " back");
}

bool LiveRunner::accountFlat() const {
    return openLegs_.empty() && placedOrderInfo_.empty() && pendingCloses_.empty() && waitingOrders_.empty();
}

bool LiveRunner::stopIsMarketable(OrderSide side, Price price) const {
    if (!latestBid_ || !latestAsk_) return false;
    const double px = price.toDouble(config_.strategy.symbol);
    return side == OrderSide::BuyStop ? px <= *latestAsk_ : px >= *latestBid_;
}

void LiveRunner::sendStopOrderLocked(const OpenLeg& leg, int sequenceIndex) {
    const std::string clOrdId = std::to_string(leg.id);
    const int side = (leg.side == OrderSide::BuyStop) ? 1 : 2;
    const double lots = leg.volume.toDouble(config_.strategy.symbol);
    // cTrader's FIX OrderQty(38) is in "lot hundredths" (its
    // platform-wide volume convention), not raw lots -- confirmed
    // empirically: sending OrderQty=2/3 opened 0.02/0.03-lot
    // positions, and the broker's stated minimum ("1.00") turned out
    // to mean 0.01 lot, not 1.00 lot. engine/core's SymbolSpec/Volume
    // stay in real lots throughout (P&L, risk checks); this scaling
    // is purely a wire-format detail of this one broker/dialect, so it
    // belongs here rather than in engine/core.
    const double qty = lots * kWireQtyPerLot;
    const double stopPx = leg.price.toDouble(config_.strategy.symbol);
    std::ostringstream oss;
    oss << "[order] placing seq=" << sequenceIndex << " side=" << side << " lots=" << lots << " stopPx=" << stopPx
        << " (ClOrdID=" << clOrdId << ")";
    log(oss.str());
    placedOrderInfo_[leg.id] = leg;
    orderSentAt_[clOrdId] = std::chrono::steady_clock::now();
    if (currentBasket_) ++currentBasket_->ordersPlaced;
    tradeSession_->send(buildNewOrderSingle(clOrdId, symbolId_, side, qty, stopPx));
}

void LiveRunner::releaseWaitingOrdersLocked() {
    if (waitingOrders_.empty() || !tradeSession_) return;
    for (auto it = waitingOrders_.begin(); it != waitingOrders_.end();) {
        if (stopIsMarketable(it->second.side, it->second.price)) {
            ++it;
            continue;
        }
        log("[order] the price is back on the right side of waiting order " + std::to_string(it->first) +
            " -- placing it now");
        const OpenLeg leg = it->second;
        it = waitingOrders_.erase(it);
        sendStopOrderLocked(leg, 0);
        markDirty();
    }
}

bool LiveRunner::spreadTooWideLocked() {
    const int64_t limit = config_.strategy.riskLimits.maxSpreadTicks;
    if (limit <= 0 || !latestBid_ || !latestAsk_) return false;
    const SymbolSpec& spec = config_.strategy.symbol;
    const int64_t spread = Price::fromDouble(*latestAsk_, spec).ticks - Price::fromDouble(*latestBid_, spec).ticks;
    if (spread <= limit) {
        if (spreadWaitLogged_) log("[spread] back within the limit -- a new range can open");
        spreadWaitLogged_ = false;
        return false;
    }
    if (!spreadWaitLogged_) {
        log("[spread] spread " + std::to_string(static_cast<double>(spread) * spec.tickSize) + " is above the limit " +
            std::to_string(static_cast<double>(limit) * spec.tickSize) + " -- a new range waits until it narrows");
        spreadWaitLogged_ = true;
    }
    return true;
}

void LiveRunner::noteBrokerIssueLocked(const std::string& issue) {
    log("[verify] " + issue);
    brokerIssues_.emplace_back(std::chrono::steady_clock::now(), localTimestamp().substr(11) + "  " + issue);
    if (brokerIssues_.size() > 20) brokerIssues_.erase(brokerIssues_.begin());
}

void LiveRunner::noteMissedLocked() {
    if (currentBasket_) ++currentBasket_->ordersMissed;
}

void LiveRunner::finishRuntimeOrderCheckLocked(const std::vector<ExecutionReportInfo>& reports) {
    const SymbolSpec& spec = config_.strategy.symbol;
    std::unordered_set<OrderId> atBroker;
    std::vector<const ExecutionReportInfo*> foreign;
    for (const auto& r : reports) {
        if (r.symbolId && *r.symbolId != symbolId_) continue;
        const auto id = tryParseOrderId(r.clOrdId);
        if (!id) continue;  // placed by hand in cTrader -- not the bot's business
        atBroker.insert(*id);
        if (placedOrderInfo_.count(*id) == 0 && cancelledOrderInfo_.count(*id) == 0 && waitingOrders_.count(*id) == 0) {
            foreign.push_back(&r);
        }
    }

    std::vector<OrderId> ids;
    for (const auto& [id, leg] : placedOrderInfo_) ids.push_back(id);
    for (const OrderId id : ids) {
        auto it = placedOrderInfo_.find(id);
        if (it == placedOrderInfo_.end()) continue;
        if (atBroker.count(id) > 0) {
            it->second.confirmed = true;
            orderSentAt_.erase(std::to_string(id));
            continue;
        }
        // Sent after the question went out: the answer can't include it yet.
        if (const auto sent = orderSentAt_.find(std::to_string(id));
            sent != orderSentAt_.end() && sent->second > orderStatusRequestedAt_) {
            continue;
        }
        const OpenLeg order = it->second;
        placedOrderInfo_.erase(it);
        orderSentAt_.erase(std::to_string(id));
        if (order.adopted) {
            log("[verify] old order " + std::to_string(id) + " is no longer at the broker");
            continue;
        }
        noteMissedLocked();
        noteBrokerIssueLocked("MISSED: " + std::string(order.side == OrderSide::BuyStop ? "Buy" : "Sell") + " Stop " +
                              std::to_string(order.volume.toDouble(spec)).substr(0, 4) + " lot @ " +
                              std::to_string(order.price.toDouble(spec)).substr(0, 7) + " (ClOrdID " +
                              std::to_string(id) + ") is not at the broker -- placing it again");
        markDirty();
        if (engine_ && engine_->status() == EngineStatus::Active) {
            for (const auto& cmd : engine_->replaceOrder(id)) handleCommand(cmd);
        }
    }

    // Working orders with a bot-style id that this bot didn't place: another
    // copy of Muuk (or an older run) is trading this account.
    for (const ExecutionReportInfo* r : foreign) {
        const OrderId id = *tryParseOrderId(r->clOrdId);
        OpenLeg leg;
        leg.id = id;
        leg.side = r->side == 2 ? OrderSide::SellStop : OrderSide::BuyStop;
        leg.volume = Volume::fromDouble(r->orderQty.value_or(0.0) / kWireQtyPerLot, spec);
        leg.price = Price::fromDouble(r->stopPx.value_or(0.0), spec);
        leg.requestedPrice = leg.price;
        leg.adopted = true;
        leg.confirmed = true;
        placedOrderInfo_[id] = leg;
        if (reportedForeignOrders_.insert(id).second) {
            noteBrokerIssueLocked("a working order this bot didn't place is at the broker (ClOrdID " + std::to_string(id) +
                                  ") -- is Muuk also running on another PC/VPS with this account? Listed as \"old\".");
        }
    }
    awaitingAck_.clear();
    brokerCheckedAt_ = localTimestamp();
}

void LiveRunner::checkBasketHealthLocked() {
    if (!engine_ || !tradeSession_) return;
    // Orders the broker hasn't answered at all: ask it what it has.
    const auto now = std::chrono::steady_clock::now();
    if (!awaitingAck_.empty() && now - ackCheckSentAt_ > std::chrono::seconds(15)) {
        log("[guard] the broker didn't answer the order check -- will ask again");
        awaitingAck_.clear();
        activeMassStatusReqId_.clear();
    }
    if (awaitingAck_.empty() && activeMassStatusReqId_.empty() && config_.connection.orderAckTimeoutSeconds > 0) {
        const auto timeout = std::chrono::seconds(config_.connection.orderAckTimeoutSeconds);
        for (const auto& [id, leg] : placedOrderInfo_) {
            const auto sent = orderSentAt_.find(std::to_string(id));
            if (sent != orderSentAt_.end() && now - sent->second > timeout) awaitingAck_.insert(id);
        }
        if (!awaitingAck_.empty()) {
            log("[guard] no answer from the broker for " + std::to_string(awaitingAck_.size()) + " order(s) after " +
                std::to_string(config_.connection.orderAckTimeoutSeconds) + "s -- checking with the broker");
            ackCheckSentAt_ = now;
            orderStatusRequestedAt_ = now;
            requestOrderStatusLocked();
        }
    }
    if (engine_->status() != EngineStatus::Active || engine_->seeding() || !pendingCloses_.empty()) {
        return;
    }
    const SymbolSpec& spec = config_.strategy.symbol;
    const muuk::core::EngineState st = engine_->exportState();
    for (const auto& armed : st.pendingOrders) {
        const OrderId id = armed.order.id;
        if (placedOrderInfo_.count(id) > 0 || waitingOrders_.count(id) > 0 || cancelledOrderInfo_.count(id) > 0 ||
            operatorCancelled_.count(id) > 0) {
            continue;
        }
        noteMissedLocked();
        log("[guard] MISSED: pending " + std::string(armed.order.side == OrderSide::BuyStop ? "Buy" : "Sell") +
            " Stop " + std::to_string(armed.order.volume.toDouble(spec)) + " lot @ " +
            std::to_string(armed.order.price.toDouble(spec)) + " (ClOrdID=" + std::to_string(id) +
            ") isn't at the broker -- placing it again");
        for (const auto& cmd : engine_->replaceOrder(id)) handleCommand(cmd);
    }
    if (engine_->activeOrderCount() == 0 && !openLegs_.empty() && config_.strategy.reArmOnFill && healthRearms_ < 3) {
        const auto last = std::max_element(openLegs_.begin(), openLegs_.end(), [](const OpenLeg& a, const OpenLeg& b) {
            return a.adopted != b.adopted ? a.adopted : a.id < b.id;
        });
        ++healthRearms_;
        noteMissedLocked();
        log("[guard] MISSED: the running basket has " + std::to_string(openLegs_.size()) +
            " position(s) but no pending order -- placing its next one");
        for (const auto& cmd : engine_->armNextAfter(last->side)) handleCommand(cmd);
    }
}

void LiveRunner::placeWaitingOrderNow(OrderId id) {
    std::scoped_lock lock(engineMutex_);
    if (!tradeSession_) return;
    const auto it = waitingOrders_.find(id);
    if (it == waitingOrders_.end()) return;
    const OpenLeg leg = it->second;
    waitingOrders_.erase(it);
    const std::string clOrdId = std::to_string(id);
    const double lots = leg.volume.toDouble(config_.strategy.symbol);
    log("[manual] operator requested: place waiting " + std::string(leg.side == OrderSide::BuyStop ? "Buy" : "Sell") +
        " " + std::to_string(lots) + " lot now at market (ClOrdID=" + clOrdId + ")");
    // Tracked like any placed order: its fill is processed as this order's fill.
    placedOrderInfo_[id] = leg;
    orderSentAt_[clOrdId] = std::chrono::steady_clock::now();
    tradeSession_->send(
        buildMarketOrderSingle(clOrdId, symbolId_, leg.side == OrderSide::BuyStop ? 1 : 2, lots * kWireQtyPerLot));
    markDirty();
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
    // the broker (pendingCloses_ empty -- see the
    // ExecutionReport::Trade handling in onTradeMessage), start the next
    // cycle rather than sitting idle. With seedOffsetPoints>0 this re-seeds
    // around the tick that just triggered the close (see armInitialOrders'
    // comment on lastTick_), so each new cycle re-anchors to live price
    // instead of drifting away from a now-closed cycle's range. Never
    // starts a new cycle while any closing leg is still outstanding, or
    // once basketCloseFailed_ is set -- either would open a fresh range on
    // top of a position that hasn't actually been booked yet.
    // Two ways here: a basket booked by the engine (Completed), or the
    // operator's Close range asking for a new range afterwards (Idle).
    const bool afterTarget = engine_->status() == EngineStatus::Completed;
    const bool afterManualClose = engine_->status() == EngineStatus::Idle && reopenAfterManualClose_;
    if ((!afterTarget && !afterManualClose) || !accountFlat() || basketCloseFailed_) {
        return;
    }
    if (!config_.connection.autoContinue) {
        reopenAfterManualClose_ = false;
        return;  // the operator opens the next range by hand (openRange)
    }

    const int cooldownSeconds = afterManualClose ? reopenCooldownSeconds_ : config_.connection.basketCooldownSeconds;
    if (cooldownSeconds > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!cooldownActive_) {
            // First call to see the basket fully flat -- start the clock
            // from right now, not from whenever it merely decided to close.
            cooldownActive_ = true;
            cooldownUntil_ = now + std::chrono::seconds(cooldownSeconds);
            log(std::string(afterManualClose ? "[range] flat" : "[basket] fully flat") +
                "; next range opens after a " + std::to_string(cooldownSeconds) + "s cooldown");
            markDirty();
            return;
        }
        if (now < cooldownUntil_) {
            return;  // still cooling down -- this function gets called again on the next tick
        }
    }
    // A new range never starts into a spread spike or a market pause
    // (session end, news, daily break, weekend): wait (re-checked on every
    // tick) -- the cooldown, if any, stays served.
    if (spreadTooWideLocked() || marketBlocksLocked()) return;

    cooldownActive_ = false;
    markDirty();
    if (afterManualClose) {
        if (!latestBid_ || !latestAsk_) return;  // no live price to center on
        reopenAfterManualClose_ = false;
        prepareNextCycle(true);
        log("[range] opening a new range around " + std::to_string((*latestBid_ + *latestAsk_) / 2.0));
        closeReason_.clear();
        beginBasket();
        for (const auto& cmd : engine_->start()) handleCommand(cmd);
        return;
    }
    prepareNextCycle(config_.connection.autoRangeWidthPoints > 0.0);
    log("basket closed; starting the next cycle");
    closeReason_.clear();
    beginBasket();
    for (const auto& cmd : engine_->restart()) handleCommand(cmd);
}

void LiveRunner::maybeRefreshStaleEntryLocked() {
    const int timeout = config_.connection.entryTimeoutSeconds;
    if (timeout <= 0 || !engine_ || !tradeSession_ || !latestBid_ || !latestAsk_) return;
    if (engine_->status() != EngineStatus::Active) return;
    // Only a range that hasn't started: nothing filled, nothing closing,
    // no lock or guard order of its own.
    if (!openLegs_.empty() || !pendingCloses_.empty() || locked_ || guardLockOrderId_ != 0) return;
    if (currentBasket_ && !currentBasket_->legs.empty()) return;
    if (std::chrono::steady_clock::now() - currentBasketStart_ < std::chrono::seconds(timeout)) return;
    // A new range would only wait too (spread spike, market pause): keep this one.
    if (spreadTooWideLocked() || marketBlocksLocked()) return;

    log("[range] the entry order didn't trigger within " + std::to_string(timeout) +
        "s -- cancelling it and opening the range again around " +
        std::to_string((*latestBid_ + *latestAsk_) / 2.0));
    cancelPendingLocked();
    engine_->stopCycle();
    currentBasket_.reset();  // nothing filled: nothing to record
    closeReason_.clear();
    cooldownActive_ = false;
    markDirty();
    prepareNextCycle(true);
    beginBasket();
    for (const auto& cmd : engine_->start()) handleCommand(cmd);
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
    markDirty();
    if (const auto* place = std::get_if<PlaceOrderCommand>(&cmd)) {
        if (guardNextOrderLocked(*place)) return;
        const OpenLeg leg{place->id, place->side, place->volume, place->price, place->price};
        if (stopIsMarketable(place->side, place->price)) {
            // The broker would refuse a Stop the market has already passed:
            // hold it until the price is back (releaseWaitingOrdersLocked).
            waitingOrders_[place->id] = leg;
            log("[order] " + std::string(place->side == OrderSide::BuyStop ? "Buy" : "Sell") + " Stop " +
                std::to_string(place->volume.toDouble(config_.strategy.symbol)) + " lot @ " +
                std::to_string(place->price.toDouble(config_.strategy.symbol)) +
                " is already past the market (bid " + std::to_string(*latestBid_) + " / ask " +
                std::to_string(*latestAsk_) + ") -- waiting for the price to come back before placing it");
            return;
        }
        sendStopOrderLocked(leg, place->sequenceIndex);
    } else if (const auto* cancel = std::get_if<CancelOrderCommand>(&cmd)) {
        if (waitingOrders_.erase(cancel->id) > 0) {
            log("[order] dropped waiting order " + std::to_string(cancel->id) + " (never sent to the broker)");
            return;
        }
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
        log(std::string(close->stopLoss ? "[basket] STOP LOSS reached" : "[basket] target reached") +
            ", net P/L = " + std::to_string(close->netPnlMoney) + ", " + std::to_string(openLegs_.size()) +
            " leg(s) to flatten");
        ++basketsClosed_;  // booked P/L itself comes from the real closing fills (see onTradeMessage)
        setCloseReason(close->stopLoss ? "stop loss" : "profit target");
        if (currentBasket_) {
            currentBasket_->netAtTrigger = close->netPnlMoney;
            // Closed on the lowered (break-even) target rather than the full one.
            if (!close->stopLoss && engine_ && engine_->escapeActive()) {
                currentBasket_->breakEvenFromLevel = config_.strategy.escapeFromLevel;
            }
        }
        closeLegs(std::exchange(openLegs_, {}));
    } else if (const auto* alert = std::get_if<RiskAlertCommand>(&cmd)) {
        log("[risk] ALERT: limit breached (kind=" + std::to_string(static_cast<int>(alert->kind)) +
            ") -- no further orders will be armed this cycle; the open positions still close at the target / "
            "stop loss");
    }
}

void LiveRunner::closeLegs(const std::vector<OpenLeg>& legs) {
    // Closes every leg individually: an opposite-direction Market order of
    // exactly that leg's volume, aimed at that leg's own position via
    // PosMaintRptID(721). On a hedging account (this one is) the 721 part
    // is what makes it a close at all -- per the cTrader FIX spec, an order
    // without it "creates a new position", which is exactly what the close
    // buttons used to do. Shared by the automatic CloseBasketCommand path
    // and every manual close control. Caller must hold engineMutex_ and
    // must have already removed `legs` from openLegs_; any leg that can't
    // be closed is put back there.
    if (legs.empty()) {
        log("[basket] already flat -- nothing to close");
        return;
    }
    markDirty();
    // The order to close them in: each next close leaves the open exposure
    // (buy lots - sell lots still open) as small as it can. A deep basket is
    // mostly hedged pairs plus a net; closing the big net-side leg first
    // means the market moving while the rest fill barely matters. (In the
    // order they opened, a 10-level x2 basket kept up to 3+ lots exposed
    // until its very last close.)
    std::vector<std::size_t> order;
    {
        auto signedSteps = [](const OpenLeg& l) {
            return l.side == OrderSide::BuyStop ? l.volume.steps : -l.volume.steps;
        };
        int64_t open = 0;
        for (const auto& l : legs) open += signedSteps(l);
        std::vector<bool> used(legs.size(), false);
        for (std::size_t k = 0; k < legs.size(); ++k) {
            std::size_t best = legs.size();
            int64_t bestLeft = 0;
            for (std::size_t i = 0; i < legs.size(); ++i) {
                if (used[i]) continue;
                const int64_t left = std::llabs(open - signedSteps(legs[i]));
                if (best == legs.size() || left < bestLeft ||
                    (left == bestLeft && legs[i].volume.steps > legs[best].volume.steps)) {
                    best = i;
                    bestLeft = left;
                }
            }
            used[best] = true;
            open -= signedSteps(legs[best]);
            order.push_back(best);
        }
    }
    for (const std::size_t index : order) {
        const OpenLeg& leg = legs[index];
        if (leg.positionId.empty()) {
            // Sending it anyway would open a *new* opposite position.
            log("[basket] CRITICAL: can't close a " + std::string(leg.side == OrderSide::BuyStop ? "buy" : "sell") +
                " " + std::to_string(leg.volume.toDouble(config_.strategy.symbol)) +
                " lot position: the broker never told us its position id. Close it in cTrader, then press "
                "Sync from broker.");
            basketCloseFailed_ = true;
            openLegs_.push_back(leg);
            continue;
        }
        const int side = (leg.side == OrderSide::BuyStop) ? 2 : 1;  // opposite of the open leg
        const double lots = leg.volume.toDouble(config_.strategy.symbol);
        const double qty = lots * kWireQtyPerLot;
        const std::string closeClOrdId = "x" + std::to_string(nextCloseReqId_++);
        log("[basket] closing position " + leg.positionId + ": side=" + std::to_string(side) +
            " lots=" + std::to_string(lots) + " (ClOrdID=" + closeClOrdId + ")");
        pendingCloses_.insert({closeClOrdId, leg});
        orderSentAt_[closeClOrdId] = std::chrono::steady_clock::now();
        closeSentAt_[closeClOrdId] = std::chrono::steady_clock::now();
        ++closeAttempts_[leg.positionId];
        tradeSession_->send(buildMarketOrderSingle(closeClOrdId, symbolId_, side, qty, leg.positionId));
    }
}

void LiveRunner::syncPositions() {
    std::scoped_lock lock(engineMutex_);
    markDirty();
    if (!tradeSession_ || !activePosReqId_.empty()) return;
    activePosReqId_ = "pos-" + std::to_string(nextPosReqId_++);
    posReports_.clear();
    knownAtPosRequest_.clear();
    for (const auto& leg : openLegs_) {
        if (!leg.positionId.empty()) knownAtPosRequest_.insert(leg.positionId);
    }
    for (const auto& [clOrdId, leg] : pendingCloses_) {
        if (!leg.positionId.empty()) knownAtPosRequest_.insert(leg.positionId);
    }
    if (connectSync_) log("[sync] requesting open positions from the broker");
    tradeSession_->send(buildRequestForPositions(activePosReqId_));
}

void LiveRunner::finishPositionSync(const std::vector<PositionReportInfo>& reports) {
    if (connectSync_) {
        // Connecting: just collect -- reconcileOnConnectLocked() decides.
        connectPositions_ = reports;
        connectPositionsKnown_ = true;
        activePosReqId_.clear();
        {
            std::scoped_lock lock(posSyncMutex_);
            posSyncDone_ = true;
        }
        posSyncCv_.notify_all();
        return;
    }
    markDirty();
    const SymbolSpec& spec = config_.strategy.symbol;

    std::unordered_set<std::string> atBroker;
    int otherSymbols = 0;
    for (const auto& r : reports) {
        if (!r.positionId) continue;
        if (r.symbolId && *r.symbolId != symbolId_) {
            ++otherSymbols;
            continue;
        }
        atBroker.insert(*r.positionId);
    }

    // Tracked positions the broker no longer has were closed outside the bot.
    // Only positions already known when the request went out are judged --
    // anything that filled since then simply isn't in this (older) answer.
    std::vector<OpenLeg> gone;
    std::erase_if(openLegs_, [&](const OpenLeg& leg) {
        const bool closed = !leg.positionId.empty() && knownAtPosRequest_.count(leg.positionId) > 0 &&
                            atBroker.count(leg.positionId) == 0;
        if (closed) gone.push_back(leg);
        return closed;
    });
    bool basketLegGone = false;
    for (const OpenLeg& leg : gone) {
        closedPositionIds_.insert(leg.positionId);
        noteBrokerIssueLocked("position " + leg.positionId + " (" + (leg.side == OrderSide::BuyStop ? "BUY " : "SELL ") +
                              std::to_string(leg.volume.toDouble(spec)).substr(0, 4) +
                              " lot) is no longer at the broker -- closed outside the bot");
        recordLegClosed(leg, 0.0, 0.0, 0.0, "closed outside the bot");
        basketLegGone = basketLegGone || !leg.adopted;
    }
    for (auto& leg : openLegs_) {
        if (atBroker.count(leg.positionId) > 0) leg.confirmed = true;
    }
    // A running basket missing one of its own positions can't be managed
    // safely any more (its P/L and next order assume that position): stop
    // it -- pending orders cancelled, the rest of the positions listed, and
    // nothing new opens until the operator decides.
    if (basketLegGone && basketRunningLocked()) {
        noteBrokerIssueLocked("the running basket lost a position -- stopped it and cancelled its pending orders; "
                              "close the rest or press Open range");
        setCloseReason("interrupted: a position was closed outside the bot");
        cancelPendingLocked();
        engine_->stopCycle();
        cooldownActive_ = false;
        reopenAfterManualClose_ = false;
    }

    // Closing orders the broker never confirmed (rejected, lost, no answer):
    // gone from the broker -> closed there; still open -> send the close
    // again, at most 3 times, then stop and wait for the operator.
    {
        const auto now = std::chrono::steady_clock::now();
        const auto confirm = std::chrono::seconds(std::max(1, config_.connection.closeConfirmSeconds));
        std::vector<std::pair<std::string, OpenLeg>> closedThere;
        std::vector<std::pair<std::string, OpenLeg>> retry;
        for (const auto& [clOrdId, leg] : pendingCloses_) {
            const auto sent = closeSentAt_.find(clOrdId);
            const bool overdue = sent == closeSentAt_.end() || now - sent->second > confirm;
            if (closeRecheck_.count(clOrdId) == 0 && !overdue) continue;
            if (knownAtPosRequest_.count(leg.positionId) == 0) continue;  // sent after this question
            (atBroker.count(leg.positionId) == 0 ? closedThere : retry).emplace_back(clOrdId, leg);
        }
        for (const auto& [clOrdId, leg] : closedThere) {
            pendingCloses_.erase(clOrdId);
            closeSentAt_.erase(clOrdId);
            closeRecheck_.erase(clOrdId);
            closeAttempts_.erase(leg.positionId);
            closedPositionIds_.insert(leg.positionId);
            recordLegClosed(leg, 0.0, 0.0, 0.0, "closed at the broker (no fill report) -- see cTrader for its price");
            auditEventLocked(3, 1, "position " + leg.positionId + " is already closed at the broker -- booked as closed");
        }
        for (const auto& [clOrdId, leg] : retry) {
            pendingCloses_.erase(clOrdId);
            closeSentAt_.erase(clOrdId);
            closeRecheck_.erase(clOrdId);
            if (closeAttempts_[leg.positionId] < 3) {
                auditEventLocked(3, 1, "position " + leg.positionId + " is still open -- closing it again (try " +
                                           std::to_string(closeAttempts_[leg.positionId] + 1) + " of 3)");
                closeLegs({leg});
            } else {
                openLegs_.push_back(leg);
                basketCloseFailed_ = true;
                auditEventLocked(3, 2, "position " + leg.positionId +
                                           " still open after 3 closing orders -- close it in cTrader, then press "
                                           "Sync from broker");
            }
        }
        if (!closedThere.empty() && pendingCloses_.empty()) {
            maybeFinalizeBasket();
            if (engine_) maybeRestartAfterBasketClose();
        }
    }

    // Positions the broker has that the bot doesn't track: adopt them.
    std::unordered_set<std::string> tracked;
    for (const auto& leg : openLegs_) tracked.insert(leg.positionId);
    for (const auto& [clOrdId, leg] : pendingCloses_) tracked.insert(leg.positionId);
    int adopted = 0;
    for (const auto& r : reports) {
        if (!r.positionId || (r.symbolId && *r.symbolId != symbolId_)) continue;
        if (tracked.count(*r.positionId) > 0 || closedPositionIds_.count(*r.positionId) > 0) continue;
        const bool buy = r.longQty > 0.0;
        const double qty = buy ? r.longQty : r.shortQty;
        if (qty <= 0.0) continue;
        OpenLeg leg;
        leg.id = nextAdoptedId_++;
        leg.side = buy ? OrderSide::BuyStop : OrderSide::SellStop;
        leg.volume = Volume::fromDouble(qty / kWireQtyPerLot, spec);
        leg.price = Price::fromDouble(r.settlPrice.value_or(0.0), spec);
        leg.requestedPrice = leg.price;
        leg.positionId = *r.positionId;
        leg.adopted = true;
        leg.confirmed = true;
        openLegs_.push_back(leg);
        ++adopted;
        if (engine_ && engine_->status() == EngineStatus::Active) {
            noteBrokerIssueLocked("position " + leg.positionId + " appeared that this bot didn't open -- a manual trade, "
                                  "or Muuk running elsewhere on this account? Listed as \"old\".");
        }
        log("[sync] found open " + std::string(buy ? "BUY" : "SELL") + " " + std::to_string(qty / kWireQtyPerLot) +
            " lot @ " + std::to_string(leg.price.toDouble(spec)) + " (position " + leg.positionId + ")");
    }

    if (adopted > 0 || !gone.empty() || !manualPositionSync_) {
        log("[sync] done: " + std::to_string(atBroker.size()) + " open position(s) on " + config_.connection.symbolName +
            (adopted > 0 ? ", " + std::to_string(adopted) + " newly listed" : "") +
            (otherSymbols > 0 ? ", " + std::to_string(otherSymbols) + " on other symbols (ignored)" : ""));
    }
    manualPositionSync_ = false;
    maybeFinalizeBasket();

    if (basketCloseFailed_ && openLegs_.empty() && pendingCloses_.empty()) {
        basketCloseFailed_ = false;
        log("[sync] account is flat again -- ranges can be opened");
    }

    activePosReqId_.clear();
    {
        std::scoped_lock lock(posSyncMutex_);
        posSyncDone_ = true;
    }
    posSyncCv_.notify_all();
}

void LiveRunner::closePositionsLocked() {
    log("[manual] closing all positions (" + std::to_string(openLegs_.size()) + " leg(s))");
    closeLegs(std::exchange(openLegs_, {}));
}

void LiveRunner::cancelPendingLocked() {
    markDirty();
    if (!waitingOrders_.empty()) {
        log("[orders] dropping " + std::to_string(waitingOrders_.size()) + " waiting order(s) (never sent)");
        waitingOrders_.clear();
    }
    log("[orders] cancelling all pending orders (" + std::to_string(placedOrderInfo_.size()) + " order(s))");
    for (const auto& [orderId, leg] : placedOrderInfo_) {
        const std::string cancelClOrdId = "c" + std::to_string(nextCancelReqId_++);
        log("[orders] cancelling OrigClOrdID=" + std::to_string(orderId));
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

    if (basketRunningLocked()) {
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
            std::to_string(pendingCloses_.size()) + " closing order(s) open");
        return false;
    }
    if (!latestBid_ || !latestAsk_) {
        log("[manual] can't open a range: no live quote yet");
        return false;
    }
    spreadWaitLogged_ = false;
    if (spreadTooWideLocked()) return false;  // logs why
    marketWaitLogged_ = false;
    if (marketBlocksLocked()) return false;   // logs why

    if (engine_->status() != EngineStatus::Idle) engine_->stopCycle();  // Completed / HaltedByRisk -> Idle
    cooldownActive_ = false;
    reopenAfterManualClose_ = false;
    prepareNextCycle(true);
    log("[manual] opening a new range around " + std::to_string((*latestBid_ + *latestAsk_) / 2.0));
    closeReason_.clear();
    beginBasket();
    for (const auto& cmd : engine_->start()) handleCommand(cmd);
    return true;
}

void LiveRunner::closeRange(bool reopen) {
    std::scoped_lock lock(engineMutex_);
    if (!engine_ || !tradeSession_) return;
    log("[manual] operator requested: close range");
    closeRangeLocked(reopen, "manual: close range");
}

void LiveRunner::closeRangeLocked(bool reopen, const std::string& reason) {
    resetRecoveryLocked();
    setCloseReason(reason);
    cancelPendingLocked();
    closePositionsLocked();
    engine_->stopCycle();
    cooldownActive_ = false;
    reopenAfterManualClose_ =
        reopen && config_.connection.autoContinue && config_.connection.manualCloseCooldownSeconds > 0;
    reopenCooldownSeconds_ = config_.connection.manualCloseCooldownSeconds;
    if (reopenAfterManualClose_) {
        log("[manual] a new range opens " + std::to_string(config_.connection.manualCloseCooldownSeconds) +
            "s after everything is closed (Auto-open is on)");
    }
    maybeFinalizeBasket();
    maybeRestartAfterBasketClose();  // already flat (nothing was open)? start the countdown now
}

void LiveRunner::closeAllPositions() {
    std::scoped_lock lock(engineMutex_);
    if (!engine_ || !tradeSession_) return;
    setCloseReason("manual: close all positions");
    resetRecoveryLocked();
    closePositionsLocked();
    engine_->stopCycle();
    cooldownActive_ = false;
    reopenAfterManualClose_ = false;
    maybeFinalizeBasket();
}

void LiveRunner::cancelAllPendingOrders() {
    std::scoped_lock lock(engineMutex_);
    if (!engine_ || !tradeSession_) return;
    setCloseReason("manual: cancel all pending");
    guardLockOrderId_ = 0;
    escape_.buyStopId = escape_.sellStopId = 0;
    cancelPendingLocked();
    engine_->stopCycle();
    cooldownActive_ = false;
    reopenAfterManualClose_ = false;
    maybeFinalizeBasket();
}

bool LiveRunner::flattenAndWait(std::chrono::milliseconds timeout) {
    {
        std::scoped_lock lock(engineMutex_);
        if (!engine_ || !tradeSession_ || !running_) return accountFlat();
        const bool flat = accountFlat();
        if (!flat) {
            log("[guard] shutting down with open trades -- closing everything first");
            setCloseReason("guard: app closing");
            cancelPendingLocked();
            closePositionsLocked();
        }
        engine_->stopCycle();  // also keeps a cooldown from opening a new range meanwhile
        cooldownActive_ = false;
        reopenAfterManualClose_ = false;
        maybeFinalizeBasket();
        if (flat) return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock lock(engineMutex_);
            if (accountFlat()) {
                log("[guard] all positions closed and orders cancelled");
                return true;
            }
            if (basketCloseFailed_ || !running_) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    log("[guard] WARNING: could not confirm the account is flat -- check cTrader for open positions/orders");
    return false;
}

void LiveRunner::beginBasket() {
    BasketRecord b;
    b.number = ++basketCounter_;
    b.session = sessionStartedAt_;
    b.symbol = config_.connection.symbolName;
    b.contractSize = config_.strategy.symbol.contractSize;
    b.startedAt = localTimestamp();
    b.marketSession = sessionLabelAt(utcNowMs());
    const RangeConfig range = engine_ ? engine_->currentRange() : config_.strategy.range;
    b.rangeLower = range.lowerBound.toDouble(config_.strategy.symbol);
    b.rangeUpper = range.upperBound.toDouble(config_.strategy.symbol);
    currentBasket_ = std::move(b);
    currentBasketStart_ = std::chrono::steady_clock::now();
    markDirty();
}

void LiveRunner::recordLegOpened(const OpenLeg& leg) {
    if (!currentBasket_) beginBasket();
    const SymbolSpec& spec = config_.strategy.symbol;
    BasketLegRecord r;
    r.id = leg.id;
    r.positionId = leg.positionId;
    r.buy = leg.side == OrderSide::BuyStop;
    r.lots = leg.volume.toDouble(spec);
    r.requestedPrice = leg.requestedPrice.toDouble(spec);
    r.entryPrice = leg.price.toDouble(spec);
    r.openedAt = leg.adopted ? std::string() : localTimestamp();
    r.adopted = leg.adopted;
    currentBasket_->legs.push_back(r);
    double openLots = 0.0;
    for (const auto& l : openLegs_) openLots += l.volume.toDouble(spec);
    currentBasket_->maxOpenLots = std::max(currentBasket_->maxOpenLots, openLots);
    // Seed mode re-anchors the range on the first fill -- keep the record current.
    if (engine_) {
        const RangeConfig range = engine_->currentRange();
        currentBasket_->rangeLower = range.lowerBound.toDouble(spec);
        currentBasket_->rangeUpper = range.upperBound.toDouble(spec);
    }
}

void LiveRunner::recordLegClosed(const OpenLeg& leg, double exitPrice, double gross, double commission,
                                 const std::string& note) {
    if (!currentBasket_) {
        // Closing something outside any recorded range (e.g. an adopted
        // position): record it as its own basket.
        beginBasket();
        currentBasket_->rangeLower = currentBasket_->rangeUpper = 0.0;
    }
    auto& legs = currentBasket_->legs;
    auto it = std::find_if(legs.begin(), legs.end(), [&](const BasketLegRecord& r) { return r.id == leg.id; });
    if (it == legs.end()) {
        recordLegOpened(leg);
        it = std::prev(legs.end());
    }
    it->exitPrice = exitPrice;
    it->gross = gross;
    it->commission = commission;
    it->net = gross - commission;
    it->closedAt = localTimestamp();
    it->note = note;
}

void LiveRunner::setCloseReason(const std::string& reason) {
    if (closeReason_.empty()) closeReason_ = reason;
}

void LiveRunner::maybeFinalizeBasket() {
    if (!currentBasket_) return;
    if (engine_ && engine_->status() == EngineStatus::Active) return;  // range still running
    if (!openLegs_.empty() || !pendingCloses_.empty()) return;          // something still open at the broker

    markDirty();
    BasketRecord b = std::move(*currentBasket_);
    currentBasket_.reset();
    if (b.legs.empty()) {
        closeReason_.clear();  // range closed before anything filled -- nothing to record
        return;
    }
    b.closedAt = localTimestamp();
    b.durationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - currentBasketStart_).count();
    b.closeReason = closeReason_.empty() ? "closed" : closeReason_;
    auto startsWith = [&](const char* p) { return b.closeReason.rfind(p, 0) == 0; };
    b.outcome = basketCloseFailed_                ? "failed"
                : startsWith("profit target")     ? "target"
                : startsWith("stop loss")         ? "stop loss"
                : startsWith("manual")            ? "manual"
                : startsWith("interrupted")       ? "interrupted"
                : startsWith("guard")             ? "guard"
                : startsWith("break-even")        ? "break-even"
                : startsWith("market")            ? "market pause"
                : startsWith("risk")              ? "daily limit"
                : startsWith("locked")            ? "locked"
                : startsWith("audit")             ? "audit"
                                                  : "closed";
    closeReason_.clear();
    for (const auto& l : b.legs) {
        b.gross += l.gross;
        b.commission += l.commission;
        b.net += l.net;
    }
    log("[history] basket #" + std::to_string(b.number) + " saved: " + std::to_string(b.legs.size()) +
        " position(s), net " + std::to_string(b.net) + " (" + b.closeReason + ")");
    completedBaskets_.push_back(b);
    if (basketHandler_) {
        try {
            basketHandler_(b);
        } catch (const std::exception& ex) {
            log(std::string("[history] could not save basket: ") + ex.what());
        }
    }
}

LiveSettings LiveRunner::settings() const {
    std::scoped_lock lock(engineMutex_);
    return settingsFromConfig(config_);
}

void LiveRunner::applySettings(const LiveSettings& settings) {
    std::scoped_lock lock(engineMutex_);
    applySettingsToConfig(config_, settings);
    // Target, stop loss and auto target act at once, also on a running basket.
    if (engine_) engine_->updateExitRules(config_.strategy);
    marketDirty_ = true;
    marketWaitLogged_ = false;
    log("[settings] updated -- range width " + std::to_string(settings.rangeWidth) + ", lots " +
        std::to_string(settings.initialLot) + " +" + std::to_string(settings.lotIncrement) + ", target $" +
        std::to_string(settings.profitTarget) + (settings.autoContinue ? ", auto-continue on" : ", auto-continue off") +
        " (range and lots apply from the next range; target, stop loss and market hours now)");
}

void LiveRunner::updateMarket() {
    // Scanning ahead for the next change is the slow part -- done off the
    // lock, so quotes and fills are never held up by it.
    MarketHoursConfig cfg;
    bool full = false;
    MarketStatus previous;
    bool known = false;
    const int64_t now = utcNowMs();
    {
        std::scoped_lock lock(engineMutex_);
        cfg = config_.market;
        previous = marketStatus_;
        known = marketKnown_;
        full = marketDirty_ || !marketKnown_ || now - marketFullEvalMs_ >= 60'000 ||
               (marketStatus_.nextChangeUtcMs > 0 && now >= marketStatus_.nextChangeUtcMs);
        marketDirty_ = false;
    }
    std::vector<NewsEvent> feed;
    if (newsFeed_) {
        for (auto& e : newsFeed_->events()) {
            if (newsIsGuarded(e, cfg)) feed.push_back(std::move(e));
        }
    }
    MarketStatus st = evaluateMarket(now, cfg, &feed, full);
    if (!full) {
        st.nextChangeUtcMs = previous.nextChangeUtcMs;
        st.nextChangeLabel = previous.nextChangeLabel;
        st.hasNextNews = previous.hasNextNews;
        st.nextNews = previous.nextNews;
    }

    bool closeNow = false;
    std::string closeWhy;
    {
        std::scoped_lock lock(engineMutex_);
        if (full) marketFullEvalMs_ = now;
        marketStatus_ = st;
        marketKnown_ = true;
        if (known && previous.canOpenRange && !st.canOpenRange) {
            log("[market] " + st.label + " -- " + st.detail +
                (st.nextChangeUtcMs > 0 ? " New ranges wait until " + formatLocalDateTime(st.nextChangeUtcMs) + "." : ""));
        } else if (known && !previous.canOpenRange && st.canOpenRange) {
            log("[market] open again (" + st.sessionLabel + ") -- new ranges may start");
        } else if (known && previous.label != st.label && !st.canOpenRange) {
            log("[market] " + st.label + " -- " + st.detail);
        }
        if (st.canOpenRange) {
            closedForPause_ = false;
        } else if (!closedForPause_ && engine_ && tradeSession_) {
            // closeRangeAt: close a running range as the pause starts.
            const bool weekendAhead =
                st.phase == MarketPhase::Weekend ||
                (st.phase == MarketPhase::SessionPause && st.label.find("New York") != std::string::npos &&
                 newYorkWeekday(now) == 5);
            const bool wanted = cfg.closeRangeAt == "every" || (cfg.closeRangeAt == "weekend" && weekendAhead);
            const bool somethingOpen = engine_->status() == EngineStatus::Active || !openLegs_.empty() ||
                                       !placedOrderInfo_.empty() || !waitingOrders_.empty();
            if (wanted && somethingOpen) {
                closeNow = true;
                closeWhy = "market: " + st.label;
            }
            closedForPause_ = true;
        }
        if (closeNow) {
            log("[market] closing the running range: " + st.label + " (Settings > Market hours)");
            closeRangeLocked(true, closeWhy);
        }
    }
}

bool LiveRunner::marketBlocksLocked() {
    if (locked_) {
        if (!marketWaitLogged_) {
            log("[risk] no new range: a basket is locked -- close it (Close range) to go on");
            marketWaitLogged_ = true;
        }
        return true;
    }
    if (dailyLimitHit_) {
        if (!marketWaitLogged_) {
            log("[risk] no new range: today's loss limit was hit -- trading resumes tomorrow");
            marketWaitLogged_ = true;
        }
        return true;
    }
    if (!marketKnown_ || marketStatus_.canOpenRange) {
        marketWaitLogged_ = false;
        return false;
    }
    if (!marketWaitLogged_) {
        log("[market] no new range now: " + marketStatus_.label + " -- " + marketStatus_.detail);
        marketWaitLogged_ = true;
    }
    return true;
}

void LiveRunner::closePosition(OrderId id) {
    std::scoped_lock lock(engineMutex_);
    if (!tradeSession_) return;
    const auto it = std::find_if(openLegs_.begin(), openLegs_.end(), [id](const OpenLeg& l) { return l.id == id; });
    if (it == openLegs_.end()) return;
    log("[manual] operator requested: close position id=" + std::to_string(id));
    // Only names the basket's ending if no range is running -- otherwise the
    // basket carries on and ends however it ends.
    if (!basketRunningLocked()) setCloseReason("manual: close position");
    const OpenLeg leg = *it;
    openLegs_.erase(it);
    closeLegs({leg});
}

void LiveRunner::cancelOrder(OrderId id) {
    std::scoped_lock lock(engineMutex_);
    if (!tradeSession_) return;
    operatorCancelled_.insert(id);
    if (waitingOrders_.erase(id) > 0) {
        log("[manual] operator requested: cancel order id=" + std::to_string(id) + " (waiting, never sent)");
        markDirty();
        return;
    }
    const auto it = placedOrderInfo_.find(id);
    if (it == placedOrderInfo_.end()) return;
    log("[manual] operator requested: cancel order id=" + std::to_string(id));
    markDirty();
    const std::string cancelClOrdId = "c" + std::to_string(nextCancelReqId_++);
    cancelledOrderInfo_.insert({id, it->second});
    placedOrderInfo_.erase(it);
    tradeSession_->send(buildOrderCancelRequest(cancelClOrdId, std::to_string(id)));
}


// ---------------------------------------------------------------------------
// State persistence

void LiveRunner::requestOrderStatusLocked() {
    if (!tradeSession_) return;
    activeMassStatusReqId_ = "ord-" + std::to_string(nextMassStatusReqId_++);
    orderReports_.clear();
    if (connectSync_) log("[sync] requesting working orders from the broker");
    tradeSession_->send(buildOrderMassStatusRequest(activeMassStatusReqId_));
}

void LiveRunner::finishOrderSync(std::vector<ExecutionReportInfo> reports, bool known) {
    if (!connectSync_) {  // a check for unconfirmed orders while running
        activeMassStatusReqId_.clear();
        finishRuntimeOrderCheckLocked(reports);
        return;
    }
    connectOrders_ = std::move(reports);
    connectOrdersKnown_ = known;
    activeMassStatusReqId_.clear();
    {
        std::scoped_lock lock(posSyncMutex_);
        orderSyncDone_ = true;
    }
    posSyncCv_.notify_all();
}

void LiveRunner::loadSavedState() {
    if (stateFile_.empty()) return;
    std::optional<PersistedState> s;
    try {
        s = loadState(stateFile_);
    } catch (const std::exception& ex) {
        // Keep the unreadable file for inspection instead of overwriting it.
        std::filesystem::path bad = stateFile_;
        bad += ".unreadable";
        std::error_code ec;
        std::filesystem::rename(stateFile_, bad, ec);
        log(std::string("[state] could not read the saved state (") + ex.what() + ") -- moved it to " + bad.string() +
            " and starting without it");
        return;
    }
    if (!s) {
        log("[state] no saved state yet -- it will be kept in " + stateFile_.string());
        return;
    }
    if (s->account != accountKey(config_) || s->symbol != config_.connection.symbolName) {
        log("[state] the saved state belongs to " + s->account + " / " + s->symbol + " -- ignored");
        return;
    }
    log("[state] loaded the state saved at " + s->savedAt);
    std::scoped_lock lock(engineMutex_);
    loadedState_ = std::move(s);
}

void LiveRunner::reconcileOnConnectLocked() {
    const SymbolSpec& spec = config_.strategy.symbol;
    std::optional<PersistedState> saved = std::exchange(loadedState_, std::nullopt);

    auto toLeg = [](const PersistedLeg& p) {
        OpenLeg l;
        l.id = p.id;
        l.side = p.buy ? OrderSide::BuyStop : OrderSide::SellStop;
        l.volume = Volume{p.volumeSteps};
        l.price = Price{p.priceTicks};
        l.requestedPrice = Price{p.requestedPriceTicks};
        l.positionId = p.positionId;
        l.adopted = p.adopted;
        l.brokerEntryCommission = p.brokerEntryCommission;
        return l;
    };

    // Counters and lifetime statistics carry over whatever happens to the basket.
    if (saved) {
        basketCounter_ = std::max(basketCounter_, saved->basketCounter);
        nextCancelReqId_ = std::max<uint64_t>(nextCancelReqId_.load(), saved->nextCancelReqId);
        nextCloseReqId_ = std::max<uint64_t>(nextCloseReqId_.load(), saved->nextCloseReqId);
        nextAdoptedId_ = std::max<OrderId>(nextAdoptedId_, saved->nextAdoptedId);
        statsSince_ = saved->statsSince;
        dayKey_ = saved->dayKey;
        dayRealized_ = saved->dayRealized;
        dailyLimitHit_ = saved->dailyLimitHit;
        locked_ = saved->locked && !saved->openLegs.empty();
        lockedLoss_ = saved->lockedLoss;
        lockedAt_ = saved->lockedAt;
        if (!saved->openLegs.empty()) {
            guardLockOrderId_ = saved->guardLockOrderId;
            guardRangeLow_ = Price{saved->guardRangeLowTicks};
            guardRangeHigh_ = Price{saved->guardRangeHighTicks};
            escape_.buyStopId = saved->escapeBuyStopId;
            escape_.sellStopId = saved->escapeSellStopId;
            escape_.legId = saved->escapeLegId;
            escape_.buy = saved->escapeBuy;
            escape_.entry = Price{saved->escapeEntryTicks};
            escape_.trailing = saved->escapeTrailing;
            escape_.stop = saved->escapeStop;
            escape_.atr = saved->escapeAtr;
            escapeTries_ = saved->escapeTries;
            escapeAwaitingDecision_ = saved->escapeAwaitingDecision;
            escapeHeld_ = saved->escapeHeld;
            escapeExtraTries_ = saved->escapeExtraTries;
            lockedSince_ = std::chrono::steady_clock::now();  // read the market afresh after a restart
        }
        ticksProcessed_ += saved->ticksProcessed;
        tickDecisionUs_.carryOver(saved->tickDecisionUs);
        fillReactionUs_.carryOver(saved->fillReactionUs);
        brokerRoundTripMs_.carryOver(saved->brokerRoundTripMs);
    }
    if (statsSince_.empty()) statsSince_ = localTimestamp();

    // What the broker actually has on this symbol.
    struct BrokerPosition {
        std::string id;
        OrderSide side;
        int64_t steps;
        int64_t priceTicks;
        bool used = false;
    };
    std::vector<BrokerPosition> positions;
    for (const auto& r : connectPositions_) {
        if (!r.positionId || (r.symbolId && *r.symbolId != symbolId_)) continue;
        const bool buy = r.longQty > 0.0;
        const double qty = buy ? r.longQty : r.shortQty;
        if (qty <= 0.0) continue;
        positions.push_back(BrokerPosition{*r.positionId, buy ? OrderSide::BuyStop : OrderSide::SellStop,
                                           Volume::fromDouble(qty / kWireQtyPerLot, spec).steps,
                                           Price::fromDouble(r.settlPrice.value_or(0.0), spec).ticks});
    }
    struct BrokerOrder {
        OrderId id;
        OrderSide side;
        int64_t steps;
        int64_t priceTicks;
        bool used = false;
    };
    std::vector<BrokerOrder> orders;
    int foreignOrders = 0;
    OrderId maxSeenId = 0;
    for (const auto& r : connectOrders_) {
        if (r.symbolId && *r.symbolId != symbolId_) continue;
        if (r.ordStatus != OrdStatus::New && r.ordStatus != OrdStatus::PartiallyFilled &&
            r.ordStatus != OrdStatus::Unknown) {
            continue;
        }
        const auto id = tryParseOrderId(r.clOrdId);
        if (!id) {
            ++foreignOrders;  // not placed by this bot (e.g. by hand in cTrader) -- left alone
            continue;
        }
        maxSeenId = std::max(maxSeenId, *id);
        orders.push_back(BrokerOrder{*id, r.side == 2 ? OrderSide::SellStop : OrderSide::BuyStop,
                                     Volume::fromDouble(r.orderQty.value_or(0.0) / kWireQtyPerLot, spec).steps,
                                     Price::fromDouble(r.stopPx.value_or(0.0), spec).ticks});
    }
    auto findPosition = [&](const std::string& id) -> BrokerPosition* {
        for (auto& p : positions) {
            if (!p.used && !id.empty() && p.id == id) return &p;
        }
        return nullptr;
    };
    auto findOrder = [&](OrderId id) -> BrokerOrder* {
        for (auto& o : orders) {
            if (!o.used && o.id == id) return &o;
        }
        return nullptr;
    };
    // A pending order that's gone from the broker most likely filled: its
    // position has the same side and size and opened near the Stop price.
    const int64_t width = saved ? saved->engine.range.upperBound.ticks - saved->engine.range.lowerBound.ticks : 0;
    const int64_t tolerance = std::max<int64_t>(Price::fromDouble(1.0, spec).ticks, width);
    auto matchFill = [&](const OpenLeg& order) -> BrokerPosition* {
        BrokerPosition* best = nullptr;
        int64_t bestDistance = tolerance + 1;
        for (auto& p : positions) {
            if (p.used || p.side != order.side || p.steps != order.volume.steps) continue;
            const int64_t d = std::llabs(p.priceTicks - order.price.ticks);
            if (d <= tolerance && d < bestDistance) {
                best = &p;
                bestDistance = d;
            }
        }
        return best;
    };

    // Order ids (ClOrdIDs) start from a time-based number on every connect,
    // so two copies of the bot -- another PC/VPS on the same account, or an
    // older run -- never use the same ids. The broker's reports for the other
    // copy's orders are then never mistaken for this one's (unknown ids are
    // ignored), which small ids starting at 1 could not guarantee.
    const auto uniqueBase = static_cast<uint64_t>(std::time(nullptr)) * 1000;
    nextCloseReqId_ = std::max<uint64_t>(nextCloseReqId_.load(), uniqueBase);
    nextCancelReqId_ = std::max<uint64_t>(nextCancelReqId_.load(), uniqueBase);
    muuk::core::EngineState idle;
    idle.nextOrderId = std::max<OrderId>({saved ? saved->engine.nextOrderId : 1, maxSeenId + 1, uniqueBase});

    const bool brokerKnown = connectPositionsKnown_;
    bool consistent = brokerKnown;
    std::vector<std::string> problems;
    struct Fill {
        OrderId id;
        Price price;
        std::string positionId;
    };
    std::vector<Fill> fills;
    std::vector<OrderId> toReplace;  // pending orders gone from the broker without a fill
    std::vector<OpenLeg> reclose;
    std::vector<OrderId> stillToCancel;
    bool hadBasket = false;
    int basketNumber = 0;

    if (saved) {
        hadBasket = saved->engine.status != EngineStatus::Idle || !saved->openLegs.empty() ||
                    !saved->pendingOrders.empty() || !saved->pendingCloses.empty() || !saved->cancellingOrders.empty();
        currentBasket_ = saved->currentBasket;
        if (currentBasket_) {
            basketNumber = currentBasket_->number;
            currentBasketStart_ = std::chrono::steady_clock::now() -
                                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(secondsSinceStamp(currentBasket_->startedAt)));
        }
        closeReason_ = saved->closeReason;
        if (hadBasket && !brokerKnown) problems.push_back("the broker's open positions couldn't be checked");

        for (const auto& p : saved->openLegs) {
            const OpenLeg leg = toLeg(p);
            if (!brokerKnown) {
                openLegs_.push_back(leg);
            } else if (BrokerPosition* bp = findPosition(leg.positionId)) {
                bp->used = true;
                openLegs_.push_back(leg);
            } else {
                consistent = false;
                problems.push_back("position " + leg.positionId + " was closed outside the bot");
                closedPositionIds_.insert(leg.positionId);
                recordLegClosed(leg, 0.0, 0.0, 0.0, "closed outside the bot while it was disconnected");
            }
        }
        for (const auto& p : saved->pendingCloses) {
            const OpenLeg leg = toLeg(p);
            if (!brokerKnown) {
                openLegs_.push_back(leg);  // can't tell whether the close went through -- list it
            } else if (BrokerPosition* bp = findPosition(leg.positionId)) {
                bp->used = true;
                reclose.push_back(leg);  // the close never happened: do it now
            } else {
                closedPositionIds_.insert(leg.positionId);
                recordLegClosed(leg, 0.0, 0.0, 0.0, "closed while the bot was disconnected (exit price not reported)");
            }
        }
        for (const auto& p : saved->pendingOrders) {
            const OpenLeg leg = toLeg(p);
            placedOrderInfo_[leg.id] = leg;
            if (!brokerKnown) continue;
            if (BrokerOrder* bo = findOrder(leg.id)) {
                bo->used = true;  // still working -- nothing happened while away
            } else if (BrokerPosition* bp = matchFill(leg)) {
                bp->used = true;
                fills.push_back(Fill{leg.id, Price{bp->priceTicks}, bp->id});
            } else if (!connectOrdersKnown_) {
                consistent = false;
                problems.push_back("the broker's working orders couldn't be checked");
            } else {
                // Not working any more and no position for it: it was
                // cancelled (or expired). The basket still needs it -- place
                // it again once the engine is restored.
                placedOrderInfo_.erase(leg.id);
                toReplace.push_back(leg.id);
            }
        }
        for (const auto& p : saved->cancellingOrders) {
            const OpenLeg leg = toLeg(p);
            if (!brokerKnown) continue;
            if (BrokerOrder* bo = findOrder(leg.id)) {
                bo->used = true;
                cancelledOrderInfo_[leg.id] = leg;
                stillToCancel.push_back(leg.id);
            } else if (BrokerPosition* bp = matchFill(leg)) {
                bp->used = true;
                cancelledOrderInfo_[leg.id] = leg;
                fills.push_back(Fill{leg.id, Price{bp->priceTicks}, bp->id});  // surplus -- closed below
            }
        }
        if (saved->closeFailed && !openLegs_.empty()) basketCloseFailed_ = true;
    }
    const bool hadWaiting = saved && !saved->waitingOrders.empty();

    // Anything else open on this symbol isn't part of the saved basket:
    // list it so it can be closed/cancelled from here.
    for (auto& p : positions) {
        if (p.used || closedPositionIds_.count(p.id) > 0) continue;
        OpenLeg leg;
        leg.id = nextAdoptedId_++;
        leg.side = p.side;
        leg.volume = Volume{p.steps};
        leg.price = Price{p.priceTicks};
        leg.requestedPrice = leg.price;
        leg.positionId = p.id;
        leg.adopted = true;
        openLegs_.push_back(leg);
        log("[sync] found open " + std::string(p.side == OrderSide::BuyStop ? "BUY" : "SELL") + " " +
            std::to_string(leg.volume.toDouble(spec)) + " lot @ " + std::to_string(leg.price.toDouble(spec)) +
            " (position " + p.id + ")");
    }
    for (auto& o : orders) {
        if (o.used) continue;
        OpenLeg leg;
        leg.id = o.id;
        leg.side = o.side;
        leg.volume = Volume{o.steps};
        leg.price = Price{o.priceTicks};
        leg.requestedPrice = leg.price;
        leg.adopted = true;
        placedOrderInfo_[o.id] = leg;
        log("[sync] found a working " + std::string(o.side == OrderSide::BuyStop ? "Buy" : "Sell") + " Stop " +
            std::to_string(leg.volume.toDouble(spec)) + " lot @ " + std::to_string(leg.price.toDouble(spec)) +
            " (ClOrdID=" + std::to_string(o.id) + ") from an earlier run");
    }
    if (foreignOrders > 0) {
        log("[sync] " + std::to_string(foreignOrders) + " working order(s) not placed by the bot -- left alone");
    }

    const EngineStatus savedStatus = saved ? saved->engine.status : EngineStatus::Idle;
    const bool cycleWasRunning = savedStatus != EngineStatus::Idle;
    const bool resume = saved && cycleWasRunning && consistent;
    if (resume) {
        muuk::core::EngineState e = saved->engine;
        e.nextOrderId = std::max(e.nextOrderId, idle.nextOrderId);
        engine_->restoreState(e);
    } else {
        engine_->restoreState(idle);
    }

    for (const auto& f : fills) {
        log("[resume] order " + std::to_string(f.id) + " filled while the bot was disconnected (position " +
            f.positionId + " @ " + std::to_string(f.price.toDouble(spec)) + ")");
        if (resume) {
            applyFillLocked(f.id, f.price, f.positionId);  // exactly as if it had just filled
            continue;
        }
        // Not resuming: keep the position listed, but don't let the engine act on it.
        auto take = [&](std::unordered_map<OrderId, OpenLeg>& from) {
            const auto it = from.find(f.id);
            if (it == from.end()) return false;
            OpenLeg leg = it->second;
            from.erase(it);
            leg.price = f.price;
            leg.positionId = f.positionId;
            openLegs_.push_back(leg);
            recordLegOpened(leg);
            return true;
        };
        if (!take(placedOrderInfo_)) (void)take(cancelledOrderInfo_);
    }

    if (resume) {
        if (hadWaiting) {
            for (const auto& p : saved->waitingOrders) waitingOrders_[p.id] = toLeg(p);  // still held back
        }
        for (const OrderId id : toReplace) {
            noteMissedLocked();
            log("[resume] pending order " + std::to_string(id) + " is no longer at the broker -- placing it again");
            for (const auto& cmd : engine_->replaceOrder(id)) handleCommand(cmd);
        }
        // A running basket must always have its next order: if none is
        // pending (or waiting), arm the one after the most recent fill.
        if (engine_->status() == EngineStatus::Active && !engine_->seeding() && engine_->activeOrderCount() == 0 &&
            !openLegs_.empty()) {
            const auto last = std::max_element(openLegs_.begin(), openLegs_.end(),
                                               [](const OpenLeg& a, const OpenLeg& b) {
                                                   return a.adopted != b.adopted ? a.adopted : a.id < b.id;
                                               });
            log("[resume] the basket has no pending order -- placing its next one");
            for (const auto& cmd : engine_->armNextAfter(last->side)) handleCommand(cmd);
        }
    }

    if (!reclose.empty()) {
        log("[resume] " + std::to_string(reclose.size()) +
            " closing trade(s) never went through before the disconnect -- closing again");
        closeLegs(reclose);
    }
    for (const OrderId id : stillToCancel) {
        log("[resume] order " + std::to_string(id) + " was being cancelled -- cancelling again");
        tradeSession_->send(buildOrderCancelRequest("c" + std::to_string(nextCancelReqId_++), std::to_string(id)));
    }

    const std::string basketName = basketNumber > 0 ? "basket #" + std::to_string(basketNumber) : "the saved basket";
    if (resume) {
        resumeNote_ = "Resumed " + basketName + " where it left off: " + std::to_string(openLegs_.size()) +
                      " open position(s), " + std::to_string(placedOrderInfo_.size()) + " pending order(s)" +
                      (fills.empty() ? "" : ", " + std::to_string(fills.size()) + " fill(s) that happened while away");
        log("[resume] " + resumeNote_);
    } else if (hadBasket && !problems.empty()) {
        std::string why;
        for (std::size_t i = 0; i < problems.size(); ++i) why += (i ? "; " : "") + problems[i];
        resumeNote_ = "Could not safely resume " + basketName + " (" + why +
                      "). Its pending orders were cancelled and its open positions are listed -- close them to "
                      "start a new range.";
        log("[resume] " + resumeNote_);
        setCloseReason("interrupted: " + problems.front());
        holdAutoStart_ = true;
        if (!placedOrderInfo_.empty()) cancelPendingLocked();
    } else if (std::string why; !openLegs_.empty() && engine_->status() == EngineStatus::Idle &&
                                pendingCloses_.empty() && waitingOrders_.empty() &&
                                (!currentBasket_ || currentBasket_->legs.empty()) &&
                                std::all_of(openLegs_.begin(), openLegs_.end(),
                                            [](const OpenLeg& l) { return l.adopted; })) {
        // Positions at the broker that the bot has no basket for: if they
        // are one of its baskets (e.g. left by an older version, or the
        // state file was lost), pick it up and carry on.
        if (!adoptBasketFromBrokerLocked(why)) {
            resumeNote_ = std::to_string(openLegs_.size()) +
                          " position(s) left from before are still open at the broker (not picked up as a basket: " +
                          why + ") -- close them to start a new range.";
            log("[resume] " + resumeNote_);
        }
    } else if (hadBasket && !openLegs_.empty()) {
        const bool allOld = std::all_of(openLegs_.begin(), openLegs_.end(), [](const OpenLeg& l) { return l.adopted; });
        resumeNote_ = std::to_string(openLegs_.size()) + " position(s) " +
                      (allOld ? std::string("left from before") : "of " + basketName) +
                      " are still open at the broker -- close them to start a new range.";
        log("[resume] " + resumeNote_);
    }
    if (resume && !waitingOrders_.empty() && !resumeNote_.empty()) {
        resumeNote_ += ". Its next order is already past the market, so it waits for the price to come back "
                       "(or place it now at market from Pending orders).";
    }
    maybeFinalizeBasket();  // e.g. everything was closed while away
    markDirty();
}

bool LiveRunner::adoptBasketFromBrokerLocked(std::string& why) {
    const SymbolSpec& spec = config_.strategy.symbol;
    std::vector<OpenLeg> legs = openLegs_;
    std::sort(legs.begin(), legs.end(), [](const OpenLeg& a, const OpenLeg& b) {
        return a.volume.steps != b.volume.steps ? a.volume.steps < b.volume.steps : a.positionId < b.positionId;
    });

    // Lots must be exactly the sequence 1, 2, 3... of the current settings, with alternating sides.
    const StartSide startSide = config_.strategy.startSide;
    for (std::size_t k = 0; k < legs.size(); ++k) {
        const Volume expected = computeSequenceLot(config_.strategy.lotProgression, static_cast<int>(k) + 1, spec);
        if (legs[k].volume != expected) {
            why = "their lots aren't " + std::to_string(computeSequenceLot(config_.strategy.lotProgression, 1, spec)
                                                            .toDouble(spec)) +
                  ", " + std::to_string(computeSequenceLot(config_.strategy.lotProgression, 2, spec).toDouble(spec)) +
                  "... as the current lot settings would place them";
            return false;
        }
        if (k > 0 && legs[k].side == legs[k - 1].side) {
            why = "buys and sells don't alternate";
            return false;
        }
    }
    if ((startSide == StartSide::BuyOnly && legs.front().side != OrderSide::BuyStop) ||
        (startSide == StartSide::SellOnly && legs.front().side != OrderSide::SellStop)) {
        why = "the first position isn't on the side the settings start with";
        return false;
    }

    // The next order: opposite the last fill, next lot. A working order from
    // an earlier run that is exactly that one is kept; any other working
    // order means this isn't a basket the bot can take over cleanly.
    const OrderSide lastSide = legs.back().side;
    const OrderSide nextSide = lastSide == OrderSide::BuyStop ? OrderSide::SellStop : OrderSide::BuyStop;
    const Volume nextLot =
        computeSequenceLot(config_.strategy.lotProgression, static_cast<int>(legs.size()) + 1, spec);
    std::optional<OpenLeg> keptOrder;
    for (const auto& [id, order] : placedOrderInfo_) {
        if (!keptOrder && order.side == nextSide && order.volume == nextLot) {
            keptOrder = order;
        } else {
            why = "there are other working orders at the broker";
            return false;
        }
    }

    // The range hangs off the first fill: a Buy anchors the top, a Sell the bottom.
    const int64_t width = std::max<int64_t>(1, config_.strategy.range.upperBound.ticks - config_.strategy.range.lowerBound.ticks);
    const Price anchor = legs.front().price;
    const RangeConfig range = legs.front().side == OrderSide::BuyStop ? RangeConfig{Price{anchor.ticks - width}, anchor}
                                                                      : RangeConfig{anchor, Price{anchor.ticks + width}};

    muuk::core::EngineState e = engine_->exportState();
    e.status = EngineStatus::Active;
    e.range = range;
    e.pendingOrders.clear();
    e.buyVolumeSteps = e.buySumEntryPriceTicksVolume = e.sellVolumeSteps = e.sellSumEntryPriceTicksVolume = 0;
    e.slippageMoneyAccrued = 0.0;
    e.swapMoneyAccrued = 0.0;
    e.commissionAccrued = 0.0;
    e.seeding = false;
    for (auto& leg : legs) {
        const bool buy = leg.side == OrderSide::BuyStop;
        leg.requestedPrice = buy ? range.upperBound : range.lowerBound;
        leg.adopted = false;
        (buy ? e.buyVolumeSteps : e.sellVolumeSteps) += leg.volume.steps;
        (buy ? e.buySumEntryPriceTicksVolume : e.sellSumEntryPriceTicksVolume) +=
            leg.requestedPrice.ticks * leg.volume.steps;
        const int64_t slipTicks =
            buy ? leg.price.ticks - leg.requestedPrice.ticks : leg.requestedPrice.ticks - leg.price.ticks;
        e.slippageMoneyAccrued += static_cast<double>(slipTicks) * spec.tickValue * leg.volume.toDouble(spec);
        e.commissionAccrued += muuk::core::roundTurnCommissionMoney(leg.volume.toDouble(spec), leg.price.toDouble(spec),
                                                                    spec, config_.strategy.costs);
    }
    e.nextSequenceIndex = static_cast<int>(legs.size()) + 1;
    e.filledLevel = static_cast<int>(legs.size());
    if (keptOrder) {
        muuk::core::EngineState::Armed armed;
        armed.order = PendingOrder{keptOrder->id, e.nextSequenceIndex, nextSide, keptOrder->price, nextLot,
                                   OrderState::Pending};
        e.pendingOrders.push_back(armed);
        e.nextSequenceIndex += 1;
        e.nextOrderId = std::max<OrderId>(e.nextOrderId, keptOrder->id + 1);
        placedOrderInfo_[keptOrder->id].adopted = false;
    }
    engine_->restoreState(e);

    openLegs_ = legs;
    closeReason_.clear();
    beginBasket();
    currentBasket_->startedAt.clear();  // unknown -- opened before the bot connected
    for (const auto& leg : legs) {
        recordLegOpened(leg);
        currentBasket_->legs.back().adopted = true;
        currentBasket_->legs.back().openedAt.clear();
        currentBasket_->legs.back().note = "opened before the bot connected";
    }
    if (!keptOrder) {
        for (const auto& cmd : engine_->armNextAfter(lastSide)) handleCommand(cmd);
    }

    const double lastLots = legs.back().volume.toDouble(spec);
    std::ostringstream note;
    note.setf(std::ios::fixed);
    note.precision(2);
    note << "Picked up the basket already open at the broker: " << legs.size() << " positions ("
         << legs.front().volume.toDouble(spec) << " to " << lastLots << " lot), range "
         << range.lowerBound.toDouble(spec) << " - " << range.upperBound.toDouble(spec) << "; next order "
         << (nextSide == OrderSide::BuyStop ? "Buy" : "Sell") << " Stop " << nextLot.toDouble(spec) << " @ "
         << (nextSide == OrderSide::BuyStop ? range.upperBound : range.lowerBound).toDouble(spec);
    if (keptOrder) {
        note << " (already working)";
    } else if (!waitingOrders_.empty()) {
        note << " is already past the market, so it waits for the price to come back (or place it now at market "
                "from Pending orders)";
    }
    resumeNote_ = note.str();
    log("[resume] " + resumeNote_);
    return true;
}

PersistedState LiveRunner::buildStateLocked() const {
    auto toPersisted = [](const OpenLeg& l, const std::string& closeClOrdId = {}) {
        PersistedLeg p;
        p.id = l.id;
        p.buy = l.side == OrderSide::BuyStop;
        p.volumeSteps = l.volume.steps;
        p.priceTicks = l.price.ticks;
        p.requestedPriceTicks = l.requestedPrice.ticks;
        p.positionId = l.positionId;
        p.adopted = l.adopted;
        p.closeClOrdId = closeClOrdId;
        p.brokerEntryCommission = l.brokerEntryCommission;
        return p;
    };
    auto sortedIds = [](const std::unordered_map<OrderId, OpenLeg>& m) {
        std::vector<OrderId> ids;
        for (const auto& [id, leg] : m) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        return ids;
    };

    PersistedState s;
    s.account = accountKey(config_);
    s.symbol = config_.connection.symbolName;
    s.savedAt = localTimestamp();
    if (engine_) s.engine = engine_->exportState();
    for (const auto& l : openLegs_) s.openLegs.push_back(toPersisted(l));
    for (const OrderId id : sortedIds(placedOrderInfo_)) s.pendingOrders.push_back(toPersisted(placedOrderInfo_.at(id)));
    for (const OrderId id : sortedIds(cancelledOrderInfo_)) {
        s.cancellingOrders.push_back(toPersisted(cancelledOrderInfo_.at(id)));
    }
    for (const auto& [clOrdId, leg] : pendingCloses_) s.pendingCloses.push_back(toPersisted(leg, clOrdId));
    for (const OrderId id : sortedIds(waitingOrders_)) s.waitingOrders.push_back(toPersisted(waitingOrders_.at(id)));
    std::sort(s.pendingCloses.begin(), s.pendingCloses.end(),
              [](const PersistedLeg& a, const PersistedLeg& b) { return a.closeClOrdId < b.closeClOrdId; });
    s.closeFailed = basketCloseFailed_;
    s.currentBasket = currentBasket_;
    s.closeReason = closeReason_;
    s.basketCounter = basketCounter_;
    s.nextCancelReqId = nextCancelReqId_.load();
    s.nextCloseReqId = nextCloseReqId_.load();
    s.nextAdoptedId = nextAdoptedId_;
    s.statsSince = statsSince_;
    s.dayKey = dayKey_;
    s.dayRealized = dayRealized_;
    s.dailyLimitHit = dailyLimitHit_;
    s.locked = locked_;
    s.lockedLoss = lockedLoss_;
    s.lockedAt = lockedAt_;
    s.guardLockOrderId = guardLockOrderId_;
    s.guardRangeLowTicks = guardRangeLow_.ticks;
    s.guardRangeHighTicks = guardRangeHigh_.ticks;
    s.escapeBuyStopId = escape_.buyStopId;
    s.escapeSellStopId = escape_.sellStopId;
    s.escapeLegId = escape_.legId;
    s.escapeBuy = escape_.buy;
    s.escapeEntryTicks = escape_.entry.ticks;
    s.escapeTrailing = escape_.trailing;
    s.escapeTries = escapeTries_;
    s.escapeStop = escape_.stop;
    s.escapeAtr = escape_.atr;
    s.escapeAwaitingDecision = escapeAwaitingDecision_;
    s.escapeHeld = escapeHeld_;
    s.escapeExtraTries = escapeExtraTries_;
    s.ticksProcessed = ticksProcessed_;
    s.tickDecisionUs = tickDecisionUs_.totals();
    s.fillReactionUs = fillReactionUs_.totals();
    s.brokerRoundTripMs = brokerRoundTripMs_.totals();
    return s;
}

void LiveRunner::flushState() {
    if (stateFile_.empty()) return;
    PersistedState state;
    uint64_t seq = 0;
    {
        std::scoped_lock lock(engineMutex_);
        if (!stateReady_ || !stateDirty_.exchange(false)) return;
        state = buildStateLocked();
        seq = ++stateSeq_;
    }
    std::scoped_lock lock(stateWriteMutex_);
    if (seq <= lastWrittenSeq_) return;  // a newer state was already written
    try {
        saveState(stateFile_, state);
        lastWrittenSeq_ = seq;
    } catch (const std::exception& ex) {
        stateDirty_ = true;  // try again next time
        log(std::string("[state] could not save: ") + ex.what());
    }
}

void LiveRunner::resetStats() {
    {
        std::scoped_lock lock(engineMutex_);
        completedBaskets_.clear();
        basketsClosed_ = 0;
        bookedGross_ = 0.0;
        bookedCommission_ = 0.0;
        tickDecisionUs_.reset();
        fillReactionUs_.reset();
        brokerRoundTripMs_.reset();
        ticksProcessed_ = 0;
        basketCounter_ = 0;
        statsSince_ = localTimestamp();
        resumeNote_.clear();
        dayRealized_ = 0.0;
        dailyLimitHit_ = false;
        auditLog_.clear();
        markDirty();
        log("[reset] statistics and basket numbering start over from now");
    }
    flushState();
}

// ---------------------------------------------------------------------------
// Audit

namespace {
enum AuditIndex {
    kAuditPrices,
    kAuditBroker,
    kAuditPending,
    kAuditCloses,
    kAuditTarget,
    kAuditEngine,
    kAuditDaily,
    kAuditRisk,
    kAuditLock,
    kAuditCount
};
const char* const kAuditNames[kAuditCount] = {"Prices from the broker",   "Positions match the broker",
                                              "Pending order in place",   "Closing orders",
                                              "Profit target / stop loss", "Bot vs its positions",
                                              "Daily loss limit",         "Risk settings",
                                              "Lock & pause"};

std::string usd(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s$%.2f", v < 0.0 ? "-" : "", v < 0.0 ? -v : v);
    return buf;
}
std::string priceText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}
std::string lotsText(int64_t steps, const SymbolSpec& spec) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(steps) * spec.volumeStep);
    return buf;
}
}  // namespace

void LiveRunner::auditEventLocked(int check, int level, const std::string& text) {
    if (check >= 0 && check < kAuditCount) {
        if (lastAuditText_[check] == text) return;  // a lasting problem is reported once
        lastAuditText_[check] = text;
    }
    const std::string line = std::string(check >= 0 && check < kAuditCount ? kAuditNames[check] : "Audit") + ": " + text;
    auditLog_.push_back(AuditEvent{localTimestamp().substr(11), level, line});
    while (auditLog_.size() > 60) auditLog_.pop_front();
    log(std::string(level >= 2 ? "[audit] NEEDS ATTENTION -- " : "[audit] ") + line);
}

void LiveRunner::addRealizedLocked(double net) {
    const std::string today = localTimestamp().substr(0, 10);
    if (dayKey_ != today) {
        dayKey_ = today;
        dayRealized_ = 0.0;
        dailyLimitHit_ = false;
    }
    dayRealized_ += net;
    markDirty();
}

void LiveRunner::auditLocked() {
    if (!engine_) return;
    const auto now = std::chrono::steady_clock::now();
    const SymbolSpec& spec = config_.strategy.symbol;
    audit_.assign(kAuditCount, AuditCheck{});
    for (int i = 0; i < kAuditCount; ++i) audit_[i].name = kAuditNames[i];
    auto set = [&](int i, int level, const std::string& detail, bool event) {
        audit_[i].level = level;
        audit_[i].detail = detail;
        if (level == 0) {
            lastAuditText_[i].clear();
        } else if (event) {
            auditEventLocked(i, level, detail);
        }
    };
    const bool haveQuote = latestBid_ && latestAsk_;
    PnlBreakdown live;
    if (haveQuote) {
        for (const auto& leg : openLegs_) live += legPnl(leg);
    }
    const bool active = basketRunningLocked();

    // 1. Prices: a feed that stopped while the market is open is reconnected.
    {
        const bool marketOpen = !marketKnown_ || marketStatus_.marketOpen;
        if (marketOpen && !wasMarketOpen_) marketOpenSince_ = now;
        wasMarketOpen_ = marketOpen;
        const int stale = config_.connection.quoteStaleSeconds;
        const bool anyQuote = lastQuoteAt_.time_since_epoch().count() != 0;
        const auto age = anyQuote ? std::chrono::duration_cast<std::chrono::seconds>(now - lastQuoteAt_).count() : -1;
        if (!marketOpen) {
            set(kAuditPrices, 0, "market closed -- no prices expected", false);
        } else if (!anyQuote) {
            set(kAuditPrices, 1, "no price received yet", false);
        } else if (stale > 0 && age > stale && now - marketOpenSince_ > std::chrono::seconds(stale)) {
            if (now - lastStaleReconnect_ > std::chrono::minutes(10) || lastStaleReconnect_.time_since_epoch().count() == 0) {
                lastStaleReconnect_ = now;
                set(kAuditPrices, 2,
                    "no price for " + std::to_string(age) + " s while the market is open -- the price feed looks dead, "
                    "reconnecting",
                    true);
                {
                    std::scoped_lock infoLock(infoMutex_);
                    disconnectReason_ = "audit: no prices for " + std::to_string(age) + " s";
                }
                running_ = false;  // run() ends; the app / muuk-live reconnect by themselves
            } else {
                set(kAuditPrices, 2,
                    "no price for " + std::to_string(age) + " s -- already reconnected in the last 10 min; check the "
                    "internet / broker",
                    true);
            }
        } else {
            set(kAuditPrices, 0, "last price " + std::to_string(std::max<long long>(0, age)) + " s ago", false);
        }
    }

    // 2. Positions and orders vs the broker's own lists (every verifySeconds).
    {
        int recent = 0;
        std::string last;
        for (const auto& [at, issue] : brokerIssues_) {
            if (now - at < std::chrono::minutes(5)) {
                ++recent;
                last = issue;
            }
        }
        if (brokerCheckedAt_.empty()) {
            set(kAuditBroker, 0, "first check running", false);
        } else if (recent > 0) {
            set(kAuditBroker, 1, std::to_string(recent) + " difference(s) in the last 5 min, handled: " + last, false);
        } else {
            set(kAuditBroker, 0, "matched at " + brokerCheckedAt_.substr(11), false);
        }
    }

    // 3. The running range always has its next pending order.
    {
        const std::size_t pending = placedOrderInfo_.size() + waitingOrders_.size();
        if (!active) {
            set(kAuditPending, 0, "no range running", false);
        } else if (engine_->status() == EngineStatus::HaltedByRisk) {
            set(kAuditPending, 1,
                "a limit (max orders / max lot / max basket loss) stops new levels -- the open positions close at "
                "the target or the stop loss",
                false);
        } else if (engine_->seeding()) {
            set(kAuditPending, 0, "entry orders out", false);
        } else if (pending == 0 && config_.strategy.reArmOnFill && pendingCloses_.empty()) {
            set(kAuditPending, healthRearms_ < 3 ? 1 : 2,
                healthRearms_ < 3 ? "no pending order -- placing the next one"
                                  : "no pending order and re-placing it failed 3 times -- press Close range / Open range",
                true);
        } else if (pending > 0) {
            const auto& o = !placedOrderInfo_.empty() ? placedOrderInfo_.begin()->second : waitingOrders_.begin()->second;
            set(kAuditPending, 0,
                std::string(o.side == OrderSide::BuyStop ? "Buy" : "Sell") + " Stop " + lotsText(o.volume.steps, spec) +
                    " @ " + priceText(o.price.toDouble(spec)) +
                    (placedOrderInfo_.empty() ? " (held by the bot)" : (o.confirmed ? " (at the broker)" : " (sent)")),
                false);
        } else {
            set(kAuditPending, 0, "none needed", false);
        }
    }

    // 4. Closing orders: every one is confirmed, or settled with the broker.
    {
        if (basketCloseFailed_) {
            set(kAuditCloses, 2,
                "a position couldn't be closed -- close it in cTrader, then press Sync from broker (Positions tab)", true);
        } else if (!pendingCloses_.empty()) {
            const auto confirm = std::chrono::seconds(std::max(1, config_.connection.closeConfirmSeconds));
            long long oldest = 0;
            for (const auto& [clOrdId, leg] : pendingCloses_) {
                const auto sent = closeSentAt_.find(clOrdId);
                if (sent == closeSentAt_.end()) {
                    closeRecheck_.insert(clOrdId);  // e.g. resumed from the state file
                    auditSyncWanted_ = true;
                    continue;
                }
                oldest = std::max<long long>(oldest, std::chrono::duration_cast<std::chrono::seconds>(now - sent->second).count());
                if (now - sent->second > confirm) {
                    closeRecheck_.insert(clOrdId);
                    auditSyncWanted_ = true;
                }
            }
            if (oldest > config_.connection.closeConfirmSeconds) {
                set(kAuditCloses, 1,
                    std::to_string(pendingCloses_.size()) + " closing order(s) not confirmed after " +
                        std::to_string(oldest) + " s -- checking with the broker",
                    true);
            } else {
                set(kAuditCloses, 0, std::to_string(pendingCloses_.size()) + " closing, waiting for the fills", false);
            }
        } else {
            set(kAuditCloses, 0, "nothing closing", false);
        }
    }

    // 5. Target / stop loss reached must mean the basket closes.
    {
        const muuk::core::BasketPnlResult pnl = engine_->lastBasketPnl();
        const double target = engine_->effectiveTargetMoney(pnl);
        const double stop = config_.strategy.basketStopLossMoney;
        if (!active || openLegs_.empty() || !pendingCloses_.empty() || !haveQuote) {
            set(kAuditTarget, 0, active ? "watching" : "no range running", false);
        } else if ((target > 0.0 && pnl.netPnlMoney >= target) || (stop > 0.0 && pnl.netPnlMoney <= -stop)) {
            // The engine closes on the tick that reaches it; if that didn't
            // happen, give it the live price again, then close by hand.
            for (const auto& cmd : engine_->onTick(latestTick())) handleCommand(cmd);
            if (basketRunningLocked()) {
                closeRangeLocked(true, "audit: target / stop loss reached but the basket didn't close");
            }
            set(kAuditTarget, 1, "net " + usd(pnl.netPnlMoney) + " reached the target / stop loss but was still open -- closed it", true);
        } else {
            set(kAuditTarget, 0,
                "net " + usd(pnl.netPnlMoney) + ", target " + usd(target) + (stop > 0.0 ? ", stop -" + usd(stop).substr(1) : ", no stop loss"),
                false);
        }
    }

    // 6. The range's own count of lots vs its positions at the broker.
    {
        if (!active || !pendingCloses_.empty() || engine_->seeding()) {
            set(kAuditEngine, 0, active ? "watching" : "no range running", false);
        } else {
            const muuk::core::EngineState st = engine_->exportState();
            int64_t buy = 0;
            int64_t sell = 0;
            for (const auto& leg : openLegs_) {
                if (leg.adopted) continue;
                (leg.side == OrderSide::BuyStop ? buy : sell) += leg.volume.steps;
            }
            if (buy != st.buyVolumeSteps || sell != st.sellVolumeSteps) {
                set(kAuditEngine, 1,
                    "the range counts buy " + lotsText(st.buyVolumeSteps, spec) + " / sell " +
                        lotsText(st.sellVolumeSteps, spec) + " lots, its positions are buy " + lotsText(buy, spec) +
                        " / sell " + lotsText(sell, spec) + " -- a position was closed by hand or partly filled",
                    true);
            } else {
                set(kAuditEngine, 0, "buy " + lotsText(buy, spec) + " / sell " + lotsText(sell, spec) + " lots, matching", false);
            }
        }
    }

    // 7. Daily loss limit: booked today + floating.
    {
        const std::string today = localTimestamp().substr(0, 10);
        if (dayKey_ != today) {
            if (dailyLimitHit_) log("[risk] a new day -- the daily loss limit is reset");
            dayKey_ = today;
            dayRealized_ = 0.0;
            dailyLimitHit_ = false;
            markDirty();
        }
        const double limit = config_.connection.dailyLossLimitMoney;
        const double todayNet = dayRealized_ + live.net;
        if (limit <= 0.0) {
            set(kAuditDaily, 0, "off -- today " + usd(todayNet), false);
        } else if (dailyLimitHit_) {
            set(kAuditDaily, 2, "hit today (" + usd(todayNet) + ") -- no new range until tomorrow", true);
        } else if (todayNet <= -limit) {
            dailyLimitHit_ = true;
            markDirty();
            set(kAuditDaily, 2,
                "today's net " + usd(todayNet) + " reached the limit -" + usd(limit).substr(1) +
                    " -- closing everything; no new range until tomorrow",
                true);
            const bool somethingOpen =
                active || !openLegs_.empty() || !placedOrderInfo_.empty() || !waitingOrders_.empty();
            if (somethingOpen && tradeSession_) closeRangeLocked(true, "risk: daily loss limit");
        } else {
            set(kAuditDaily, 0, "today " + usd(todayNet) + " of -" + usd(limit).substr(1), false);
        }
    }

    // 8. Risk settings that leave losses open-ended.
    {
        std::string warn;
        if (config_.strategy.basketStopLossMoney <= 0.0 && config_.strategy.lotProgression.multiplier > 1.0) {
            warn = "no stop loss while lots multiply";
        } else if (config_.strategy.basketStopLossMoney <= 0.0) {
            warn = "no stop loss";
        }
        if (config_.connection.dailyLossLimitMoney <= 0.0) warn += std::string(warn.empty() ? "" : "; ") + "no daily loss limit";
        // Margin the next fill would need vs the typed-in balance (FIX sends neither).
        const double balance = config_.connection.accountBalance;
        if (balance > 0.0 && active && haveQuote) {
            int64_t buy = 0;
            int64_t sell = 0;
            for (const auto& leg : openLegs_) (leg.side == OrderSide::BuyStop ? buy : sell) += leg.volume.steps;
            for (const auto& [id, o] : placedOrderInfo_) (o.side == OrderSide::BuyStop ? buy : sell) += o.volume.steps;
            const double price = (*latestBid_ + *latestAsk_) / 2.0;
            const double margin = static_cast<double>(std::max(buy, sell)) * spec.volumeStep * spec.contractSize * price /
                                  std::max(1.0, config_.connection.leverage);
            const double equity = balance + dayRealized_ + live.net;
            if (margin > equity * 0.8) {
                warn += std::string(warn.empty() ? "" : "; ") + "the next fill needs " + usd(margin) +
                        " margin, equity is about " + usd(equity) + " -- stop-out risk";
            }
        }
        if (warn.find("stop-out") != std::string::npos) {
            set(kAuditRisk, 2, warn, true);
        } else if (warn.empty()) {
            set(kAuditRisk, 0, "stop loss " + usd(config_.strategy.basketStopLossMoney) + ", daily limit " +
                                   usd(config_.connection.dailyLossLimitMoney),
                false);
        } else {
            set(kAuditRisk, 1, warn + " (Settings)", true);
        }
    }

    // 9. Lock & pause.
    {
        if ((locked_ || guardLockOrderId_ != 0 || escapeTries_ > 0) && openLegs_.empty() && pendingCloses_.empty() &&
            placedOrderInfo_.empty() && managedMarket_.empty()) {
            if (locked_) log("[risk] the locked basket is closed -- the bot can go on");
            resetRecoveryLocked();
        }
        // Orders of the guard / escape that the bot no longer has (filled or
        // cancelled while it was away): settle them.
        auto known = [&](OrderId id) {
            return placedOrderInfo_.count(id) > 0 || waitingOrders_.count(id) > 0;
        };
        auto isOpenLeg = [&](OrderId id) {
            return std::any_of(openLegs_.begin(), openLegs_.end(), [&](const OpenLeg& l) { return l.id == id; });
        };
        if (guardLockOrderId_ != 0 && !known(guardLockOrderId_) && pendingCloses_.empty()) {
            if (isOpenLeg(guardLockOrderId_)) {
                onRecoveryFillLocked(guardLockOrderId_);
            } else if (!openLegs_.empty() && haveQuote) {
                guardLockOrderId_ = 0;
                lockNowLocked("the lock stop order is gone from the broker", live.net);
            } else {
                guardLockOrderId_ = 0;
                markDirty();
            }
        }
        for (OrderId* id : {&escape_.buyStopId, &escape_.sellStopId}) {
            if (*id != 0 && !known(*id)) {
                if (isOpenLeg(*id)) {
                    onRecoveryFillLocked(*id);
                } else {
                    *id = 0;
                    markDirty();
                }
            }
        }
        retryManagedMarketLocked();
        maybeGuardLocked();  // also when no quote arrives (e.g. right after a lock filled)
        const double lockAt = lockAtLossMoneyLocked();
        const RecoveryConfig& rc = config_.connection.recovery;
        if (locked_) {
            std::string how = " (swap is charged every night) -- close it when you decide (Close range)";
            if (rc.escapeEnabled) {
                how = escapePhase_.empty() ? std::string(" -- escape: waiting for a balanced book")
                                           : " -- escape: " + escapePhase_;
            }
            set(kAuditLock, 2,
                "locked since " + (lockedAt_.size() > 11 ? lockedAt_.substr(11) : lockedAt_) + " at " + usd(lockedLoss_) +
                    ", now " + usd(live.net) + how,
                false);
        } else if (guardLockOrderId_ != 0) {
            set(kAuditLock, 1,
                "margin guard: the next level was held back, a lock stop waits at the line; closes at " +
                    usd(breakEvenMoneyLocked()) + " if the price turns back",
                false);
        } else if (lockAt <= 0.0 && !guardsOnLocked()) {
            set(kAuditLock, 0, "off -- set the balance in Settings for the margin guards", false);
        } else {
            std::string text;
            if (guardsOnLocked()) {
                text = "margin guards " + pctText(rc.guardLockMarginLevel) + " / " + pctText(rc.guardStopMarginLevel);
                if (haveQuote && !openLegs_.empty()) {
                    const BookLots book = bookLotsLocked();
                    const double mid = (*latestBid_ + *latestAsk_) / 2.0;
                    const double margin = bookMargin(book.buys, book.sells, mid, spec.contractSize, marginRuleLocked());
                    if (margin > 0.0) text += ", margin level now " + pctText(marginLevelPercent(equityEstimateLocked(live.net), margin));
                }
            }
            if (lockAt > 0.0) text += std::string(text.empty() ? "" : "; ") + "locks at -" + usd(lockAt).substr(1);
            set(kAuditLock, 0, text, false);
        }
    }
}

}  // namespace muuk::ctrader_fix
