// aleks2 (V1-PGO): see pgo_dump.hpp. Switch only (CMakeLists.txt); the writer exists only with ALEKS2_PGO_GENERATE.
#include "dusk/pgo_dump.hpp"

#ifdef ALEKS2_PGO_GENERATE

#include "dusk/logging.h"

#include <switch.h>

extern "C" {
#include <gcov.h>  // no extern "C" of its own
}

#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// -fprofile-info-section=gcov_info puts a pointer to every unit's gcov_info in this section; the linker provides its
// bounds because the name is a C identifier.
extern "C" const struct gcov_info* const __start_gcov_info[];
extern "C" const struct gcov_info* const __stop_gcov_info[];

namespace dusk::pgo_dump {
namespace {

constexpr borealis::Log Log{"dusk::pgo"};

constexpr const char* kDir = "sdmc:/switch/dusklight/pgo";
constexpr u64 kPeriodNs = 120'000'000'000ull;  // a snapshot every 2 minutes
constexpr u64 kPollNs = 250'000'000ull;

// File layout (pgo_stream.py reads it): "DKPGO\1\0\0", u32 units, u32 0; per unit u32 name length, the name (the
// .gcda path the compiler recorded), u32 data length, the gcda stream; then "DKPGOEND", u32 units written.
constexpr char kMagic[8] = {'D', 'K', 'P', 'G', 'O', 1, 0, 0};
constexpr char kEnd[8] = {'D', 'K', 'P', 'G', 'O', 'E', 'N', 'D'};

std::string s_path;  // this run's file
Thread s_thread;
bool s_started = false;
bool s_threaded = false;
bool s_wasFront = true;
u64 s_lastSnapshotNs = 0;  // main-thread fallback only
std::atomic<bool> s_request{false};
std::atomic<bool> s_stop{false};
unsigned s_dumps = 0;

struct Unit {
    std::string name;
    std::vector<std::uint8_t> data;
    std::vector<std::unique_ptr<std::uint8_t[]>> allocations;
};

void on_data(const void* p, unsigned n, void* arg) {
    auto& u = *static_cast<Unit*>(arg);
    const auto* b = static_cast<const std::uint8_t*>(p);
    u.data.insert(u.data.end(), b, b + n);
}

void on_filename(const char* name, void* arg) {
    static_cast<Unit*>(arg)->name = name != nullptr ? name : "";
}

void* on_allocate(unsigned n, void* arg) {
    auto& u = *static_cast<Unit*>(arg);
    u.allocations.emplace_back(new std::uint8_t[n]);
    return u.allocations.back().get();
}

bool put(std::FILE* f, const void* p, std::size_t n) {
    return std::fwrite(p, 1, n, f) == n;
}

bool put_u32(std::FILE* f, std::uint32_t v) {
    return put(f, &v, sizeof v);
}

void snapshot(const char* why) {
    const auto* begin = __start_gcov_info;
    const auto* end = __stop_gcov_info;
    const auto units = static_cast<std::uint32_t>(end - begin);
    if (units == 0) {
        return;
    }
    const u64 t0 = armTicksToNs(armGetSystemTick());
    const std::string tmp = s_path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        Log.warn("PGO: cannot write {}", tmp);
        return;
    }
    static std::vector<char> s_buffer(1 << 20);
    std::setvbuf(f, s_buffer.data(), _IOFBF, s_buffer.size());
    bool ok = put(f, kMagic, sizeof kMagic) && put_u32(f, units) && put_u32(f, 0);
    std::uint64_t bytes = 0;
    std::uint32_t written = 0;
    Unit u;
    for (const auto* p = begin; ok && p != end; ++p) {
        if (*p == nullptr) {
            continue;
        }
        u.name.clear();
        u.data.clear();
        __gcov_info_to_gcda(*p, on_filename, on_data, on_allocate, &u);
        u.allocations.clear();
        ok = put_u32(f, static_cast<std::uint32_t>(u.name.size())) && put(f, u.name.data(), u.name.size()) &&
             put_u32(f, static_cast<std::uint32_t>(u.data.size())) && put(f, u.data.data(), u.data.size());
        bytes += u.data.size();
        ++written;
    }
    ok = ok && put(f, kEnd, sizeof kEnd) && put_u32(f, written);
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        Log.warn("PGO: writing {} failed; the previous snapshot stays", tmp);
        std::remove(tmp.c_str());
        return;
    }
    std::remove(s_path.c_str());  // FAT: rename does not replace
    if (std::rename(tmp.c_str(), s_path.c_str()) != 0) {
        Log.warn("PGO: rename to {} failed; the snapshot is in {}", s_path, tmp);
        return;
    }
    ++s_dumps;
    const u64 ms = (armTicksToNs(armGetSystemTick()) - t0) / 1'000'000ull;
    Log.info("PGO: snapshot {} ({}): {} units, {:.1f} MiB of counters, {} ms -> {}", s_dumps, why, written,
        static_cast<double>(bytes) / (1024.0 * 1024.0), ms, s_path);
}

