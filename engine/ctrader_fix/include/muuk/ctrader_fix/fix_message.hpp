#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace muuk::ctrader_fix {

inline constexpr char kSOH = '\x01';

class FixMessageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A single FIX message as an ordered list of (tag, value) fields. This is
// an I/O-boundary type (like everything in this module) — allocation here
// is expected and fine, unlike engine/core's hot path.
//
// Session-header fields that every message needs (BeginString(8),
// BodyLength(9), MsgType(35), CheckSum(10)) are handled specially by
// encode()/tryDecode() rather than stored as ordinary fields; callers only
// ever set the fields that are meaningful to them.
class FixMessage {
public:
    explicit FixMessage(std::string msgType) : msgType_(std::move(msgType)) {}

    FixMessage& set(int tag, std::string value) {
        fields_.emplace_back(tag, std::move(value));
        return *this;
    }
    FixMessage& set(int tag, int64_t value) { return set(tag, std::to_string(value)); }
    FixMessage& set(int tag, double value, int precision = 5);

    [[nodiscard]] const std::string& msgType() const { return msgType_; }

    [[nodiscard]] bool has(int tag) const;
    // First occurrence of `tag`. Throws FixMessageError if absent.
    [[nodiscard]] const std::string& getString(int tag) const;
    [[nodiscard]] std::optional<std::string> getOptionalString(int tag) const;
    [[nodiscard]] int64_t getInt(int tag) const;
    [[nodiscard]] double getDouble(int tag) const;
    // All occurrences of `tag`, in insertion order (repeating groups).
    [[nodiscard]] std::vector<std::string> getAll(int tag) const;

    [[nodiscard]] const std::vector<std::pair<int, std::string>>& fields() const { return fields_; }

    // Encodes to wire format (SOH-delimited), computing BodyLength/CheckSum.
    [[nodiscard]] std::string encode(const std::string& beginString = "FIX.4.4") const;

    // Attempts to decode one complete message from the front of `buffer`.
    // Returns false (and leaves `out`/`consumedBytes` untouched) if the
    // buffer doesn't yet contain a full message — the caller should read
    // more bytes and retry. Throws FixMessageError for a message that is
    // structurally present but invalid (bad checksum, missing MsgType,
    // etc.) — that is never something to silently ignore in a trading
    // client.
    static bool tryDecode(std::string_view buffer, FixMessage& out, std::size_t& consumedBytes);

private:
    std::string msgType_;
    std::vector<std::pair<int, std::string>> fields_;
};

}  // namespace muuk::ctrader_fix
