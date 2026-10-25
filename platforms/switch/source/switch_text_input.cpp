// The system software keyboard for text fields (aleks2 H38).
//
// A mod's string control used to open an RmlUi <input>, which needs a hardware keyboard or touch on the
// console. Here the field opens libnx's swkbd applet instead: one swkbdShow per activation, QWERTY for text and
// the number pad for numeric fields. The game's text layer (dusk::ui::NativeTextEditSession) decides when to call
// it, applies an accepted value once and keeps the old one on cancel or error.
//
// 1.0.2: swkbdShow runs on a thread of its own (begin/poll), so the game thread keeps running its frames while the
// keyboard is up. It blocked them before, and with them the network a mod runs from the game thread: Crests of
// Courage's UDP to the host went quiet and the host dropped the player typing a chat line. libnx's wait for the
// applet (appletHolderJoin) is a plain event wait, and the game, not in focus behind a library applet, is not
// suspended (AppletFocusHandlingMode_SuspendHomeSleep), so the main loop goes on as usual. The inline keyboard
// (swkbdInline*) would do the same with a per-frame update, its own input routing and a layer drawn over the
// game; none of that could be tried on a console for this release, so it is not used.
//
// Length: swkbd counts characters, so the character limit handed to it is the control's maxLength (at most
// kSwkbdMaxChars); a control without one gets kNativeTextUnlimitedBytes characters, and the result is cut to
// that many UTF-8 bytes afterwards by limit_text_input.
#include "switch/text_input.hpp"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <string>

namespace dusk::sw {

namespace {

constexpr int kSwkbdMaxChars = 500;  // libnx's swkbd input limit
// The keyboard thread: swkbd's config and reply buffers are on the heap, the applet IPC needs little stack.
// libnx adds TLS and reent on top (threadCreate with no stack of ours).
constexpr size_t kKeyboardStack = 128 * 1024;

class SwitchTextInput final : public dusk::ui::PlatformTextInput {
public:
    bool is_native() const override { return true; }

    dusk::ui::TextInputResult show(const dusk::ui::TextInputRequest& request) override { return run(request); }

    dusk::ui::TextInputStart begin(const dusk::ui::TextInputRequest& request) override {
        using dusk::ui::TextInputStart;
        if (mStarted) {
            if (!mDone.load(std::memory_order_acquire)) {
                return TextInputStart::Busy;  // an abandoned edit's keyboard is still up
            }
            reap();
        }
        mRequest = request;
        mResult = {};
        mAbandoned = false;
        mDone.store(false, std::memory_order_relaxed);
        if (R_FAILED(threadCreate(&mThread, entry, this, nullptr, kKeyboardStack, 0x2C, -2))) {
            return TextInputStart::Unsupported;  // the game thread opens it the old way
        }
        if (R_FAILED(threadStart(&mThread))) {
            threadClose(&mThread);
            return TextInputStart::Unsupported;
        }
        mStarted = true;
        return TextInputStart::Started;
    }

    std::optional<dusk::ui::TextInputResult> poll() override {
        if (!mStarted) {
            return dusk::ui::TextInputResult{dusk::ui::TextInputStatus::Error, {}};
        }
        if (!mDone.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        reap();
        if (mAbandoned) {
            return dusk::ui::TextInputResult{dusk::ui::TextInputStatus::Cancelled, {}};
        }
        return std::move(mResult);
    }

    void abandon() override { mAbandoned = true; }

private:
    static void entry(void* self) {
        auto* input = static_cast<SwitchTextInput*>(self);
        input->mResult = run(input->mRequest);
        input->mDone.store(true, std::memory_order_release);
    }

    // The thread has finished (mDone): wait for its exit and free it.
    void reap() {
        threadWaitForExit(&mThread);
        threadClose(&mThread);
        mStarted = false;
    }

    static dusk::ui::TextInputResult run(const dusk::ui::TextInputRequest& request) {
        using dusk::ui::TextInputStatus;
        const int chars = request.maxLength >= 1
                              ? std::min(request.maxLength, kSwkbdMaxChars)
                              : dusk::ui::kNativeTextUnlimitedBytes;

        SwkbdConfig kbd;
        if (R_FAILED(swkbdCreate(&kbd, 0))) {
            return {TextInputStatus::Error, {}};
        }
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetType(&kbd, request.numeric ? SwkbdType_NumPad : SwkbdType_QWERTY);
        swkbdConfigSetStringLenMax(&kbd, static_cast<u32>(chars));
        if (!request.header.empty()) {
            swkbdConfigSetHeaderText(&kbd, request.header.c_str());
        }
        const std::string initial = dusk::ui::limit_text_input(request.initialText, request.maxLength);
        swkbdConfigSetInitialText(&kbd, initial.c_str());

        // 4 bytes per character covers any UTF-8 the applet returns, plus the terminator.
        std::string buffer(static_cast<size_t>(chars) * 4 + 1, '\0');
        const Result rc = swkbdShow(&kbd, buffer.data(), buffer.size());
        swkbdClose(&kbd);

        if (R_FAILED(rc)) {
            // Cancel and failure both leave the field as it was; the game does not tell them apart.
            return {TextInputStatus::Cancelled, {}};
        }
        buffer.resize(std::char_traits<char>::length(buffer.c_str()));
        return {TextInputStatus::Accepted, std::move(buffer)};
    }

    // Written by the game thread before the keyboard thread starts and read by it after mDone, or the reverse.
    Thread mThread{};
    bool mStarted = false;
    bool mAbandoned = false;
    std::atomic<bool> mDone{false};
    dusk::ui::TextInputRequest mRequest;
    dusk::ui::TextInputResult mResult;
};

}  // namespace

dusk::ui::PlatformTextInput& text_input() {
    static SwitchTextInput instance;
    return instance;
}

}  // namespace dusk::sw
