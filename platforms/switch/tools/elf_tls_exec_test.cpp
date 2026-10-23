// TLS-lab: dynamic TLS for mapped modules, really executed. Built for aarch64-linux and run under
// qemu-aarch64 by test-elf-tls.sh.
//
//   elf_tls_exec_test DIR [CATALOG_LIB...]
//
// DIR holds tls_desc.so, tls_trad.so and tls_ie.so (elf_tls_test_module.cpp, three TLS dialects).
// It maps them with src/dusk/mods/loader/elf_loader.cpp and the runtime in elf_tls.cpp (POSIX
// memory: mmap, then mprotect to the loader's permissions) and checks:
//
//   * initial values (.tdata), zeroes (.tbss, 11616 bytes), a pointer in .tdata relocated in the
//     template, 64-byte alignment, symbol and section (local) descriptors;
//   * a constructor that touches a thread-local before any other code of the module runs;
//   * a thread that already existed when the module was loaded, then 8 threads at once: each its own
//     block, initial values, no interference;
//   * thread_local destructors (__cxa_thread_atexit) run at thread exit, and the blocks freed;
//   * the descriptor calling convention: x2-x29 and all of q0-q31 preserved, slow and fast path;
//   * a thread-local imported from the "game" (this executable) through an accessor;
//   * the traditional dialect (__tls_get_addr, DTPMOD64/DTPREL64);
//   * refusals: initial-exec (TPREL64, DF_STATIC_TLS) with TLS on; any TLS with TLS off.
//
// Then each CATALOG_LIB (a published mod's lib/linux-aarch64/mod.so) is planned with TLS off and on
// against a game that defines every name, and mapped without running it: what is left once TLS is
// provided is printed, and nothing thread-local may be among it.
//
// Under qemu-user this is Linux: TPIDR_EL0 is glibc's thread pointer, and the runtime finds its
// own per-thread table through this executable's TLS (TPIDR_EL0 too). On Switch the module still
// reads TPIDR_EL0 and the runtime reads libnx's TPIDRRO_EL0 + 0x1F8 -- the one difference, a macro
// in elf_tls_aarch64.S. The resolver's result does not depend on TPIDR_EL0's value either way.
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"
#include "dusk/mods/loader/elf_tls.hpp"

#include <dlfcn.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace dusk::mods::elf;

extern "C" void __register_frame(void*);

extern "C" {
// The game's thread-local a module imports (elf_tls_test_module.cpp: aleks2_test_host_var).
__thread int aleks2_test_host_var = 3;
void* aleks2_test_host_var_address() { return &aleks2_test_host_var; }
}

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

