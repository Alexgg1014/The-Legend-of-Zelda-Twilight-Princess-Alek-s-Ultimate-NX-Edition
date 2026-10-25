#pragma once

// aleks2 H38: a text field can be edited by the platform's own keyboard instead of an RmlUi <input>.
//
// PlatformTextInput is the seam. The default backend (platform_text_input.cpp) reports is_native() == false
// and BaseStringButton keeps its RmlUi editor, so desktop and Android behave as before. The Switch backend
// (platforms/switch/source/switch_text_input.cpp) opens the system software keyboard. Nothing here depends on
// RmlUi or libnx: host tests drive NativeTextEditSession with a fake backend.
//
// 1.0.2: a backend can run the keyboard without blocking the caller (begin/poll). The Switch one does: the game
// thread kept no frames running while swkbd was up, so a mod's network on that thread (Crests of Courage's UDP)
// timed out and the host dropped the player who was typing.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace dusk::ui {

// What a mod control with no (or a non-positive) maxLength is limited to on a native keyboard, in UTF-8 bytes.
constexpr int kNativeTextUnlimitedBytes = 256;

struct TextInputRequest {
    std::string initialText;
    std::string header;
    bool numeric = false;
    // Characters, as RmlUi's maxlength counts them; < 1 means "unlimited" and falls back to
    // kNativeTextUnlimitedBytes bytes.
    int maxLength = -1;
};

enum class TextInputStatus { Accepted, Cancelled, Error };
enum class TextInputStart { Started, Busy, Unsupported };

struct TextInputResult {
    TextInputStatus status = TextInputStatus::Cancelled;
    std::string text;
};

class PlatformTextInput {
public:
    virtual ~PlatformTextInput() = default;
    // True when show() is the way to edit text and the RmlUi editor must not be created.
    virtual bool is_native() const = 0;
    // Blocks until the keyboard closes. Never called when is_native() is false.
    virtual TextInputResult show(const TextInputRequest& request) = 0;
    // Opens the keyboard without blocking. Started: poll() gives the result once it closes. Busy: a keyboard of an
    // edit that went away (abandon()) is still open, so none is opened now. Unsupported: use show().
    virtual TextInputStart begin(const TextInputRequest&) { return TextInputStart::Unsupported; }
    // The result of the keyboard begin() opened, once it closed; nullopt while it is open.
    virtual std::optional<TextInputResult> poll() { return TextInputResult{TextInputStatus::Error, {}}; }
    // The edit begin() opened the keyboard for is gone: its result, when it comes, is dropped.
    virtual void abandon() {}
};

// The backend of this platform; replace it with a fake in tests (nullptr restores the default).
PlatformTextInput& platform_text_input();
void set_platform_text_input_for_testing(PlatformTextInput* backend);

// Truncates UTF-8 text without splitting a character: to maxLength code points when maxLength >= 1,
// otherwise to kNativeTextUnlimitedBytes bytes.
inline std::string limit_text_input(std::string text, int maxLength) {
    const auto is_continuation = [](unsigned char c) { return (c & 0xC0) == 0x80; };
    if (maxLength >= 1) {
        int chars = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (!is_continuation(static_cast<unsigned char>(text[i])) && ++chars > maxLength) {
                text.resize(i);
                break;
            }
        }
        return text;
    }
    if (text.size() > static_cast<std::size_t>(kNativeTextUnlimitedBytes)) {
        std::size_t cut = kNativeTextUnlimitedBytes;
        while (cut > 0 && is_continuation(static_cast<unsigned char>(text[cut]))) {
            --cut;
        }
        text.resize(cut);
    }
    return text;
}

// A numeric field keeps only what a number input accepts.
inline std::string sanitize_numeric_input(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
            out.push_back(c);
        }
    }
    return out;
}

enum class NativeEditOutcome { NotRun, Running, Applied, Cancelled, Failed };

// One native edit of one field: request() marks it wanted (ignored while one is pending or running), and
// pump() -- called from the owner's per-frame update -- opens the keyboard exactly once for it, applies an
// accepted value once, keeps the old value on cancel or error, and restores the UI focus afterwards. With a
// backend that can begin() the keyboard, pump() returns Running while it is open and finishes the edit on the
// first frame after it closed; the field stays busy until then.
class NativeTextEditSession {
public:
    NativeTextEditSession() = default;
    NativeTextEditSession(const NativeTextEditSession&) = delete;
    NativeTextEditSession& operator=(const NativeTextEditSession&) = delete;
    ~NativeTextEditSession() {
        if (mOpenOn != nullptr) {
            mOpenOn->abandon();
        }
    }

    bool request() {
        if (mPending || mRunning) {
            return false;
        }
        mPending = true;
        return true;
    }
    bool busy() const { return mPending || mRunning; }

    NativeEditOutcome pump(PlatformTextInput& backend, const TextInputRequest& request,
        const std::function<void(std::string)>& apply, const std::function<void()>& restoreFocus) {
        if (mOpenOn != nullptr) {
            std::optional<TextInputResult> result = mOpenOn->poll();
            if (!result) {
                return NativeEditOutcome::Running;
            }
            mOpenOn = nullptr;
            // The request the keyboard was opened with: the field's own value may have moved since.
            return finish(*result, mOpenRequest, apply, restoreFocus);
        }
        if (!mPending) {
            return NativeEditOutcome::NotRun;
        }
        mPending = false;
        mRunning = true;
        switch (backend.begin(request)) {
        case TextInputStart::Started:
            mOpenOn = &backend;
            mOpenRequest = request;
            return NativeEditOutcome::Running;
        case TextInputStart::Busy:
            return finish({TextInputStatus::Cancelled, {}}, request, apply, restoreFocus);
        case TextInputStart::Unsupported:
            break;
        }
        return finish(backend.show(request), request, apply, restoreFocus);
    }

private:
    NativeEditOutcome finish(const TextInputResult& result, const TextInputRequest& request,
        const std::function<void(std::string)>& apply, const std::function<void()>& restoreFocus) {
        NativeEditOutcome outcome = NativeEditOutcome::Cancelled;
        if (result.status == TextInputStatus::Accepted) {
            std::string text = request.numeric ? sanitize_numeric_input(result.text) : result.text;
            if (apply) {
                apply(limit_text_input(std::move(text), request.maxLength));
            }
            outcome = NativeEditOutcome::Applied;
        } else if (result.status == TextInputStatus::Error) {
            outcome = NativeEditOutcome::Failed;
        }
        mRunning = false;
        if (restoreFocus) {
            restoreFocus();
        }
        return outcome;
    }

    bool mPending = false;
    bool mRunning = false;
    PlatformTextInput* mOpenOn = nullptr;  // the backend whose keyboard is open for this edit
    TextInputRequest mOpenRequest;
};

}  // namespace dusk::ui
