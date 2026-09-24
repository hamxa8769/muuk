#include "muuk/ctrader_fix/volume_stats.hpp"

#include <cstdio>
#include <ctime>
#include <map>
#include <sstream>

namespace muuk::ctrader_fix {

void VolumeBucket::add(const BasketRecord& basket) {
    ++baskets;
    double basketLots = 0.0;
    for (const auto& leg : basket.legs) {
        ++positions;
        basketLots += leg.lots;
        notional += leg.lots * basket.contractSize * leg.entryPrice;
    }
    lots += basketLots;
    if (basket.net > 0.005) {
        ++won;
        wonLots += basketLots;
    } else if (basket.net < -0.005) {
        ++lost;
        lostLots += basketLots;
    }
    gross += basket.gross;
    commission += basket.commission;
    net += basket.net;
}

std::string periodKey(const std::string& timestamp, VolumePeriod period) {
    int y = 0;
    int m = 0;
    int d = 0;
    char dash1 = 0;
    char dash2 = 0;
    std::istringstream in(timestamp);
    in >> y >> dash1 >> m >> dash2 >> d;
    if (in.fail() || dash1 != '-' || dash2 != '-') return "unknown";
    char buf[32];
    switch (period) {
        case VolumePeriod::Month:
            std::snprintf(buf, sizeof(buf), "%04d-%02d", y, m);
            return buf;
        case VolumePeriod::Week: {
            // Back to that week's Monday (calendar arithmetic via mktime at noon, DST-safe).
            std::tm tm{};
            tm.tm_year = y - 1900;
            tm.tm_mon = m - 1;
            tm.tm_mday = d;
            tm.tm_hour = 12;
            tm.tm_isdst = -1;
            if (std::mktime(&tm) == static_cast<std::time_t>(-1)) return "unknown";
            const int sinceMonday = (tm.tm_wday + 6) % 7;
            tm.tm_mday -= sinceMonday;
            tm.tm_isdst = -1;
            std::mktime(&tm);
            std::snprintf(buf, sizeof(buf), "Week of %04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            return buf;
        }
        case VolumePeriod::Day:
        default:
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
            return buf;
    }
}

std::vector<VolumeBucket> volumeByPeriod(const std::vector<BasketRecord>& baskets, VolumePeriod period) {
    std::map<std::string, VolumeBucket> byKey;  // keys sort chronologically
    for (const auto& b : baskets) {
        const std::string key = periodKey(b.closedAt.empty() ? b.startedAt : b.closedAt, period);
        VolumeBucket& bucket = byKey[key];
        bucket.period = key;
        bucket.add(b);
    }
    std::vector<VolumeBucket> out;
    out.reserve(byKey.size());
    for (auto it = byKey.rbegin(); it != byKey.rend(); ++it) out.push_back(it->second);
    return out;
}

VolumeBucket volumeTotal(const std::vector<BasketRecord>& baskets) {
    VolumeBucket total;
    total.period = "All time";
    for (const auto& b : baskets) total.add(b);
    return total;
}

}  // namespace muuk::ctrader_fix