struct PosixMemory : MemoryBackend {
    uint8_t* base = nullptr;
    bool reserve(uint64_t sz, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase, std::string& error) override {
        void* raw = mmap(nullptr, sz + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            error = "mmap failed";
            return false;
        }
        const auto aligned = (reinterpret_cast<uintptr_t>(raw) + align - 1) & ~(align - 1);
        base = reinterpret_cast<uint8_t*>(aligned);
        staging = base;
        runtimeBase = aligned;
        return true;
    }
    bool publish(const std::vector<Protection>& ranges, std::string& error) override {
        for (const auto& r : ranges) {
            if (r.perm & kPermX) {
                __builtin___clear_cache(reinterpret_cast<char*>(base + r.offset), reinterpret_cast<char*>(base + r.offset + r.size));
            }
        }
        for (const auto& r : ranges) {
            const int prot = (r.perm & kPermR ? PROT_READ : 0) | (r.perm & kPermW ? PROT_WRITE : 0) |
                             (r.perm & kPermX ? PROT_EXEC : 0);
            if (mprotect(base + r.offset, r.size, prot) != 0) {
                error = "mprotect failed";
                return false;
            }
        }
        return true;
    }
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The tables switch_native_loader.cpp builds for a Linux module with TLS (linux_tls_shims, tls_imports):
// the production shims and accessors of elf_tls.cpp and elf_tls_once.cpp, not copies.
std::vector<Shim> all_shims() {
    auto table = shims();
    for (const auto& s : tls::shims()) {
        table.push_back(s);
    }
    for (const auto& s : tls::once_shims()) {
        table.push_back(s);
    }
    return table;
}

std::vector<tls::HostVariable> host_tls_variables() {
    auto v = tls::once_variables();
    v.push_back({"aleks2_test_host_var", &aleks2_test_host_var_address});
    return v;
}

Resolver resolver_for_this_process() {
    const auto game = [](std::string_view name) -> std::optional<uint64_t> {
        void* p = dlsym(RTLD_DEFAULT, std::string(name).c_str());
        if (p == nullptr) return std::nullopt;
        return reinterpret_cast<uintptr_t>(p);
    };
    return make_resolver(PackageAbi::Linux, game, all_shims(), image_runtime(), tls::host_variables(host_tls_variables()));
}

struct Module {
    std::string path;
    std::vector<uint8_t> file;
    std::optional<ParseResult> parsed;
    Loaded loaded;
    template <class F>
    F* fn(const char* name) const {
        return reinterpret_cast<F*>(static_cast<uintptr_t>(loaded.find(name)));
    }
};

bool load(const std::string& path, Module& m) {
    m.path = path;
    m.file = read_file(path);
    m.parsed = parse(m.file);
    if (!m.parsed->image) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), m.parsed->diagnostic.message.c_str());
        return false;
    }
    const auto p = plan(*m.parsed->image, resolver_for_this_process(), make_needed_policy(PackageAbi::Linux), true);
    if (!p.ok()) {
        for (const auto& pr : p.problems) std::fprintf(stderr, "%s: %s: %s\n", path.c_str(), error_code(pr.error), pr.message.c_str());
        return false;
    }
    auto* memory = new PosixMemory;  // kept for the life of the process, as the game keeps modules
    auto mapped = map(*m.parsed->image, p, *memory, &tls::runtime());
    if (!mapped.loaded) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), mapped.diagnostic.message.c_str());
        return false;
    }
    m.loaded = std::move(*mapped.loaded);
    // As switch_native_loader.cpp does: exceptions cross the module's frames (std::call_once's retry).
    if (m.loaded.ehFrame != 0) {
        __register_frame(reinterpret_cast<void*>(static_cast<uintptr_t>(m.loaded.ehFrame)));
    }
    run_constructors(m.loaded);
    std::printf("%s: mapped at %#lx, TLS module %lu, %zu constructor(s) run\n", path.c_str(),
        static_cast<unsigned long>(m.loaded.runtimeBase), static_cast<unsigned long>(m.loaded.tlsModule),
        m.loaded.constructors.size());
    return true;
}

// The checks one thread makes of its own block. initialCounter: what t_counter must be before the
// first bump in this thread (42 on the loading thread: the constructor bumped it).
void check_thread_view(const Module& m, int initialCounter, const char* who) {
    CHECK(m.fn<int()>("tls_bump")() == initialCounter + 1);
    CHECK(m.fn<int()>("tls_pointer_value")() == 7);
    CHECK(m.fn<int()>("tls_local_bump")() == 1001);
    CHECK(m.fn<int()>("tls_probe")() == 5);
    CHECK((m.fn<uintptr_t()>("tls_aligned_address")() & 63) == 0);
    // .tbss starts at zero: the first fill's sum is the formula's.
    const uint64_t seed = 1000;
    uint64_t expect = 0;
    for (uint64_t i = 0; i < 1452; ++i) expect += seed + i;
    CHECK(m.fn<uint64_t(uint64_t)>("tls_big_fill")(seed) == expect);
    CHECK(std::string(m.fn<const char*()>("tls_tracked")()) == "constructed in this thread");
    std::printf("  %s: own block checked\n", who);
}

void test_module(const std::string& path, bool importsHost) {
    Module m;
    CHECK(load(path, m));
    if (m.loaded.runtimeBase == 0) return;
    CHECK(m.loaded.tlsModule != 0);

    // A thread that exists before the module is loaded and touches it only afterwards is covered
    // in main(); here: the loading thread, then 8 at once.
    check_thread_view(m, 42, "loading thread");

    if (importsHost) {
        // This thread's instance, whatever an earlier module left in it (3 initially).
        CHECK(m.fn<int()>("tls_host_get")() == aleks2_test_host_var);
        m.fn<void(int)>("tls_host_set")(99);
        CHECK(aleks2_test_host_var == 99);  // the module wrote this thread's instance of the game's variable
    }

    const int destroyedBefore = m.fn<int()>("tls_destroyed")();
    std::mutex mu;
    std::set<void*> addresses{m.fn<void*()>("tls_counter_address")()};
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    // All eight stay alive until each has recorded its block: a thread that exits frees its block, and
    // malloc may hand the same address to the next one.
    std::barrier sync(8);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            int last = 0;
            for (int i = 0; i < 10000; ++i) last = m.fn<int()>("tls_bump")();
            if (last == 41 + 10000) ok.fetch_add(1);
            if (importsHost) {
                if (m.fn<int()>("tls_host_get")() != 3) ok.fetch_sub(100);  // a fresh thread sees the initial value
                m.fn<void(int)>("tls_host_set")(t);
                if (aleks2_test_host_var != t) ok.fetch_sub(100);
            }
            if (std::string(m.fn<const char*()>("tls_tracked")()) != "constructed in this thread") ok.fetch_sub(100);
            {
                std::lock_guard lock(mu);
                addresses.insert(m.fn<void*()>("tls_counter_address")());
            }
            sync.arrive_and_wait();
        });
    }
    for (auto& t : threads) t.join();
    CHECK(ok.load() == 8);
    CHECK(addresses.size() == 9);  // nine threads, nine blocks
    CHECK(m.fn<int()>("tls_destroyed")() == destroyedBefore + 8);  // each thread's Tracked, at its exit

    // The calling convention, on a fresh thread: its first access takes the slow path, the second
    // the fast one.
    int first = -1, second = -1;
    std::thread([&] {
        first = m.fn<int()>("tls_regs")();
        second = m.fn<int()>("tls_regs")();
    }).join();
    if (first != 0 || second != 0) std::fprintf(stderr, "tls_regcheck: slow path %d, fast path %d\n", first, second);
    CHECK(first == 0 && second == 0);
    std::printf("  8 threads: own blocks, no interference, destructors run; registers preserved (slow and fast path)\n");
}