void writer(void*) {
    u64 waited = 0;
    while (!s_stop.load(std::memory_order_acquire)) {
        svcSleepThread(kPollNs);
        waited += kPollNs;
        if (s_request.exchange(false, std::memory_order_acq_rel)) {
            snapshot("out of focus");
            waited = 0;
        } else if (waited >= kPeriodNs) {
            snapshot("every 2 min");
            waited = 0;
        }
    }
}

}  // namespace

void start() {
    if (s_started) {
        return;
    }
    s_started = true;  // once, whether or not the thread starts
    if (+__stop_gcov_info == +__start_gcov_info) {
        Log.warn("PGO build without profile units: nothing to write");
        return;
    }
    mkdir("sdmc:/switch/dusklight", 0777);
    mkdir(kDir, 0777);
    char stamp[32] = "unknown";
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    if (localtime_r(&now, &tm) != nullptr) {
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    }
    s_path = std::string(kDir) + "/run-" + stamp + ".dkpgo";
    s_lastSnapshotNs = armTicksToNs(armGetSystemTick());
    // Low priority, off the game's core: it only reads counters and waits on the card. 0x3B, not 0x3F: the emulator's
    // process allows 0x3B at the lowest and refused 0x3F (rc 0xe001, invalid priority).
    Result rc = threadCreate(&s_thread, writer, nullptr, nullptr, 64 * 1024, 0x3B, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&s_thread);
        if (R_FAILED(rc)) {
            threadClose(&s_thread);
        }
    }
    s_threaded = R_SUCCEEDED(rc);
    if (!s_threaded) {
        s_thread = Thread{};
        Log.warn("PGO: no writer thread (rc 0x{:x}); the main thread takes the snapshots (a short stop each time)", rc);
    }
    Log.info("PGO profile build: {} units; snapshots every 2 min, on HOME and at quit -> {}",
        static_cast<unsigned>(__stop_gcov_info - __start_gcov_info), s_path);
}

void tick() {
    if (!s_started) {
        start();
    }
    const bool front = appletGetFocusState() == AppletFocusState_InFocus;
    const bool lostFocus = s_wasFront && !front;
    s_wasFront = front;
    if (s_path.empty()) {
        return;
    }
    if (s_threaded) {
        if (lostFocus) {
            s_request.store(true, std::memory_order_release);
        }
        return;
    }
    const u64 now = armTicksToNs(armGetSystemTick());
    if (lostFocus) {
        snapshot("out of focus");
        s_lastSnapshotNs = now;
    } else if (now - s_lastSnapshotNs >= kPeriodNs) {
        snapshot("every 2 min");
        s_lastSnapshotNs = armTicksToNs(armGetSystemTick());
    }
}

void shutdown() {
    if (!s_started || s_path.empty()) {
        return;
    }
    s_stop.store(true, std::memory_order_release);
    if (s_threaded) {
        threadWaitForExit(&s_thread);
        threadClose(&s_thread);
    }
    snapshot("quit");
}

}  // namespace dusk::pgo_dump

#else

namespace dusk::pgo_dump {
void start() {}
void tick() {}
void shutdown() {}
}  // namespace dusk::pgo_dump

#endif
