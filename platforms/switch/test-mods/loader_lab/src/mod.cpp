// H37 Level 0: a native mod that exercises what a loader has to get right, and says so in
// the log. Every line starts with [h37-lab]:
//
//   constructors   a C constructor (.init_array) and a C++ static object with a std::string
//   relocations    a table of pointers (RELATIVE), a vtable (ABS64/RELATIVE), library calls
//                  (JUMP_SLOT), library data (GLOB_DAT)
//   C++ runtime    std::string, std::vector, std::sort, operator new/delete
//   exceptions     throws and catches std::runtime_error inside mod_initialize: needs the
//                  module's .eh_frame registered with the unwinder
//   lifecycle      init/update/shutdown, any number of times on the same mapping (the
//                  loader never unmaps a module, so re-enabling reuses it)
//
// It registers nothing, hooks nothing and writes no file.
#include "mods/service.hpp"
#include "mods/svc/log.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);

namespace {

int g_cConstructor = 0;
__attribute__((constructor)) void lab_c_constructor() {
    g_cConstructor = 0x4C4142;  // "LAB"
}

struct Greeting {
    std::string text;
    Greeting() : text("static object constructed") {}
};
Greeting g_greeting;

struct Shape {
    virtual ~Shape() = default;
    virtual int sides() const = 0;
};
struct Triangle final : Shape {
    int sides() const override { return 3; }
};
struct Square final : Shape {
    int sides() const override { return 4; }
};

const char* const kWords[] = {"delta", "alpha", "charlie", "bravo"};

int g_updates = 0;
int g_cycle = 0;

void say(const char* format, int a = 0, int b = 0) {
    char line[160];
    std::snprintf(line, sizeof line, format, a, b);
    std::string message = "[h37-lab] ";
    message += line;
    svc_log->info(mod_ctx, message.c_str());
}

int sum_of_sides() {
    std::vector<Shape*> shapes{new Triangle, new Square, new Triangle};
    int total = 0;
    for (const Shape* s : shapes) {
        total += s->sides();
        delete s;
    }
    return total;
}

}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ++g_cycle;
    g_updates = 0;
    say("init cycle %d", g_cycle);
    svc_log->info(mod_ctx, g_cConstructor == 0x4C4142 ? "[h37-lab] c constructor ran" : "[h37-lab] c constructor DID NOT RUN");
    svc_log->info(mod_ctx, (std::string("[h37-lab] ") + g_greeting.text).c_str());

    std::vector<std::string> words(std::begin(kWords), std::end(kWords));
    std::sort(words.begin(), words.end());
    std::string joined;
    for (const auto& w : words) {
        joined += (joined.empty() ? "" : ",") + w;
    }
    svc_log->info(mod_ctx, ("[h37-lab] sorted " + joined).c_str());
    say("virtual calls: %d sides", sum_of_sides());

    try {
        throw std::runtime_error("thrown inside the module");
    } catch (const std::runtime_error& e) {
        svc_log->info(mod_ctx, (std::string("[h37-lab] caught: ") + e.what()).c_str());
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    ++g_updates;
    if (g_updates % 100 == 0) {
        say("update %d (cycle %d)", g_updates, g_cycle);
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    say("shutdown after %d update(s), cycle %d", g_updates, g_cycle);
    return MOD_OK;
}
}
