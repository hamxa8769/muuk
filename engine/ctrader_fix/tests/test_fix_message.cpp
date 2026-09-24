#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/fix_message.hpp"
#include "muuk/ctrader_fix/messages.hpp"

using namespace muuk::ctrader_fix;

namespace {
// Replaces the '|' placeholder (used for readability) with a real SOH byte,
// matching how the cTrader FIX spec's own worked examples are written.
std::string soh(std::string s) {
    for (char& c : s) {
        if (c == '|') c = kSOH;
    }
    return s;
}
}  // namespace

TEST_CASE("encode() computes a correct, checksum-verifiable Logon message") {
    // Field content from "cTrader FIX Engine, Rules of Engagement" v2.18.1,
    // section 6.1. That section's own printed BodyLength(9)/CheckSum(10)
    // values (126/131) don't actually match its field content by hand
    // count (summing each "tag=value<SOH>" segment's length gives a
    // 131-byte body, not 126) — likely a transcription error in the doc.
    // The values asserted below are independently hand-computed from the
    // field content itself (see the hand-verified Heartbeat case below for
    // a fully worked, small example of the same checksum algorithm), not
    // copied from the spec, so this test is checking correctness rather
    // than byte-for-byte fidelity to a possibly-wrong example.
    FixMessage msg("A");
    msg.set(49, std::string("live.theBroker.12345"));
    msg.set(56, std::string("CSERVER"));
    msg.set(34, int64_t{1});
    msg.set(52, std::string("20170117-08:03:04"));
    msg.set(57, std::string("TRADE"));
    msg.set(50, std::string("any_string"));
    msg.set(98, int64_t{0});
    msg.set(108, int64_t{30});
    msg.set(141, std::string("Y"));
    msg.set(553, std::string("12345"));
    msg.set(554, std::string("passw0rd!"));

    const std::string expected =
        soh("8=FIX.4.4|9=131|35=A|49=live.theBroker.12345|56=CSERVER|34=1|52=20170117-"
            "08:03:04|57=TRADE|50=any_string|98=0|108=30|141=Y|553=12345|554=passw0rd!|10=093|");

    REQUIRE(msg.encode() == expected);
}

TEST_CASE("encode() matches a fully hand-computed checksum for a minimal message") {
    // FixMessage("0") with no extra fields encodes to exactly:
    //   "8=FIX.4.4" SOH "9=5" SOH "35=0" SOH "10=163" SOH
    // CheckSum is the sum of the ASCII values of every byte up to and
    // including the SOH before "10=", mod 256. Summing
    // "8=FIX.4.4"+SOH+"9=5"+SOH+"35=0"+SOH byte-by-byte
    // (56+61+70+73+88+46+52+46+52+1 + 57+61+53+1 + 51+53+61+48+1 = 931;
    // 931 mod 256 = 163) gives 163, independent of this codebase's own
    // checksum implementation.
    const FixMessage msg("0");
    const std::string expected = soh("8=FIX.4.4|9=5|35=0|10=163|");
    REQUIRE(msg.encode() == expected);
}

TEST_CASE("tryDecode() parses the spec's Logon example field content") {
    const std::string raw =
        soh("8=FIX.4.4|9=131|35=A|49=live.theBroker.12345|56=CSERVER|34=1|52=20170117-"
            "08:03:04|57=TRADE|50=any_string|98=0|108=30|141=Y|553=12345|554=passw0rd!|10=093|");

    FixMessage msg("");
    std::size_t consumed = 0;
    REQUIRE(FixMessage::tryDecode(raw, msg, consumed));
    REQUIRE(consumed == raw.size());
    REQUIRE(msg.msgType() == "A");
    REQUIRE(msg.getString(49) == "live.theBroker.12345");
    REQUIRE(msg.getString(56) == "CSERVER");
    REQUIRE(msg.getInt(34) == 1);
    REQUIRE(msg.getString(57) == "TRADE");
    REQUIRE(msg.getInt(98) == 0);
    REQUIRE(msg.getInt(108) == 30);
    REQUIRE(msg.getString(141) == "Y");
    REQUIRE(msg.getString(553) == "12345");
    REQUIRE(msg.getString(554) == "passw0rd!");
}

