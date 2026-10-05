// H38: NativeTextEditSession and the length rules behind the console's keyboard, with a fake backend.
#include "dusk/ui/platform_text_input.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dusk::ui;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

struct Fake final : PlatformTextInput {
    TextInputResult next;
    int shows = 0;
    TextInputRequest last;
    bool is_native() const override { return true; }
    TextInputResult show(const TextInputRequest& r) override {
        ++shows;
        last = r;
        return next;
    }
};

struct Harness {
    NativeTextEditSession session;
    Fake backend;
    std::vector<std::string> applied;
    int focusRestores = 0;
    NativeEditOutcome pump(const TextInputRequest& req) {
        return session.pump(
            backend, req, [this](std::string v) { applied.push_back(std::move(v)); },
            [this] { ++focusRestores; });
    }
};

}  // namespace

int main() {
    {  // accept: applied once, focus restored, keyboard opened once
        Harness h;
        h.backend.next = {TextInputStatus::Accepted, "Link"};
        TextInputRequest req;
        req.initialText = "Old";
        CHECK(h.session.request());
        CHECK(h.pump(req) == NativeEditOutcome::Applied);
        CHECK(h.applied.size() == 1 && h.applied[0] == "Link");
        CHECK(h.backend.shows == 1 && h.backend.last.initialText == "Old");
        CHECK(h.focusRestores == 1);
        CHECK(!h.session.busy());
        CHECK(h.pump(req) == NativeEditOutcome::NotRun);  // nothing pending: no second keyboard
        CHECK(h.backend.shows == 1);
    }
    {  // cancel and error keep the previous value, still restore focus
        for (const auto status : {TextInputStatus::Cancelled, TextInputStatus::Error}) {
            Harness h;
            h.backend.next = {status, "ignored"};
            h.session.request();
            const auto outcome = h.pump({});
            CHECK(outcome == (status == TextInputStatus::Error ? NativeEditOutcome::Failed
                                                               : NativeEditOutcome::Cancelled));
            CHECK(h.applied.empty());
            CHECK(h.focusRestores == 1);
            CHECK(!h.session.busy());
        }
    }
    {  // repeated activation while pending, or per-frame pumping, opens the keyboard once
        Harness h;
        h.backend.next = {TextInputStatus::Accepted, "x"};
        CHECK(h.session.request());
        CHECK(!h.session.request());
        CHECK(!h.session.request());
        for (int frame = 0; frame < 10; ++frame) {
            h.pump({});
        }
        CHECK(h.backend.shows == 1 && h.applied.size() == 1);
        CHECK(h.session.request());  // a later activation is a new edit
        h.pump({});
        CHECK(h.backend.shows == 2 && h.applied.size() == 2);
    }
    {  // length: characters when maxLength is set, 256 UTF-8 bytes otherwise, never mid-character
        CHECK(limit_text_input("abcdef", 3) == "abc");
        CHECK(limit_text_input("abc", 10) == "abc");
        CHECK(limit_text_input("\xC3\xA1\xC3\xA9\xC3\xAD", 2) == "\xC3\xA1\xC3\xA9");
        CHECK(limit_text_input(std::string(300, 'a'), -1).size() == 256);
        CHECK(limit_text_input(std::string(300, 'a'), 0).size() == 256);
        std::string wide;
        for (int i = 0; i < 200; ++i) {
            wide += "\xC3\xA1";  // 2 bytes each: 400 bytes
        }
        const std::string cut = limit_text_input(wide, -1);
        CHECK(cut.size() == 256);  // 128 characters, an even number of bytes
        Harness h;
        h.backend.next = {TextInputStatus::Accepted, "abcdefgh"};
        TextInputRequest req;
        req.maxLength = 4;
        h.session.request();
        h.pump(req);
        CHECK(h.applied.size() == 1 && h.applied[0] == "abcd");
    }
    {  // numeric fields keep only number characters
        Harness h;
        h.backend.next = {TextInputStatus::Accepted, "-12a.5 "};
        TextInputRequest req;
        req.numeric = true;
        h.session.request();
        h.pump(req);
        CHECK(h.applied.size() == 1 && h.applied[0] == "-12.5");
    }
    {  // the default backend leaves the RmlUi editor alone; a fake can be swapped in and out
        CHECK(!platform_text_input().is_native());
        Fake fake;
        set_platform_text_input_for_testing(&fake);
        CHECK(platform_text_input().is_native());
        set_platform_text_input_for_testing(nullptr);
        CHECK(!platform_text_input().is_native());
    }
    if (failures == 0) {
        std::puts("text input: accept, cancel, error, length limit, single call, focus restore: ok");
    }
    return failures == 0 ? 0 : 1;
}
