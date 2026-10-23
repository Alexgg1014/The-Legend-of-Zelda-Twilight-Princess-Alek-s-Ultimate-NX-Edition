// aleks2 (TLS-lab): phase A of the TLS candidate's console test -- the module TLS runtime on Horizon,
// before any catalog mod. See switch_tls_selftest.hpp.
//
// It runs the same checks platforms/switch/tools/elf_tls_exec_test.cpp runs under qemu, but with what
// the game really uses: elf_loader with switch_elf_memory.cpp's CodeMemory, the Switch resolver
// (elf_tls_aarch64.S built with __SWITCH__: libnx's thread pointer at TPIDRRO_EL0 + 0x1F8), newlib's
// pthreads and libstdc++, libnx's thread-exit key destructors, and the Linux module's imports bound to
// the game's exports and the production shims (linux_tls_resolver). The module is the one the host
// tests build (elf_tls_test_module.cpp + elf_tls_test_regs.S, aarch64-linux-gnu-g++ -fPIC -shared).
//
// Order: everything recoverable first; the C++-exception case of std::call_once last (if unwinding
// through the shims were broken on Horizon it would end the process -- the log line before it says so).
#include "dusk/switch_tls_selftest.hpp"

#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_tls.hpp"
#include "dusk/switch_elf_memory.hpp"
#include "dusk/switch_native_loader.hpp"

#include <borealis/log.hpp>
#include <switch.h>

#include <atomic>
#include <barrier>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" {
void __register_frame(void* begin);
void* __aarch64_read_tp();

// The game's thread-local the self-test module imports (as std::call_once's pair is imported by catalog
// mods): reached through its accessor, never through its .dynsym value.
__thread int aleks2_test_host_var = 3;
void* aleks2_test_host_var_address() {
    return &aleks2_test_host_var;
}
}

