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

// 1.0.2: a keyboard that stays open over several frames (the Switch one runs on its own thread).
struct AsyncFake final : PlatformTextInput {
    std::optional<TextInputResult> next;  // nullopt: still open
    TextInputStart start = TextInputStart::Started;
    int begins = 0, shows = 0, abandons = 0;
    TextInputRequest last;
    bool is_native() const override { return true; }
    TextInputResult show(const TextInputRequest&) override {
        ++shows;
        return {TextInputStatus::Error, {}};
    }
    TextInputStart begin(const TextInputRequest& r) override {
        ++begins;
        last = r;
        return start;
    }
    std::optional<TextInputResult> poll() override { return next; }
    void abandon() override { ++abandons; }
};

template <class Backend>
struct BasicHarness {
    NativeTextEditSession session;
    Backend backend;
    std::vector<std::string> applied;
    int focusRestores = 0;
    NativeEditOutcome pump(const TextInputRequest& req) {
        return session.pump(
            backend, req, [this](std::string v) { applied.push_back(std::move(v)); },
            [this] { ++focusRestores; });
    }
};
using Harness = BasicHarness<Fake>;
using AsyncHarness = BasicHarness<AsyncFake>;

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
    {  // async: Running while open (the game keeps its frames), applied once on the first frame after it closed
        AsyncHarness h;
        TextInputRequest req;
        req.initialText = "Old";
        req.maxLength = 4;
        CHECK(h.session.request());
        CHECK(h.pump(req) == NativeEditOutcome::Running);
        CHECK(h.backend.begins == 1 && h.backend.shows == 0 && h.backend.last.initialText == "Old");
        TextInputRequest later;  // the field moved on meanwhile: the edit still uses the request it opened with
        later.maxLength = 100;
        for (int frame = 0; frame < 5; ++frame) {
            CHECK(h.pump(later) == NativeEditOutcome::Running);
            CHECK(h.session.busy() && !h.session.request());
        }
        CHECK(h.applied.empty() && h.focusRestores == 0);
        h.backend.next = TextInputResult{TextInputStatus::Accepted, "Midna!"};
        CHECK(h.pump(later) == NativeEditOutcome::Applied);
        CHECK(h.applied.size() == 1 && h.applied[0] == "Midn");
        CHECK(h.focusRestores == 1 && !h.session.busy());
        CHECK(h.pump(later) == NativeEditOutcome::NotRun && h.backend.begins == 1);
    }
    {  // async cancel and error leave the field as it was
        for (const auto status : {TextInputStatus::Cancelled, TextInputStatus::Error}) {
            AsyncHarness h;
            h.session.request();
            CHECK(h.pump({}) == NativeEditOutcome::Running);
            h.backend.next = TextInputResult{status, "ignored"};
            h.pump({});
            CHECK(h.applied.empty() && h.focusRestores == 1 && !h.session.busy());
        }
    }
    {  // a keyboard still open for an edit that went away: no second one, the new edit is a cancel
        AsyncHarness h;
        h.backend.start = TextInputStart::Busy;
        h.session.request();
        CHECK(h.pump({}) == NativeEditOutcome::Cancelled);
        CHECK(h.backend.shows == 0 && h.applied.empty() && h.focusRestores == 1 && !h.session.busy());
    }
    {  // a backend that cannot start one asynchronously falls back to show()
        AsyncHarness h;
        h.backend.start = TextInputStart::Unsupported;
        h.session.request();
        CHECK(h.pump({}) == NativeEditOutcome::Failed);
        CHECK(h.backend.shows == 1 && h.focusRestores == 1 && !h.session.busy());
    }
    {  // the field goes away with the keyboard open: the backend drops that result
        AsyncFake backend;
        {
            NativeTextEditSession session;
            session.request();
            session.pump(backend, {}, nullptr, nullptr);
        }
        CHECK(backend.abandons == 1);
        AsyncFake idle;
        { NativeTextEditSession session; }
        CHECK(idle.abandons == 0);
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
        std::puts("text input: accept, cancel, error, length limit, single call, focus restore, async: ok");
    }
    return failures == 0 ? 0 : 1;
}
