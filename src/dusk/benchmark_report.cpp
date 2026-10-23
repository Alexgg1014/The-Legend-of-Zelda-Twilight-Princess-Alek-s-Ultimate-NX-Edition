#include "dusk/benchmark_report.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace dusk::benchmark {

double percentile_sorted(const std::vector<float>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double clamped = std::clamp(p, 0.0, 100.0);
    auto rank = static_cast<std::size_t>(std::ceil(clamped / 100.0 * static_cast<double>(sorted.size())));
    rank = std::clamp<std::size_t>(rank, 1, sorted.size());
    return static_cast<double>(sorted[rank - 1]);
}

FrameSummary summarize_frames(std::vector<float> frameMs) {
    FrameSummary out;
    out.frames = frameMs.size();
    if (frameMs.empty()) {
        return out;
    }
    // In time order, before the sort: consecutive intervals that differ by more than a step.
    if (frameMs.size() > 1) {
        for (std::size_t i = 1; i < frameMs.size(); ++i) {
            if (std::fabs(frameMs[i] - frameMs[i - 1]) > kJudderStepMs) {
                ++out.judderPairs;
            }
        }
        out.judderPct =
            100.0 * static_cast<double>(out.judderPairs) / static_cast<double>(frameMs.size() - 1);
    }
    for (const float ms : frameMs) {
        const auto v = static_cast<long>(std::lround(ms / kVblankMs));
        ++out.vblanks[std::clamp<long>(v, 1, 4)];
    }
    std::sort(frameMs.begin(), frameMs.end());

    double total = 0.0;
    for (const float ms : frameMs) {
        total += static_cast<double>(ms);
        if (ms > kMissedFrameMs) {
            ++out.over33;
        }
        if (ms > 50.0f) {
            ++out.over50;
        }
    }
    const auto count = static_cast<double>(frameMs.size());
    out.coveredSeconds = total / 1000.0;
    out.meanMs = total / count;
    out.avgFps = total > 0.0 ? count * 1000.0 / total : 0.0;
    out.medianMs = percentile_sorted(frameMs, 50.0);
    out.p95Ms = percentile_sorted(frameMs, 95.0);
    out.p99Ms = percentile_sorted(frameMs, 99.0);
    out.maxMs = static_cast<double>(frameMs.back());
    out.pctOver33 = 100.0 * static_cast<double>(out.over33) / count;
    out.pctOver50 = 100.0 * static_cast<double>(out.over50) / count;

    const auto worst = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(count * 0.01)));
    double worstTotal = 0.0;
    for (std::size_t i = frameMs.size() - worst; i < frameMs.size(); ++i) {
        worstTotal += static_cast<double>(frameMs[i]);
    }
    const double worstMean = worstTotal / static_cast<double>(worst);
    out.low1PctFps = worstMean > 0.0 ? 1000.0 / worstMean : 0.0;
    return out;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

void JsonWriter::indent() {
    mOut.append(static_cast<std::size_t>(mDepth) * 2, ' ');
}

void JsonWriter::prefix(const char* key) {
    if (!mHasMember.empty()) {
        if (mHasMember.back()) {
            mOut += ',';
        }
        mHasMember.back() = true;
        mOut += '\n';
    }
    indent();
    if (key != nullptr) {
        mOut += '"';
        mOut += json_escape(key);
        mOut += "\": ";
    }
}

JsonWriter& JsonWriter::begin_object(const char* key) {
    prefix(key);
    mOut += '{';
    ++mDepth;
    mHasMember.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    const bool had = !mHasMember.empty() && mHasMember.back();
    if (!mHasMember.empty()) {
        mHasMember.pop_back();
    }
    --mDepth;
    if (had) {
        mOut += '\n';
        indent();
    }
    mOut += '}';
    return *this;
}

JsonWriter& JsonWriter::begin_array(const char* key) {
    prefix(key);
    mOut += '[';
    ++mDepth;
    mHasMember.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    const bool had = !mHasMember.empty() && mHasMember.back();
    if (!mHasMember.empty()) {
        mHasMember.pop_back();
    }
    --mDepth;
    if (had) {
        mOut += '\n';
        indent();
    }
    mOut += ']';
    return *this;
}

JsonWriter& JsonWriter::str(const char* key, std::string_view value) {
    prefix(key);
    mOut += '"';
    mOut += json_escape(value);
    mOut += '"';
    return *this;
}

