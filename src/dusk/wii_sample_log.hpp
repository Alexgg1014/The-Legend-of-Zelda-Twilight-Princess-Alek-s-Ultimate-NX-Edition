#pragma once
// aleks2 (V1-rc12): the raw motion samples of a Wii-style motion session, one line each, so a console run can be replayed on
// the host (platforms/switch/tools/wii_sample_log_test.cpp) and HID's fused attitude (HidDirectionState) can be checked
// against our own gravity tracking before the classifier relies on it. Written by wii_controls.cpp when perf-lab.ini has
// wii_record = 1 (developer mode).
//
// Line: side,t,ax,ay,az,gx,gy,gz,flags,d0,d1,d2,d3,d4,d5,d6,d7,d8
//   side   0 left Joy-Con, 1 right, 2 the only sensor
//   t      seconds on that sensor's own clock (wii_controls.cpp, s_clock)
//   a*     m/s^2, gravity included; g* rad/s
//   flags  aurora::device::MotionSample::flags (1 gap before, 2 attitude present)
//   d*     HID's attitude, row-major, as given
// A line starting with '#' is a comment (the header says the build and the layout).

#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace dusk::wii {

struct LoggedSample {
    int side = 0;
    double t = 0.0;
    std::array<float, 3> accel{};
    std::array<float, 3> gyro{};
    unsigned flags = 0;
    std::array<float, 9> dir{};
};

inline std::string format_sample_line(const LoggedSample& s) {
    char buf[384];
    const int n = std::snprintf(buf, sizeof buf,
        "%d,%.6f,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%u,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%.5g\n", s.side, s.t,
        s.accel[0], s.accel[1], s.accel[2], s.gyro[0], s.gyro[1], s.gyro[2], s.flags, s.dir[0], s.dir[1], s.dir[2],
        s.dir[3], s.dir[4], s.dir[5], s.dir[6], s.dir[7], s.dir[8]);
    return n > 0 ? std::string(buf, static_cast<size_t>(n) < sizeof buf ? static_cast<size_t>(n) : sizeof buf - 1)
                 : std::string{};
}

inline std::optional<LoggedSample> parse_sample_line(std::string_view line) {
    if (line.empty() || line.front() == '#') {
        return std::nullopt;
    }
    const std::string text(line);
    LoggedSample s;
    unsigned flags = 0;
    const int got = std::sscanf(text.c_str(), "%d,%lf,%f,%f,%f,%f,%f,%f,%u,%f,%f,%f,%f,%f,%f,%f,%f,%f", &s.side, &s.t,
        &s.accel[0], &s.accel[1], &s.accel[2], &s.gyro[0], &s.gyro[1], &s.gyro[2], &flags, &s.dir[0], &s.dir[1],
        &s.dir[2], &s.dir[3], &s.dir[4], &s.dir[5], &s.dir[6], &s.dir[7], &s.dir[8]);
    if (got != 18 || s.side < 0 || s.side > 2) {
        return std::nullopt;
    }
    s.flags = flags;
    return s;
}

}  // namespace dusk::wii
