#include "platform_text_input.hpp"

#ifdef __SWITCH__
#include "switch/text_input.hpp"
#endif

namespace dusk::ui {

namespace {

// Platforms with a real text field of their own (desktop, Android, iOS) keep the RmlUi editor.
class RmlUiTextInput final : public PlatformTextInput {
public:
    bool is_native() const override { return false; }
    TextInputResult show(const TextInputRequest&) override { return {TextInputStatus::Error, {}}; }
};

PlatformTextInput* g_override = nullptr;

}  // namespace

PlatformTextInput& platform_text_input() {
    if (g_override != nullptr) {
        return *g_override;
    }
#ifdef __SWITCH__
    return dusk::sw::text_input();
#else
    static RmlUiTextInput instance;
    return instance;
#endif
}

void set_platform_text_input_for_testing(PlatformTextInput* backend) {
    g_override = backend;
}

}  // namespace dusk::ui
