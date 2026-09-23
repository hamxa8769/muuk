#include "muuk/ctrader_fix/fix_message.hpp"

#include <charconv>
#include <cstdio>

namespace muuk::ctrader_fix {

namespace {

struct RawField {
    int tag;
    std::string_view value;
};

// Splits `range` (a byte span not including any trailing partial field)
// into tag=value segments delimited by SOH. Throws FixMessageError on any
// segment that isn't a well-formed "<digits>=<value>".
std::vector<RawField> splitFields(std::string_view range) {
    std::vector<RawField> out;
    std::size_t start = 0;
    while (start < range.size()) {
        const std::size_t soh = range.find(kSOH, start);
        if (soh == std::string_view::npos) {
            throw FixMessageError("FIX field missing SOH terminator");
        }
        const std::string_view segment = range.substr(start, soh - start);
        const std::size_t eq = segment.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            throw FixMessageError("malformed FIX field: " + std::string(segment));
        }
        int tag = 0;
        const auto tagStr = segment.substr(0, eq);
        const auto res = std::from_chars(tagStr.data(), tagStr.data() + tagStr.size(), tag);
        if (res.ec != std::errc{} || res.ptr != tagStr.data() + tagStr.size()) {
            throw FixMessageError("malformed FIX tag: " + std::string(tagStr));
        }
        out.push_back(RawField{tag, segment.substr(eq + 1)});
        start = soh + 1;
    }
    return out;
}

std::uint32_t checksumOf(std::string_view bytes) {
    std::uint32_t sum = 0;
    for (unsigned char c : bytes) sum += c;
    return sum % 256;
}

std::string threeDigits(std::uint32_t value) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03u", value % 1000);
    return std::string(buf, 3);
}

}  // namespace

FixMessage& FixMessage::set(int tag, double value, int precision) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, value);
    return set(tag, std::string(buf));
}

bool FixMessage::has(int tag) const {
    for (const auto& [t, v] : fields_) {
        if (t == tag) return true;
    }
    return false;
}

const std::string& FixMessage::getString(int tag) const {
    for (const auto& [t, v] : fields_) {
        if (t == tag) return v;
    }
    throw FixMessageError("missing required FIX tag " + std::to_string(tag));
}

std::optional<std::string> FixMessage::getOptionalString(int tag) const {
    for (const auto& [t, v] : fields_) {
        if (t == tag) return v;
    }
    return std::nullopt;
}

int64_t FixMessage::getInt(int tag) const {
    const std::string& v = getString(tag);
    int64_t result = 0;
    const auto res = std::from_chars(v.data(), v.data() + v.size(), result);
    if (res.ec != std::errc{} || res.ptr != v.data() + v.size()) {
        throw FixMessageError("FIX tag " + std::to_string(tag) + " is not an integer: " + v);
    }
    return result;
}

double FixMessage::getDouble(int tag) const {
    const std::string& v = getString(tag);
    try {
        std::size_t consumed = 0;
        const double result = std::stod(v, &consumed);
        if (consumed != v.size()) throw std::invalid_argument("trailing characters");
        return result;
    } catch (const std::exception&) {
        throw FixMessageError("FIX tag " + std::to_string(tag) + " is not a number: " + v);
    }
}

std::vector<std::string> FixMessage::getAll(int tag) const {
    std::vector<std::string> out;
    for (const auto& [t, v] : fields_) {
        if (t == tag) out.push_back(v);
    }
    return out;
}

std::string FixMessage::encode(const std::string& beginString) const {
    std::string body;
    body += "35=" + msgType_;
    body += kSOH;
    for (const auto& [tag, value] : fields_) {
        body += std::to_string(tag) + "=" + value;
        body += kSOH;
    }

    std::string header = "8=" + beginString;
    header += kSOH;
    header += "9=" + std::to_string(body.size());
    header += kSOH;

    const std::string beforeChecksum = header + body;
    const std::string trailer = "10=" + threeDigits(checksumOf(beforeChecksum)) + kSOH;

    return beforeChecksum + trailer;
}

bool FixMessage::tryDecode(std::string_view buffer, FixMessage& out, std::size_t& consumedBytes) {
    // Buffers shorter than what's needed to even see the next field are a
    // normal "read more and retry" condition during streaming, not a
    // framing error — only throw once there's enough data to be sure it's
    // actually wrong.
    if (buffer.size() < 2) return false;
    if (buffer[0] != '8' || buffer[1] != '=') {
        throw FixMessageError("FIX message must start with tag 8 (BeginString)");
    }

    const std::size_t tag8Soh = buffer.find(kSOH, 0);
    if (tag8Soh == std::string_view::npos) return false;

    const std::size_t tag9Start = tag8Soh + 1;
    if (buffer.size() < tag9Start + 2) return false;
    if (buffer[tag9Start] != '9' || buffer[tag9Start + 1] != '=') {
        throw FixMessageError("FIX message tag 9 (BodyLength) must immediately follow tag 8");
    }
    const std::size_t tag9Soh = buffer.find(kSOH, tag9Start);
    if (tag9Soh == std::string_view::npos) return false;

    const std::string_view bodyLenStr = buffer.substr(tag9Start + 2, tag9Soh - (tag9Start + 2));
    std::size_t bodyLength = 0;
    {
        const auto res = std::from_chars(bodyLenStr.data(), bodyLenStr.data() + bodyLenStr.size(), bodyLength);
        if (res.ec != std::errc{} || res.ptr != bodyLenStr.data() + bodyLenStr.size()) {
            throw FixMessageError("malformed BodyLength: " + std::string(bodyLenStr));
        }
    }

    const std::size_t bodyStart = tag9Soh + 1;
    const std::size_t checksumFieldLen = 7;  // "10=" + 3 digits + SOH
    const std::size_t total = bodyStart + bodyLength + checksumFieldLen;
    if (buffer.size() < total) return false;  // need more bytes

    const std::string_view checksumField = buffer.substr(bodyStart + bodyLength, checksumFieldLen);
    if (checksumField.substr(0, 3) != "10=" || checksumField.back() != kSOH) {
        throw FixMessageError("expected tag 10 (CheckSum) immediately after the message body");
    }
    const std::string_view checksumDigits = checksumField.substr(3, 3);
    std::uint32_t expectedChecksum = 0;
    {
        const auto res = std::from_chars(checksumDigits.data(), checksumDigits.data() + 3, expectedChecksum);
        if (res.ec != std::errc{} || res.ptr != checksumDigits.data() + 3) {
            throw FixMessageError("malformed CheckSum digits: " + std::string(checksumDigits));
        }
    }

    const std::string_view beforeChecksum = buffer.substr(0, bodyStart + bodyLength);
    if (checksumOf(beforeChecksum) != expectedChecksum) {
        throw FixMessageError("FIX checksum mismatch");
    }

    const std::vector<RawField> allFields = splitFields(buffer.substr(0, total));

    std::optional<std::string> msgType;
    FixMessage msg{""};
    std::vector<std::pair<int, std::string>> fields;
    for (const auto& field : allFields) {
        if (field.tag == 8 || field.tag == 9 || field.tag == 10) continue;
        if (field.tag == 35) {
            msgType = std::string(field.value);
            continue;
        }
        fields.emplace_back(field.tag, std::string(field.value));
    }
    if (!msgType) {
        throw FixMessageError("FIX message is missing tag 35 (MsgType)");
    }

    out = FixMessage(*msgType);
    for (auto& [tag, value] : fields) {
        out.set(tag, std::move(value));
    }
    consumedBytes = total;
    return true;
}

}  // namespace muuk::ctrader_fix
