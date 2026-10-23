// DM2-b: lab::run as a plain Linux program (glibc, libstdc++ 13), under qemu-aarch64: the reference the
// mod's results on Switch are compared with. usage: iostream_lab_host DIR/
#include "../src/lab.hpp"

#include <cstdio>

int main(int argc, char** argv) {
    lab::Report report;
    report.say = [](const char* line) { std::puts(line); };
    lab::run(argc > 1 ? argv[1] : "/tmp/", report);
    std::printf("[iostream-lab] RESULT: %d passed, %d failed%s%s\n", report.passed, report.failed,
                report.failed ? " -- " : "", report.failures.c_str());
    return report.failed == 0 ? 0 : 1;
}