TEST_CASE("tryDecode() round-trips an encoded message") {
    FixMessage original("D");
    original.set(11, std::string("42"));
    original.set(55, std::string("1"));
    original.set(54, int64_t{1});

    const std::string wire = original.encode();
    FixMessage decoded("");
    std::size_t consumed = 0;
    REQUIRE(FixMessage::tryDecode(wire, decoded, consumed));
    REQUIRE(consumed == wire.size());
    REQUIRE(decoded.msgType() == "D");
    REQUIRE(decoded.getString(11) == "42");
    REQUIRE(decoded.getString(55) == "1");
    REQUIRE(decoded.getInt(54) == 1);
}

TEST_CASE("tryDecode() returns false on an incomplete buffer and doesn't consume anything") {
    FixMessage full("0");
    full.set(112, std::string("abc"));
    const std::string wire = full.encode();

    for (std::size_t cut = 1; cut < wire.size(); ++cut) {
        FixMessage decoded("");
        std::size_t consumed = 999;
        REQUIRE_FALSE(FixMessage::tryDecode(wire.substr(0, cut), decoded, consumed));
    }
}

TEST_CASE("tryDecode() consumes exactly one message from a buffer holding two concatenated messages") {
    FixMessage first("0");
    FixMessage second("1");
    second.set(112, std::string("hb-1"));

    const std::string wire = first.encode() + second.encode();

    FixMessage decoded1("");
    std::size_t consumed1 = 0;
    REQUIRE(FixMessage::tryDecode(wire, decoded1, consumed1));
    REQUIRE(decoded1.msgType() == "0");
    REQUIRE(consumed1 == first.encode().size());

    FixMessage decoded2("");
    std::size_t consumed2 = 0;
    REQUIRE(FixMessage::tryDecode(wire.substr(consumed1), decoded2, consumed2));
    REQUIRE(decoded2.msgType() == "1");
    REQUIRE(decoded2.getString(112) == "hb-1");
}

TEST_CASE("tryDecode() throws on a corrupted checksum") {
    FixMessage msg("0");
    std::string wire = msg.encode();
    // Flip the last checksum digit before its terminating SOH.
    wire[wire.size() - 2] = (wire[wire.size() - 2] == '9') ? '0' : '9';

    FixMessage decoded("");
    std::size_t consumed = 0;
    REQUIRE_THROWS_AS(FixMessage::tryDecode(wire, decoded, consumed), FixMessageError);
}

TEST_CASE("getAll() preserves insertion order for repeating fields") {
    FixMessage msg("W");
    msg.set(269, std::string("0"));
    msg.set(270, std::string("1.1"));
    msg.set(269, std::string("1"));
    msg.set(270, std::string("1.2"));

    const auto types = msg.getAll(269);
    const auto prices = msg.getAll(270);
    REQUIRE(types == std::vector<std::string>{"0", "1"});
    REQUIRE(prices == std::vector<std::string>{"1.1", "1.2"});
}

TEST_CASE("getString() throws for a missing tag; getOptionalString() returns nullopt") {
    FixMessage msg("0");
    REQUIRE_THROWS_AS(msg.getString(999), FixMessageError);
    REQUIRE_FALSE(msg.getOptionalString(999).has_value());
}

TEST_CASE("parseUtcTimestampMs reads FIX UTC timestamps, with or without milliseconds") {
    using muuk::ctrader_fix::parseUtcTimestampMs;
    REQUIRE(parseUtcTimestampMs("19700101-00:00:00").value() == 0);
    REQUIRE(parseUtcTimestampMs("19700101-00:00:01.250").value() == 1250);
    // 2026-09-24 14:07:30.5 UTC
    REQUIRE(parseUtcTimestampMs("20260924-14:07:30.500").value() == 1790258850500LL);
    REQUIRE_FALSE(parseUtcTimestampMs("garbage").has_value());
    REQUIRE_FALSE(parseUtcTimestampMs("20261324-14:07:30").has_value());
}
