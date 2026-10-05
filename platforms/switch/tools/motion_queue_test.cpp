// H38-c3: the sensor state-queue bookkeeping of Aurora (extern/aurora/lib/motion_queue.hpp), without libnx.
#include "motion_queue.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace aurora::device::motion;

namespace {
int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

// Newest first, numbers from `newest` down to `oldest`.
std::vector<RawState> queue(uint64_t newest, uint64_t oldest, uint64_t delta = 5'000'000) {
    std::vector<RawState> q;
    for (uint64_t n = newest; n >= oldest; --n) {
        RawState s;
        s.samplingNumber = n;
        s.deltaTime = delta;
        s.accel[0] = 0.0f;
        s.accel[1] = -1.0f;
        s.accel[2] = static_cast<float>(n) * 0.001f;
        s.gyro[0] = 0.5f;
        q.push_back(s);
        if (n == 0) break;
    }
    return q;
}

int read(Cursor& c, const std::vector<RawState>& q, Sample* out, int maxOut = 17) {
    return ingest(c, q.data(), static_cast<int>(q.size()), out, maxOut);
}
}  // namespace

int main() {
    Sample out[17];
    {  // the first read is history: nothing handed out, the cursor starts at the newest state
        Cursor c;
        CHECK(read(c, queue(30, 14), out) == 0 && c.have && c.last == 30);
        // a normal tick: three new states, oldest first, converted to m/s^2 and rad/s, 5 ms each
        const int n = read(c, queue(33, 17), out);
        CHECK(n == 3);
        CHECK(out[0].samplingNumber == 31 && out[1].samplingNumber == 32 && out[2].samplingNumber == 33);
        CHECK(std::fabs(out[0].accel[1] - (-9.80665f)) < 1e-4f && std::fabs(out[0].gyro[0] - 0.5f * kRevToRad) < 1e-4f);
        CHECK(out[0].deltaUs == 5000 && out[0].flags == 0 && out[2].flags == 0);
        CHECK(c.dropped == 0 && c.gaps == 0);
        // nothing new: nothing handed out, and not handed out twice
        CHECK(read(c, queue(33, 17), out) == 0);
        CHECK(c.last == 33);
    }
    {  // delta_time: nanoseconds, microseconds, and values that are neither
        Cursor c;
        read(c, queue(10, 0), out);
        CHECK(read(c, queue(11, 0, 5'000'000), out) == 1 && out[0].deltaUs == 5000);
        CHECK(read(c, queue(12, 0, 5000), out) == 1 && out[0].deltaUs == 5000);   // microseconds
        CHECK(read(c, queue(13, 0, 0), out) == 1 && out[0].deltaUs == kNominalUs);  // zero: the sampling cadence
        CHECK(read(c, queue(14, 0, 3), out) == 1 && out[0].deltaUs == kNominalUs);
        CHECK(read(c, queue(15, 0, 9'000'000'000'000ull), out) == 1 && out[0].deltaUs == kNominalUs);  // absurd
        CHECK(read(c, queue(16, 0, 400'000'000'000ull), out) == 1 && out[0].deltaUs == kNominalUs);
        // a sensible but off-nominal period is kept (a slower sampling mode), within limits
        CHECK(read(c, queue(17, 0, 16'000'000), out) == 1 && out[0].deltaUs == 16000);
        CHECK(read(c, queue(18, 0, 900'000), out) == 1 && out[0].deltaUs == kNominalUs);  // 0.9 ms: below the floor
        CHECK(c.timing.asNanoseconds == 2 && c.timing.asMicroseconds == 1 && c.timing.fallback == 5 && c.timing.samples == 8);
        CHECK(c.timing.rawMin == 0 && c.timing.rawMax == 9'000'000'000'000ull);
        // fallback follows the cadence across a skipped number: two steps = 10 ms
        Cursor g;
        read(g, queue(100, 90), out);
        auto q = queue(102, 91, 0);  // 101 and 102 new
        CHECK(read(g, q, out) == 2 && out[0].deltaUs == kNominalUs && out[1].deltaUs == kNominalUs);
        auto skip = queue(106, 103, 0);
        skip.erase(skip.begin() + 1);  // 105 missing: 103, 104, 106 survive
        CHECK(read(g, skip, out) == 3 && out[2].deltaUs == 2 * kNominalUs && g.dropped == 1);
    }
    {  // a gap: the queue holds 17 states but more than that went by (a long frame)
        Cursor c;
        read(c, queue(50, 34), out);
        CHECK(read(c, queue(70, 54), out) == 17);  // 51..53 were overwritten
        CHECK(c.dropped == 3 && c.gaps == 1);
        CHECK((out[0].flags & kGapBefore) != 0 && out[0].samplingNumber == 54);
        CHECK(out[1].flags == 0 && out[16].flags == 0);
        // the next, normal read has no gap
        CHECK(read(c, queue(72, 56), out) == 2 && out[0].flags == 0 && c.gaps == 1);
    }
    {  // the caller cannot take them all: the oldest are dropped and the first kept sample says so
        Cursor c;
        read(c, queue(50, 34), out);
        CHECK(read(c, queue(60, 44), out, 4) == 4);
        CHECK(out[0].samplingNumber == 57 && out[3].samplingNumber == 60);
        CHECK(c.dropped == 6 && (out[0].flags & kGapBefore) != 0 && out[1].flags == 0);
    }
    {  // the counter restarts (a controller reconnected): treated as a new baseline, with a gap, no stale burst
        Cursor c;
        read(c, queue(500, 484), out);
        CHECK(read(c, queue(3, 0), out) == 0 && c.gaps == 1 && c.last == 3);
        CHECK(read(c, queue(5, 0), out) == 2 && (out[0].flags & kGapBefore) != 0 && out[1].flags == 0);
    }
    {  // a state that was not connected is skipped, the next sample carries the gap; a trailing one carries it to the next read
        Cursor c;
        read(c, queue(10, 0), out);
        auto q = queue(14, 0);
        q[2].connected = false;  // number 12
        CHECK(read(c, q, out) == 3);
        CHECK(out[0].samplingNumber == 11 && out[0].flags == 0);
        CHECK(out[1].samplingNumber == 13 && (out[1].flags & kGapBefore) != 0 && out[2].flags == 0);
        auto t = queue(16, 0);
        t[0].connected = false;  // number 16, newest
        CHECK(read(c, t, out) == 1 && out[0].samplingNumber == 15 && out[0].flags == 0);
        CHECK(c.pendingGap);
        CHECK(read(c, queue(17, 0), out) == 1 && (out[0].flags & kGapBefore) != 0 && !c.pendingGap);
    }
    {  // degenerate input
        Cursor c;
        CHECK(ingest(c, nullptr, 3, out, 17) == 0);
        std::vector<RawState> q = queue(5, 0);
        CHECK(ingest(c, q.data(), 0, out, 17) == 0);
        CHECK(ingest(c, q.data(), static_cast<int>(q.size()), out, 0) == 0);
    }
    if (failures == 0) {
        std::puts("motion queue: new states only, delta_time unit and fallback, gaps/overflow/restart/disconnect flagged, "
                  "nothing handed out twice: ok");
    }
    return failures == 0 ? 0 : 1;
}
