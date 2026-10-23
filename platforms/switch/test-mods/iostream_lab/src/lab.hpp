// DM2-b: the checks of the iostream lab mod, shared by the mod (src/mod.cpp, run by the game's loader on
// Switch) and a plain Linux program (host/main.cpp, run under qemu-aarch64 with glibc: the reference).
// Built with Ubuntu's aarch64-linux-gnu-g++ either way, so its inline code is laid out for glibc and
// libstdc++ 13 -- what a published mod is.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <locale>
#include <new>
#include <regex>
#include <sstream>
#include <string>
#include <typeinfo>

namespace lab {

struct Report {
    int passed = 0;
    int failed = 0;
    std::string failures;
    void (*say)(const char*) = nullptr;

    void check(bool ok, const char* what) {
        char line[200];
        std::snprintf(line, sizeof line, "[iostream-lab] %s %s", ok ? "PASS" : "FAIL", what);
        if (say != nullptr) {
            say(line);
        }
        if (ok) {
            ++passed;
        } else {
            ++failed;
            failures += failures.empty() ? "" : ", ";
            failures += what;
        }
    }
};

// A Linux class derived from ifstream: its constructor runs ifstream's C2 with the derived VTT, and its own member
// sits after the whole glibc-layout object.
struct LabIfstream : std::ifstream {
    unsigned canary = 0xC0FFEEu;
    explicit LabIfstream(const char* p) : std::ifstream(p) {}
};

// The checks sol's review asked for (respuesta-sol-dm2b.md): destruction through the base with canaries around the
// object, a derived Linux class (C2/VTT), casts in every direction, and the 256 byte values through ctype<char>
// with the default, classic(), named and a stream's locale, inline and out of line. `dir` holds the scratch file.
inline void run_sol_checks(const std::string& dir, Report& r) {
    const std::string path = dir + "iostream-lab.txt";
    {
        std::ofstream out(path.c_str());
        out << "canary 7\n";
    }
    {  // canaries: an ifstream placed between two guard blocks, destroyed through istream* (virtual, D1 via the vtable)
        constexpr unsigned kGuard = 0x5A5A5A5Au;
        struct alignas(16) Block {
            unsigned before[16];
            alignas(16) unsigned char storage[sizeof(std::ifstream)];
            unsigned after[16];
        };
        static Block block;
        for (unsigned& g : block.before) g = kGuard;
        for (unsigned& g : block.after) g = kGuard;
        auto* f = new (block.storage) std::ifstream(path.c_str());
        std::string word;
        *f >> word;
        std::istream* base = f;
        base->~basic_istream();  // virtual: the most derived destructor
        bool intact = word == "canary";
        for (unsigned g : block.before) intact &= g == kGuard;
        for (unsigned g : block.after) intact &= g == kGuard;
        r.check(intact, "canaries intact around an ifstream destroyed through istream*");
    }
    {  // a derived Linux class: C2 + VTT, its own member after the glibc-layout base, deleted through the base
        auto* d = new LabIfstream(path.c_str());
        std::string word;
        int n = 0;
        *d >> word >> n;
        const bool ok = word == "canary" && n == 7 && d->canary == 0xC0FFEEu && d->is_open();
        std::istream* base = d;
        r.check(ok && dynamic_cast<LabIfstream*>(base) == d, "derived Linux ifstream (C2/VTT) reads, keeps its member");
        delete base;
        r.check(true, "derived Linux ifstream deleted through istream*");
    }
    {  // casts, every direction a module can take
        std::fstream io(path.c_str(), std::ios::in | std::ios::out);
        std::iostream* both = &io;
        std::istream* in = both;
        std::ostream* out = both;
        std::ios_base* root = in;
        const bool ok = dynamic_cast<std::fstream*>(in) == &io && dynamic_cast<std::fstream*>(out) == &io &&
                        dynamic_cast<std::ostream*>(in) == out && dynamic_cast<std::istream*>(out) == in &&
                        dynamic_cast<std::ifstream*>(in) == nullptr && dynamic_cast<std::ofstream*>(out) == nullptr &&
                        dynamic_cast<std::fstream*>(root) == &io && typeid(*in) == typeid(std::fstream) &&
                        typeid(*in) != typeid(std::ifstream);
        r.check(ok, "casts up, down, across and to the wrong class; typeid");
        std::ifstream f(path.c_str());
        std::istream& asIn = f;
        r.check(typeid(asIn) == typeid(std::ifstream) && dynamic_cast<std::ifstream*>(&asIn) == &f,
                "typeid and dynamic_cast of an ifstream through istream&");
    }
    // ctype<char>, 256 byte values, against the C functions (glibc's own tables in the reference, the NRO's here)
    const auto agree = [](const std::locale& loc) {
        const auto& ct = std::use_facet<std::ctype<char>>(loc);
        int bad = 0;
        for (int i = 0; i < 256; ++i) {
            const char c = static_cast<char>(i);
            bad += (std::isalpha(c, loc) != (isalpha(i) != 0));
            bad += (std::isdigit(c, loc) != (isdigit(i) != 0));
            bad += (std::isspace(c, loc) != (isspace(i) != 0));
            bad += (std::isupper(c, loc) != (isupper(i) != 0));
            bad += (std::ispunct(c, loc) != (ispunct(i) != 0));
            bad += (std::isxdigit(c, loc) != (isxdigit(i) != 0));
            bad += (ct.is(std::ctype_base::alnum, c) != (isalnum(i) != 0));
            bad += (ct.is(std::ctype_base::graph, c) != (isgraph(i) != 0));
            const unsigned char u = static_cast<unsigned char>(i);
            bad += (static_cast<unsigned char>(ct.toupper(c)) != (u < 128 ? toupper(i) : u));
        }
        const char text[] = "ab 12";
        bad += ct.scan_is(std::ctype_base::digit, text, text + 5) != text + 3;
        bad += ct.scan_not(std::ctype_base::alpha, text, text + 5) != text + 2;
        return bad;
    };
    r.check(agree(std::locale()) == 0, "ctype<char>: 256 bytes, default locale");
    r.check(agree(std::locale::classic()) == 0, "ctype<char>: 256 bytes, locale::classic()");
    r.check(agree(std::locale("C")) == 0, "ctype<char>: 256 bytes, locale(\"C\")");
    std::ostringstream s;
    r.check(agree(s.getloc()) == 0, "ctype<char>: 256 bytes, a stream's locale");
}

// `dir` ends with a slash. Every check is independent: a failure never stops the others.
inline void run(const std::string& dir, Report& r) {
    const std::string path = dir + "iostream-lab.txt";

    // The layout this module was compiled with: glibc's.
    r.check(sizeof(std::ifstream) == 528 && sizeof(std::ofstream) == 520 && sizeof(std::filebuf) == 248,
            "sizes are glibc's (ifstream 528, ofstream 520, filebuf 248)");

    {  // ofstream(const char*), operator<<, is_open, close
        std::ofstream out(path.c_str());
        r.check(out.is_open(), "ofstream(const char*) opens");
        out << "alpha 12\n" << "beta " << 34 << '\n';
        r.check(out.good(), "ofstream writes");
        out.close();
        r.check(!out.is_open(), "ofstream close");
    }
    {  // ofstream(std::string, app), open() later
        std::ofstream out;
        r.check(!out.is_open(), "ofstream() starts closed");
        out.open(path.c_str(), std::ios::app);
        r.check(out.is_open(), "ofstream::open(app)");
        out << "gamma 56\n";
    }
    {  // ifstream(std::string), getline, operator>>
        std::ifstream in(path);
        r.check(in.is_open(), "ifstream(std::string) opens");
        std::string line;
        std::getline(in, line);
        r.check(line == "alpha 12", "getline reads the first line");
        std::string word;
        int number = 0;
        in >> word >> number;
        r.check(word == "beta" && number == 34, "operator>> reads a word and an int");
        in >> word >> number;
        r.check(word == "gamma" && number == 56, "appended line is there");
        in >> word;
        r.check(in.eof() && in.fail(), "end of file sets eof and fail");
    }
    {  // fstream, read and write on one object
        std::fstream io(path.c_str(), std::ios::in | std::ios::out);
        r.check(io.is_open(), "fstream opens in|out");
        io.seekp(0);
        io << "ALPHA";
        io.seekg(0);
        std::string word;
        io >> word;
        r.check(word == "ALPHA", "fstream writes then reads back");
    }
    {  // a stream through its base, deleted through it (virtual destructor, D0)
        std::istream* in = new std::ifstream(path.c_str());
        std::string word;
        *in >> word;
        r.check(word == "ALPHA" && dynamic_cast<std::ifstream*>(in) != nullptr, "ifstream through istream*");
        delete in;
        r.check(true, "delete through istream*");
    }
    {  // a missing file, then exceptions
        std::ifstream missing((dir + "no-such-file.txt").c_str());
        r.check(!missing.is_open() && missing.fail(), "missing file: not open, fail");
        bool thrown = false;
        try {
            std::ifstream f;
            f.exceptions(std::ios::failbit);
            f.open((dir + "no-such-file.txt").c_str());
        } catch (const std::ios_base::failure&) {
            thrown = true;
        }
        r.check(thrown, "ios_base::failure thrown and caught");
    }
    {  // string streams
        std::ostringstream os;
        os << 42 << ' ' << 3.5 << ' ' << "x";
        r.check(os.str() == "42 3.5 x", "ostringstream formats");
        std::istringstream is("7 8.25 word");
        int i = 0;
        double d = 0;
        std::string w;
        is >> i >> d >> w;
        r.check(i == 7 && d == 8.25 && w == "word", "istringstream parses");
        std::stringstream ss;
        ss << "a,b,c";
        std::string part;
        int parts = 0;
        while (std::getline(ss, part, ',')) {
            ++parts;
        }
        r.check(parts == 3, "stringstream getline with a delimiter");
    }
    // C <ctype.h>: glibc's macros read __ctype_b_loc / __ctype_toupper_loc tables.
    r.check(isalpha('a') && !isalpha('1') && isdigit('7') && isspace(' ') && !isspace('x') && isxdigit('F') &&
                ispunct('!') && isupper('Q') && islower('q') && isalnum('z') && !isalnum('-') && isprint(' ') &&
                !isgraph(' ') && iscntrl('\n') && isblank('\t'),
            "C ctype classes (glibc tables)");
    r.check(toupper('q') == 'Q' && tolower('Q') == 'q' && toupper('1') == '1', "C toupper/tolower");
    // C++ <locale>: std::isalpha(c, loc) reads ctype<char>'s mask table inline.
    const std::locale loc;
    r.check(std::isalpha('a', loc) && !std::isalpha('1', loc) && std::isdigit('7', loc) &&
                std::isspace(' ', loc) && std::isupper('Q', loc) && !std::isupper('q', loc),
            "C++ ctype<char>::is through the locale (inline mask table)");
    r.check(std::toupper('q', loc) == 'Q', "C++ ctype<char>::toupper (out of line)");
    // std::regex: its scanner and traits read the mask table inline (Randomizer uses regex).
    r.check(std::regex_match("abc123", std::regex("[a-z]+[0-9]+")), "regex with ranges");
    r.check(std::regex_match("abc123", std::regex("\\w+\\d+")), "regex with \\w and \\d classes");
    r.check(std::regex_match("ff0A", std::regex("[[:xdigit:]]+")) && !std::regex_match("fg", std::regex("[[:xdigit:]]+")),
            "regex with [[:xdigit:]]");
    run_sol_checks(dir, r);
    std::remove(path.c_str());
}

}  // namespace lab