namespace dusk::switchelf {
namespace fs = std::filesystem;
namespace elf = mods::elf;

namespace {
constexpr borealis::Log Log{"aleks2::tls-selftest"};

class Report {
public:
    explicit Report(const fs::path& path) : m_file(std::fopen(path.string().c_str(), "w")) {}
    ~Report() {
        if (m_file != nullptr) {
            std::fclose(m_file);
        }
    }
    void line(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        char buf[512];
        va_list ap;
        va_start(ap, format);
        std::vsnprintf(buf, sizeof buf, format, ap);
        va_end(ap);
        Log.info("{}", buf);
        if (m_file != nullptr) {
            std::fprintf(m_file, "%s\n", buf);
            std::fflush(m_file);  // a later crash must not lose what was already found
        }
    }
    void check(bool ok, const char* what) {
        ok ? ++m_pass : ++m_fail;
        line("%s %s", ok ? "PASS" : "FAIL", what);
    }
    int pass() const { return m_pass; }
    int fail() const { return m_fail; }

private:
    std::FILE* m_file;
    int m_pass = 0, m_fail = 0;
};

// libnx's free thread-local slots (what pthread_key_create hands out). Every slot taken here is given
// back before returning: the probe must not use up the quota it measures.
int free_tls_slots() {
    std::vector<s32> taken;
    for (;;) {
        const s32 slot = threadTlsAlloc(nullptr);
        if (slot < 0) {
            break;
        }
        taken.push_back(slot);
    }
    for (const s32 slot : taken) {
        threadTlsFree(slot);
    }
    return static_cast<int>(taken.size());
}

uint64_t read_tpidr_el0() {
    uint64_t v;
    asm volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

struct Mapped {
    elf::Loaded loaded;
    template <class F>
    F* fn(const char* name) const {
        return reinterpret_cast<F*>(static_cast<uintptr_t>(loaded.find(name)));
    }
};

// parse, plan with the production Linux tables, map into code memory, register .eh_frame, run the
// constructors: what load() + activation do for a mod, without the mod loader around it.
bool load(const std::vector<uint8_t>& file, Mapped& out, std::string& why) {
    const auto parsed = elf::parse(file);
    if (!parsed.image) {
        why = "parse: " + parsed.diagnostic.message;
        return false;
    }
    const auto plan = elf::plan(*parsed.image,
        linux_tls_resolver({{"aleks2_test_host_var", &aleks2_test_host_var_address}}),
        elf::make_needed_policy(elf::PackageAbi::Linux), true);
    if (!plan.ok()) {
        why = "plan: " + plan.problems.front().message;
        for (size_t i = 1; i < plan.problems.size() && i < 4; ++i) {
            why += " | " + plan.problems[i].message;
        }
        return false;
    }
    auto* memory = new CodeMemory;  // kept for the life of the process, as a mod's mapping is
    auto mapped = elf::map(*parsed.image, plan, *memory, &elf::tls::runtime());
    if (!mapped.loaded) {
        why = "map: " + mapped.diagnostic.message;
        return false;
    }
    out.loaded = std::move(*mapped.loaded);
    if (out.loaded.ehFrame != 0) {
        __register_frame(reinterpret_cast<void*>(static_cast<uintptr_t>(out.loaded.ehFrame)));
    }
    elf::run_constructors(out.loaded);
    return true;
}

// One thread's own block, as elf_tls_exec_test.cpp checks it. initialCounter: t_counter before this
// thread's first bump (42 on the loading thread, whose access the constructor already made).
bool thread_view(const Mapped& m, int initialCounter) {
    bool ok = m.fn<int()>("tls_bump")() == initialCounter + 1;
    ok &= m.fn<int()>("tls_pointer_value")() == 7;           // .tdata pointer relocated in the template
    ok &= m.fn<int()>("tls_local_bump")() == 1001;           // section-relative descriptor
    ok &= m.fn<int()>("tls_probe")() == 5;
    ok &= (m.fn<uintptr_t()>("tls_aligned_address")() & 63) == 0;
    uint64_t expect = 0;
    for (uint64_t i = 0; i < 1452; ++i) expect += 1000 + i;
    ok &= m.fn<uint64_t(uint64_t)>("tls_big_fill")(1000) == expect;  // .tbss, 11616 bytes, zero-filled
    ok &= std::string(m.fn<const char*()>("tls_tracked")()) == "constructed in this thread";
    return ok;
}

}  // namespace

void run_tls_selftest(const fs::path& modsDirectory) {
    const fs::path dir = modsDirectory.parent_path() / "tls-selftest";
    const fs::path library = dir / "tls_selftest.so";
    std::error_code ec;
    if (!fs::exists(library, ec)) {
        return;
    }
    Report r(dir / "result.txt");
    r.line("TLS-lab self-test (phase A): %s", library.string().c_str());

    // The thread pointers: libnx's (what the game's own TLS uses) and TPIDR_EL0 (what a Linux module reads).
    void* tlsPage = armGetTls();
    void* tlsTp = *reinterpret_cast<void**>(static_cast<uint8_t*>(tlsPage) + 0x1F8);
    r.line("TPIDRRO_EL0 %p, ThreadVars.tls_tp %p, __aarch64_read_tp() %p, TPIDR_EL0 %#llx", tlsPage, tlsTp,
        __aarch64_read_tp(), static_cast<unsigned long long>(read_tpidr_el0()));
    r.check(tlsTp != nullptr && tlsTp == __aarch64_read_tp(), "libnx thread pointer readable at TPIDRRO_EL0 + 0x1F8");
    const int slotsBefore = free_tls_slots();
    r.line("free libnx TLS slots before: %d", slotsBefore);
    if (const auto why = unavailable_reason()) {
        r.line("SKIP: native code cannot be mapped on this launch: %s", why->c_str());
        r.line("TLS SELFTEST RESULT: SKIPPED");
        return;
    }
    std::vector<uint8_t> file;
    if (std::FILE* f = std::fopen(library.string().c_str(), "rb")) {
        uint8_t chunk[64 * 1024];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
    }

    // A thread that exists before the module is loaded; it touches the module's TLS only afterwards.
    std::mutex mu;
    std::condition_variable cv;
    const Mapped* lab = nullptr;
    bool go = false, earlyOk = false;
    std::thread early([&] {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return go; });
        lock.unlock();
        const bool ok = lab != nullptr && thread_view(*lab, 41);
        lock.lock();
        earlyOk = ok;
    });

    // No key left: refused before anything is mapped or run; then it loads.
    std::string why;
    elf::tls::inject_fault(elf::tls::Fault::KeyCreate, 1);
    Mapped refused;
    const bool refusedLoaded = load(file, refused, why);
    r.check(!refusedLoaded && why.find("no thread-local key") != std::string::npos,
        "injected key failure: module refused before mapping (recoverable)");
    r.line("  (%s)", why.c_str());
    elf::tls::inject_fault(elf::tls::Fault::KeyCreate, 0);