// The problems, grouped: denials by reason (each with a count and one example), the rest in full.
std::string summary(const Plan& p) {
    std::vector<std::pair<std::string, std::pair<int, std::string>>> groups;
    for (const auto& pr : p.problems) {
        std::string key = error_code(pr.error);
        std::string example = pr.message;
        if (pr.error == Error::DeniedSymbol) {
            const auto colon = pr.message.find("refused: ");
            key += ": " + (colon == std::string::npos ? pr.message : pr.message.substr(colon + 9, 60));
            example = pr.message.substr(0, pr.message.find(' '));
        }
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == key; });
        if (it == groups.end()) {
            groups.push_back({key, {1, example}});
        } else {
            ++it->second.first;
        }
    }
    std::string s;
    for (const auto& [key, v] : groups) {
        s += "\n      " + key + " x" + std::to_string(v.first) + " (e.g. " + v.second.substr(0, 160) + ")";
    }
    return s.empty() ? " none" : s;
}

bool thread_local_problem(const Diagnostic& d) {
    const std::string& m = d.message;
    return d.error == Error::TlsUnsupported || m.find("TLS") != std::string::npos ||
           m.find("thread-local") != std::string::npos || m.find("__tls_get_addr") != std::string::npos ||
           m.find("__cxa_thread_atexit") != std::string::npos;
}

[[noreturn]] void unresolved_stub() {
    std::fprintf(stderr, "a catalog module called into the fake game\n");
    std::abort();
}

void catalog(const std::vector<std::string>& libs) {
    if (libs.empty()) return;
    std::printf("== catalog libraries: planned with TLS off and on, mapped (not run) with it on\n");
    // A game that defines every name (the real surface is judged by the DM1-b audit): what remains
    // is what the policy itself refuses.
    const auto everything = [](std::string_view) -> std::optional<uint64_t> {
        return reinterpret_cast<uintptr_t>(&unresolved_stub);
    };
    for (const auto& path : libs) {
        const auto file = read_file(path);
        const auto parsed = parse(file);
        if (!parsed.image) {
            std::printf("  %s: parse: %s\n", path.c_str(), parsed.diagnostic.message.c_str());
            CHECK(false);
            continue;
        }
        // TLS off: the shims a build without ALEKS2_MOD_TLS has. TLS on: the production tables, with
        // std::call_once's two thread-locals reached through their real accessors.
        const auto off = plan(*parsed.image, make_resolver(PackageAbi::Linux, everything, shims(), image_runtime()),
            make_needed_policy(PackageAbi::Linux), false);
        const auto on = plan(*parsed.image,
            make_resolver(PackageAbi::Linux, everything, all_shims(), image_runtime(),
                tls::host_variables(tls::once_variables())),
            make_needed_policy(PackageAbi::Linux), true);
        bool tlsLeft = false;
        for (const auto& pr : on.problems) tlsLeft |= thread_local_problem(pr);
        CHECK(!tlsLeft);
        bool tlsBefore = false;
        for (const auto& pr : off.problems) tlsBefore |= thread_local_problem(pr);
        std::string mapped = "not mapped (other problems)";
        if (on.ok()) {
            PosixMemory memory;
            const auto r = map(*parsed.image, on, memory, &tls::runtime());
            CHECK(r.loaded.has_value());
            mapped = r.loaded ? "mapped, TLS module " + std::to_string(r.loaded->tlsModule) : "map failed: " + r.diagnostic.message;
        }
        const auto& t = parsed.image->tls;
        std::printf("  %s\n    PT_TLS %s; TLS a problem without TLS: %s; %s\n    problems left with TLS:%s\n",
            path.c_str(),
            t ? ("filesz " + std::to_string(t->filesz) + " memsz " + std::to_string(t->memsz) + " align " + std::to_string(t->align)).c_str() : "none",
            tlsBefore ? "yes" : "no", mapped.c_str(), summary(on).c_str());
    }
}

