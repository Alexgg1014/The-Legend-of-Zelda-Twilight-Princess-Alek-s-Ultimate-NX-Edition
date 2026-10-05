#pragma once
// aleks2 (H28): the part of the benchmark that knows nothing about the game --
// frame-time statistics, a small JSON writer and an atomic file write. It is
// kept apart from benchmark.cpp so it builds, and is tested, on a PC with a
// plain compiler (tools/test-benchmark-report.sh).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::benchmark {

// Bumped when a field changes meaning or is removed; adding a field does not.
// 3 (H33): profile, policy, effective resolution, waits, governor, judder.
// 4 (H35): fifo, uniforms, frame_waits, derived (painter_net, drawrec_net), interpolation frame kinds.
inline constexpr int kReportSchemaVersion = 4;

struct FrameSummary {
    std::size_t frames = 0;
    // Sum of the frame intervals: what the window actually lasted, as the
    // frames saw it. The wall clock decides when the window ends, this says
    // how much of it the frames cover.
    double coveredSeconds = 0.0;
    // Frames over covered time, not the mean of per-frame rates: the second
    // one overstates a run with a few very fast frames.
    double avgFps = 0.0;
    // Frame rate of the slowest 1 % of frames: 1000 / (mean frame time of the
    // slowest ceil(1 % of frames)). The usual "1 % low" of frame-time tools.
    double low1PctFps = 0.0;
    double meanMs = 0.0;
    double medianMs = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
    std::size_t over33 = 0;  // frames longer than 33.3 ms (a missed 30 fps tick)
    std::size_t over50 = 0;  // frames longer than 50 ms
    double pctOver33 = 0.0;
    double pctOver50 = 0.0;
    // H33: cadence irregularity. Pairs of consecutive frames whose intervals differ by more
    // than kJudderStepMs, over all pairs. A steady 30 or 60 reads ~0; a 45 on a 60 Hz output
    // (16.7/16.7/33.3 ms) or a 33/50/33/50 stagger reads high. Needs the frames in time order.
    std::size_t judderPairs = 0;
    double judderPct = 0.0;
};

inline constexpr float kJudderStepMs = 8.0f;

// Nearest-rank percentile of an ascending sequence: the smallest value that at
// least p % of the samples are less than or equal to. 0 for an empty sequence.
double percentile_sorted(const std::vector<float>& sorted, double p);

// Takes the frame intervals in milliseconds, in time order (the percentiles do not need it;
// the judder index does).
FrameSummary summarize_frames(std::vector<float> frameMs);

// Just enough JSON for a report: nested objects and arrays, strings escaped,
// non-finite numbers written as null (JSON has no NaN).
class JsonWriter {
public:
    JsonWriter& begin_object(const char* key = nullptr);
    JsonWriter& end_object();
    JsonWriter& begin_array(const char* key = nullptr);
    JsonWriter& end_array();
    JsonWriter& str(const char* key, std::string_view value);
    JsonWriter& num(const char* key, double value, int decimals = 3);
    JsonWriter& integer(const char* key, std::int64_t value);
    JsonWriter& boolean(const char* key, bool value);
    JsonWriter& null(const char* key);
    // Array elements, which have no key.
    JsonWriter& str_item(std::string_view value);

    // The finished document. Valid only when every begin has been closed.
    bool balanced() const { return mDepth == 0; }
    std::string take();

private:
    void prefix(const char* key);
    void indent();

    std::string mOut;
    int mDepth = 0;
    // Whether the container at each depth already has a member.
    std::vector<bool> mHasMember;
};

std::string json_escape(std::string_view value);

struct WriteResult {
    bool ok = false;
    std::filesystem::path path;  // the final file when ok
    std::string error;           // what failed, when not
};

// Writes `content` to dir/fileName without ever leaving a half-written file
// under that name: it is written to dir/fileName.tmp, flushed and closed, and
// only then renamed over the final name. Each way it can fail says so --
// directory, free space, write, rename -- instead of one generic error.
WriteResult write_report_atomic(
    const std::filesystem::path& dir, const std::string& fileName, std::string_view content);

// "20260930-142501" and "2026-09-30T14:25:01Z" for the clock given (UTC).
std::string utc_stamp_for_file(std::int64_t unixSeconds);
std::string utc_stamp_iso(std::int64_t unixSeconds);

// bench-<stamp>-<build id>.json, with the build id reduced to characters that
// are safe on a FAT card.
std::string report_file_name(std::string_view stamp, std::string_view buildId);

}  // namespace dusk::benchmark