JsonWriter& JsonWriter::str_item(std::string_view value) {
    return str(nullptr, value);
}

JsonWriter& JsonWriter::num(const char* key, double value, int decimals) {
    prefix(key);
    if (!std::isfinite(value)) {
        mOut += "null";
        return *this;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 9), value);
    mOut += buf;
    return *this;
}

JsonWriter& JsonWriter::integer(const char* key, std::int64_t value) {
    prefix(key);
    mOut += std::to_string(value);
    return *this;
}

JsonWriter& JsonWriter::boolean(const char* key, bool value) {
    prefix(key);
    mOut += value ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null(const char* key) {
    prefix(key);
    mOut += "null";
    return *this;
}

std::string JsonWriter::take() {
    mOut += '\n';
    return std::move(mOut);
}

WriteResult write_report_atomic(
    const std::filesystem::path& dir, const std::string& fileName, std::string_view content) {
    namespace fs = std::filesystem;
    WriteResult result;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        result.error = "cannot create directory '" + dir.string() + "': " + ec.message();
        return result;
    }
    if (!fs::is_directory(dir, ec)) {
        result.error = "'" + dir.string() + "' is not a directory";
        return result;
    }

    // A report is a few KB. The margin is for the filesystem's own overhead and
    // for a card that is already nearly full: failing here is a clear message,
    // failing inside fwrite is a truncated file.
    constexpr std::uintmax_t kSpaceMargin = 64 * 1024;
    const auto space = fs::space(dir, ec);
    if (!ec && space.available < content.size() + kSpaceMargin) {
        result.error = "not enough free space in '" + dir.string() + "' (" +
                       std::to_string(space.available) + " bytes available, " +
                       std::to_string(content.size() + kSpaceMargin) + " needed)";
        return result;
    }
    // If the free space could not be read, carry on: the write below is what
    // decides, and it reports its own error.

    const fs::path finalPath = dir / fileName;
    const fs::path tmpPath = dir / (fileName + ".tmp");

    std::FILE* file = std::fopen(tmpPath.string().c_str(), "wb");
    if (file == nullptr) {
        result.error = "cannot open '" + tmpPath.string() + "' for writing: " + std::strerror(errno);
        return result;
    }
    const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (written != content.size() || !flushed || !closed) {
        const std::string why = std::strerror(errno);
        fs::remove(tmpPath, ec);
        result.error = "write to '" + tmpPath.string() + "' failed (" + std::to_string(written) + " of " +
                       std::to_string(content.size()) + " bytes): " + why;
        return result;
    }

    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        // Some filesystems will not rename over an existing file.
        std::error_code removeEc;
        fs::remove(finalPath, removeEc);
        ec.clear();
        fs::rename(tmpPath, finalPath, ec);
    }
    if (ec) {
        const std::string why = ec.message();
        fs::remove(tmpPath, ec);
        result.error = "cannot move the report into place as '" + finalPath.string() + "': " + why;
        return result;
    }

    result.ok = true;
    result.path = finalPath;
    return result;
}

namespace {

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm), so the
// stamp does not depend on the platform's timezone database or gmtime_r.
void civil_from_unix(std::int64_t unixSeconds, int& y, int& mo, int& d, int& h, int& mi, int& s) {
    std::int64_t days = unixSeconds / 86400;
    std::int64_t rem = unixSeconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    h = static_cast<int>(rem / 3600);
    mi = static_cast<int>(rem % 3600 / 60);
    s = static_cast<int>(rem % 60);

    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(year + (mo <= 2 ? 1 : 0));
}

}  // namespace

std::string utc_stamp_for_file(std::int64_t unixSeconds) {
    int y, mo, d, h, mi, s;
    civil_from_unix(unixSeconds, y, mo, d, h, mi, s);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", y, mo, d, h, mi, s);
    return buf;
}

std::string utc_stamp_iso(std::int64_t unixSeconds) {
    int y, mo, d, h, mi, s;
    civil_from_unix(unixSeconds, y, mo, d, h, mi, s);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, s);
    return buf;
}

std::string report_file_name(std::string_view stamp, std::string_view buildId) {
    std::string id;
    for (const char c : buildId) {
        const bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          c == '-' || c == '_' || c == '.';
        id += safe ? c : '_';
    }
    if (id.empty()) {
        id = "unknown";
    }
    return "bench-" + std::string{stamp} + "-" + id + ".json";
}

}  // namespace dusk::benchmark