// std::call_once from a Linux module, end to end: module -> libstdc++'s inline call_once -> the two
// thread-locals through their production accessors -> the production __once_proxy and pthread_once
// shims (elf_tls_once.cpp). Here pthread_once is the shim, on glibc's pthreads and libstdc++ (qemu);
// on Switch the same shim runs on newlib/libnx -- the self-test checks that.
void test_call_once(const Module& m) {
    std::printf("== std::call_once through the production shims\n");
    const auto file = read_file(m.path);
    const auto parsed = parse(file);
    if (parsed.image) {
        const auto p = plan(*parsed.image, resolver_for_this_process(), make_needed_policy(PackageAbi::Linux), true);
        const auto source = [&](std::string_view name) {
            for (const auto& d : p.imports) {
                if (d.name == name) return std::make_pair(d.source, d.address);
            }
            return std::make_pair(Source::Missing, uint64_t{0});
        };
        CHECK(source("__once_proxy") ==
              std::make_pair(Source::Shim, uint64_t(reinterpret_cast<uintptr_t>(&aleks2_shim_once_proxy_linux))));
        CHECK(source("pthread_once") ==
              std::make_pair(Source::Shim, uint64_t(reinterpret_cast<uintptr_t>(&aleks2_shim_pthread_once_linux))));
        CHECK(source("_ZSt11__once_call").first == Source::Runtime);
        CHECK(source("_ZSt15__once_callable").first == Source::Runtime);
    }
    CHECK(m.fn<int()>("once_basic")() == 1);
    CHECK(m.fn<int()>("once_basic")() == 1);  // and again: still one run
    std::vector<std::thread> threads;
    std::atomic<int> sawOne{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            if (m.fn<int()>("once_concurrent_entry")() == 1) sawOne.fetch_add(1);
        });
    }
    for (auto& t : threads) t.join();
    CHECK(sawOne.load() == 8);                               // nobody returned before the routine ended
    CHECK(m.fn<int()>("once_concurrent_count")() == 100);    // one run, no overlap
    CHECK(m.fn<int()>("once_nested")() == 11);
    CHECK(m.fn<int()>("once_throw_retry")() == 21);          // thrown, retried, then no more runs
    std::printf("  once, repeated, 8 threads at once, nested flags, exception then retry: as std::call_once says\n");
}

// A backend whose publish fails: map() must abandon the template it registered.
struct FailingPublish : PosixMemory {
    bool publish(const std::vector<Protection>&, std::string& error) override {
        error = "injected publish failure";
        return false;
    }
};

