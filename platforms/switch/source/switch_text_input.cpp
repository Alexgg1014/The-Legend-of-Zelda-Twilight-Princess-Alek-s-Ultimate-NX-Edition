// The system software keyboard for text fields (aleks2 H38).
//
// A mod's string control used to open an RmlUi <input>, which needs a hardware keyboard or touch on the
// console. Here the field opens libnx's swkbd applet instead: one blocking swkbdShow per activation, QWERTY for
// text and the number pad for numeric fields. The game's text layer (dusk::ui::NativeTextEditSession) decides
// when to call it, applies an accepted value once and keeps the old one on cancel or error.
//
// Length: swkbd counts characters, so the character limit handed to it is the control's maxLength (at most
// kSwkbdMaxChars); a control without one gets kNativeTextUnlimitedBytes characters, and the result is cut to
// that many UTF-8 bytes afterwards by limit_text_input.
#include "switch/text_input.hpp"

#include <switch.h>

#include <algorithm>
#include <string>

namespace dusk::sw {

namespace {

constexpr int kSwkbdMaxChars = 500;  // libnx's swkbd input limit

class SwitchTextInput final : public dusk::ui::PlatformTextInput {
public:
    bool is_native() const override { return true; }

    dusk::ui::TextInputResult show(const dusk::ui::TextInputRequest& request) override {
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
};

}  // namespace

dusk::ui::PlatformTextInput& text_input() {
    static SwitchTextInput instance;
    return instance;
}

}  // namespace dusk::sw