    Mapped m;
    why.clear();
    const bool loaded = load(file, m, why);
    r.check(loaded, "module mapped with the real loader, constructors run");
    if (!loaded) {
        r.line("  (%s)", why.c_str());
        {
            std::lock_guard lock(mu);
            go = true;
        }
        cv.notify_all();
        early.join();
        r.line("TLS SELFTEST RESULT: FAIL (%d passed, %d failed)", r.pass(), r.fail());
        return;
    }
    r.line("mapped at %#llx, TLS module %llu", static_cast<unsigned long long>(m.loaded.runtimeBase),
        static_cast<unsigned long long>(m.loaded.tlsModule));
    r.check(free_tls_slots() == slotsBefore - 1, "exactly one libnx TLS slot taken by the runtime");

    r.check(thread_view(m, 42), "loading thread: initial values, zero fill, alignment, relocated .tdata");
    r.check(m.fn<int()>("tls_host_get")() == 3, "the game's thread-local reached through its accessor");
    m.fn<void(int)>("tls_host_set")(99);
    r.check(aleks2_test_host_var == 99, "the module wrote the game's thread-local of this thread");
    {
        std::lock_guard lock(mu);
        lab = &m;
        go = true;
    }
    cv.notify_all();
    early.join();
    r.check(earlyOk, "thread created before the module was loaded: its own block, initial values");

    // Four threads at once, each its own block; their thread_local destructors run as they exit.
    const int destroyedBefore = m.fn<int()>("tls_destroyed")();
    std::set<void*> addresses{m.fn<void*()>("tls_counter_address")()};
    std::atomic<int> ok{0};
    std::barrier sync(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            int last = 0;
            for (int i = 0; i < 10000; ++i) last = m.fn<int()>("tls_bump")();
            bool good = last == 41 + 10000 && m.fn<int()>("tls_host_get")() == 3;
            m.fn<void(int)>("tls_host_set")(t);
            good &= aleks2_test_host_var == t;
            good &= std::string(m.fn<const char*()>("tls_tracked")()) == "constructed in this thread";
            {
                std::lock_guard lock(mu);
                addresses.insert(m.fn<void*()>("tls_counter_address")());
            }
            sync.arrive_and_wait();
            if (good) ok.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();
    r.check(ok.load() == 4, "4 threads at once: 10000 increments each, no interference");
    r.check(addresses.size() == 5, "5 threads, 5 distinct blocks");
    r.check(m.fn<int()>("tls_destroyed")() == destroyedBefore + 4, "thread_local destructors ran at thread exit (4)");

    int slow = -1, fast = -1;
    std::thread([&] {
        slow = m.fn<int()>("tls_regs")();
        fast = m.fn<int()>("tls_regs")();
    }).join();
    r.line("register check: slow path %d, fast path %d (0 = every register kept)", slow, fast);
    r.check(slow == 0 && fast == 0, "resolver keeps x2-x29 and q0-q31 (slow and fast path, Switch assembly)");

    r.check(m.fn<int()>("once_basic")() == 1 && m.fn<int()>("once_basic")() == 1, "std::call_once: once, repeated");
    std::atomic<int> sawOne{0};
    threads.clear();
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            if (m.fn<int()>("once_concurrent_entry")() == 1) sawOne.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();
    r.check(sawOne.load() == 4 && m.fn<int()>("once_concurrent_count")() == 100,
        "std::call_once: 4 threads at once, one run, nobody returns early");
    r.check(m.fn<int()>("once_nested")() == 11, "std::call_once: nested, different flags");

    const auto s = elf::tls::stats();
    r.line("runtime: %llu module(s), %llu thread(s), %llu block(s) allocated, %llu freed, %llu destructor(s), "
           "%llu abandoned, key %s",
        (unsigned long long)s.modules, (unsigned long long)s.threads, (unsigned long long)s.blocks,
        (unsigned long long)s.freedBlocks, (unsigned long long)s.destructorsRun, (unsigned long long)s.abandoned,
        s.keyReady ? "ready" : "missing");
    r.check(s.freedBlocks >= 6 && s.destructorsRun >= 5, "blocks freed and destructors run at thread exit");
    r.line("free libnx TLS slots after: %d", free_tls_slots());

    r.line("next: std::call_once whose routine throws, then a retry (if the game stops here, it is this case)");
    r.check(m.fn<int()>("once_throw_retry")() == 21, "std::call_once: exception reaches the caller, retry runs once");

    r.line("TLS SELFTEST RESULT: %s (%d passed, %d failed)", r.fail() == 0 ? "PASS" : "FAIL", r.pass(), r.fail());
}

}  // namespace dusk::switchelf