// --fault=key|setspecific|alloc DIR: one fault, in a fresh process (the key is created once per process).
int fault_mode(const std::string& fault, const std::string& dir) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (fault == "key") {
        // No key left: map() refuses the module before any of its code runs; once one is, it loads.
        tls::inject_fault(tls::Fault::KeyCreate, 1);
        Module m;
        CHECK(!load(dir + "/tls_desc.so", m));
        CHECK(m.loaded.runtimeBase == 0 && m.loaded.constructors.empty());
        CHECK(!tls::stats().keyReady && tls::stats().modules == 0);
        Module again;
        CHECK(load(dir + "/tls_desc.so", again));
        CHECK(tls::stats().keyReady);
        if (again.loaded.runtimeBase != 0) check_thread_view(again, 42, "after the key fault, retried");
        std::printf("fault key: refused cleanly (no constructor run), loaded on retry\n");
        return g_failures ? 1 : 0;
    }
    // Inside a TLS access the only safe outcome is to stop: these must abort, with the reason first.
    Module m;
    if (!load(dir + "/tls_desc.so", m)) return 1;
    tls::inject_fault(fault == "setspecific" ? tls::Fault::SetSpecific : tls::Fault::Alloc, 1);
    std::thread([&] { m.fn<int()>("tls_bump")(); }).join();
    std::fprintf(stderr, "fault %s: the TLS access returned instead of stopping\n", fault.c_str());
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string_view(argv[1]).starts_with("--fault=")) {
        return fault_mode(std::string(argv[1] + 8), argv[2]);
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s DIR [CATALOG_LIB...]\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string dir = argv[1];

    // A thread that exists before any module with TLS is loaded, parked until one is.
    std::mutex mu;
    std::condition_variable cv;
    bool loaded = false, done = false;
    const Module* lab = nullptr;
    int preExistingFailures = -1;
    std::thread early([&] {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return loaded; });
        const int before = g_failures;
        lock.unlock();
        check_thread_view(*lab, 41, "thread created before the module was loaded");
        lock.lock();
        preExistingFailures = g_failures - before;
        done = true;
        cv.notify_all();
    });

    std::printf("== TLSDESC (the catalog's dialect)\n");
    Module desc;
    CHECK(load(dir + "/tls_desc.so", desc));
    if (desc.loaded.runtimeBase != 0) {
        {
            std::lock_guard lock(mu);
            lab = &desc;
            loaded = true;
        }
        cv.notify_all();
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return done; });
        }
        early.join();
        CHECK(preExistingFailures == 0);
        test_call_once(desc);
        test_module(dir + "/tls_desc.so", true);  // a second mapping of the same file: its own TLS module
    } else {
        { std::lock_guard lock(mu); loaded = true; lab = nullptr; }
        early.detach();
    }

    std::printf("== traditional dialect (__tls_get_addr, DTPMOD64/DTPREL64)\n");
    {
        const auto file = read_file(dir + "/tls_trad.so");
        const auto parsed = parse(file);
        CHECK(parsed.image.has_value());
        if (parsed.image) {
            const auto p = plan(*parsed.image, resolver_for_this_process(), make_needed_policy(PackageAbi::Linux), true);
            uint64_t dtpmod = 0;
            for (const auto& [type, n] : p.relocationCounts) dtpmod += type == kRelTlsDtpMod64 ? n : 0;
            CHECK(dtpmod != 0);
            bool getAddr = false;
            for (const auto& d : p.imports) getAddr |= d.name == "__tls_get_addr" && d.source == Source::Shim;
            CHECK(getAddr);
        }
        test_module(dir + "/tls_trad.so", true);
    }

    std::printf("== refusals\n");
    {
        const auto file = read_file(dir + "/tls_ie.so");
        const auto parsed = parse(file);
        CHECK(parsed.image.has_value());
        if (parsed.image) {
            const auto p = plan(*parsed.image, resolver_for_this_process(), make_needed_policy(PackageAbi::Linux), true);
            CHECK(!p.ok() && p.problems.front().error == Error::TlsUnsupported);
            if (!p.ok()) std::printf("  initial-exec, TLS on: %s: %s\n", error_code(p.problems.front().error), p.problems.front().message.c_str());
        }
        const auto descFile = read_file(dir + "/tls_desc.so");
        const auto descParsed = parse(descFile);
        if (descParsed.image) {
            const auto p = plan(*descParsed.image, resolver_for_this_process(), make_needed_policy(PackageAbi::Linux), false);
            CHECK(!p.ok() && p.problems.front().error == Error::TlsUnsupported);
            if (!p.ok()) std::printf("  TLSDESC, TLS off: %s: %s\n", error_code(p.problems.front().error), p.problems.front().message.c_str());
            // map() refuses a TLS module without a provider even if handed a plan made with TLS on.
            const auto on = plan(*descParsed.image, resolver_for_this_process(), make_needed_policy(PackageAbi::Linux), true);
            PosixMemory memory;
            CHECK(on.ok() && !map(*descParsed.image, on, memory, nullptr).loaded);
            // A failure after the template is registered abandons it: no descriptor reaches it.
            const uint64_t abandonedBefore = tls::stats().abandoned;
            FailingPublish failing;
            CHECK(!map(*descParsed.image, on, failing, &tls::runtime()).loaded);
            CHECK(tls::stats().abandoned == abandonedBefore + 1);
            std::printf("  publish failure after registering: template abandoned\n");
        }
    }

    std::vector<std::string> libs(argv + 2, argv + argc);
    catalog(libs);

    const auto s = tls::stats();
    std::printf("TLS runtime: %lu module(s), %lu thread(s), %lu block(s) allocated, %lu freed, %lu destructor(s) run\n",
        (unsigned long)s.modules, (unsigned long)s.threads, (unsigned long)s.blocks, (unsigned long)s.freedBlocks,
        (unsigned long)s.destructorsRun);
    CHECK(s.freedBlocks > 0 && s.destructorsRun >= 16);
    if (g_failures) {
        std::fprintf(stderr, "elf_tls_exec_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("elf_tls_exec_test: all checks passed\n");
    return 0;
}
