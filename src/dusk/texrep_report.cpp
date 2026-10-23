// aleks2 (V1-rc12): see texrep_report.hpp.
#include "dusk/texrep_report.hpp"

#ifdef __SWITCH__
#include "d/d_com_inf_game.h"
#include "dusk/benchmark.hpp"
#include "dusk/dev_mode.hpp"
#include "dusk/logging.h"
#include "dusk/texture_replacements.hpp"

#include <aurora/tuning.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dusk::texrep_report {
namespace {

inline constexpr borealis::Log Log{"texrep"};
using clock = std::chrono::steady_clock;

struct Row {
    uint32_t width = 0, height = 0, mips = 0;
    std::string format;
    uint64_t bytes = 0;
    bool full = false;
    uint32_t loads = 0;
    std::set<int> rooms;
};

struct StageData {
    std::map<std::string, Row> rows;  // by texture name (pack path)
    uint64_t peak = 0;
    uint64_t evictions = 0;
    std::set<int> rooms;
    unsigned visits = 0;
    bool dirty = false;
};

bool g_on = false;
std::map<std::string, StageData> g_stages;
std::string g_current;
AuroraTexrepUsageTotals g_last{};
clock::time_point g_nextSample{};
clock::time_point g_nextWrite{};

struct Take {
    StageData* stage;
    int room;
};

void on_row(void* user, const AuroraTexrepUsageRow* r) {
    auto* t = static_cast<Take*>(user);
    Row& row = t->stage->rows[r->name];
    row.width = r->width;
    row.height = r->height;
    row.mips = r->mips;
    row.format = r->format;
    row.bytes = r->bytes;
    row.full = row.full || r->full != 0;
    row.loads += r->loads;
    row.rooms.insert(t->room);
    t->stage->dirty = true;
}

void collect(const std::string& stage) {
    StageData& sd = g_stages[stage];
    Take take{&sd, dComIfGp_roomControl_getStayNo()};
    aurora_gx_texrep_usage_take(on_row, &take, &g_last);
    sd.peak = std::max(sd.peak, g_last.peakBytes);
    if (g_last.evictions != 0) {
        sd.evictions += g_last.evictions;
        sd.dirty = true;
    }
    sd.rooms.insert(take.room);
}

std::string rooms_text(const std::set<int>& rooms) {
    std::string s;
    for (int r : rooms) {
        s += (s.empty() ? "" : " ") + std::to_string(r);
    }
    return s;
}

double mib(uint64_t b) {
    return static_cast<double>(b) / (1024.0 * 1024.0);
}

void write(const std::string& stage) {
    auto it = g_stages.find(stage);
    if (it == g_stages.end() || !it->second.dirty) {
        return;
    }
    StageData& sd = it->second;
    sd.dirty = false;
    std::string safe = stage.empty() ? "unknown" : stage;
    for (char& c : safe) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            c = '_';
        }
    }
    std::vector<std::pair<const std::string*, const Row*>> rows;
    uint64_t total = 0;
    for (const auto& [name, row] : sd.rows) {
        rows.emplace_back(&name, &row);
        total += row.bytes;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second->bytes > b.second->bytes; });
    char path[160];
    std::snprintf(path, sizeof path, "sdmc:/switch/dusklight/cache/logs/texrep-%s.txt", safe.c_str());
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        Log.warn("cannot write {}", path);
        return;
    }
    std::fprintf(f,
        "# dusklight texrep v1: build %s, stage %s, visits %u, rooms %s\n"
        "# budget %.1f MiB (texrep_budget_mb), peak resident in this stage %.1f MiB, evictions in this stage %llu "
        "(session %llu), resident now %.1f MiB in %u textures\n"
        "# loaded in this stage: %zu textures, %.1f MiB (each counted once at its last size)\n"
        "# bytes width height mips format tier loads rooms name\n",
        benchmark::build_id().c_str(), stage.c_str(), sd.visits, rooms_text(sd.rooms).c_str(), mib(g_last.budgetBytes),
        mib(sd.peak), static_cast<unsigned long long>(sd.evictions),
        static_cast<unsigned long long>(g_last.totalEvictions), mib(g_last.residentBytes), g_last.residentTextures,
        rows.size(), mib(total));
    for (const auto& [name, row] : rows) {
        std::fprintf(f, "%llu %u %u %u %s %s %u %s %s\n", static_cast<unsigned long long>(row->bytes), row->width,
            row->height, row->mips, row->format.c_str(), row->full ? "full" : "thumb", row->loads,
            rooms_text(row->rooms).c_str(), name->c_str());
    }
    std::fclose(f);
}

bool start_if_wanted(const char* stage) {
    if (g_on) {
        return true;
    }
    if (!dev_mode() || texture_replacements::directory_registrations() == 0) {
        return false;
    }
    aurora_gx_texrep_usage_enable(1);
    g_on = true;
    g_current = stage != nullptr ? stage : "";
    g_stages[g_current].visits++;
    Log.info("texture replacement usage report on ({} pack registrations): cache/logs/texrep-<stage>.txt",
        texture_replacements::directory_registrations());
    return true;
}

}  // namespace

void tick(const char* stage) {
    const auto now = clock::now();
    if (now < g_nextSample) {
        return;
    }
    g_nextSample = now + std::chrono::seconds(2);
    if (!start_if_wanted(stage)) {
        return;
    }
    collect(g_current);
    if (now >= g_nextWrite) {
        g_nextWrite = now + std::chrono::seconds(30);
        write(g_current);
    }
}

void stage_left(const char* nextStage) {
    if (!g_on) {
        return;
    }
    collect(g_current);
    write(g_current);
    g_current = nextStage != nullptr ? nextStage : "";
    g_stages[g_current].visits++;
    g_nextWrite = clock::now() + std::chrono::seconds(30);
}

}  // namespace dusk::texrep_report

#else

namespace dusk::texrep_report {
void tick(const char*) {}
void stage_left(const char*) {}
}  // namespace dusk::texrep_report

#endif
