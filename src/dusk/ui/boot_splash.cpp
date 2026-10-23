#include "boot_splash.hpp"

#include "document.hpp"
#include "format.hpp"
#include "ui.hpp"

#include "dusk/imgui/ImGuiConsole.hpp"
#include "dusk/logging.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <memory>

namespace dusk::ui::boot_splash {
namespace {

const Rml::String kSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <style>
        body {
            width: 100%;
            height: 100%;
            background-color: #0b0d12;
            color: #e8e1cf;
            font-family: var(--font-family-body);
            text-align: center;
        }
        #splash-box {
            position: absolute;
            left: 0;
            right: 0;
            top: 30%;
        }
        #splash-title {
            font-family: var(--font-family-display);
            font-size: 52dp;
            color: #e3c77a;
            display: block;
        }
        #splash-author {
            font-size: 22dp;
            margin-top: 10dp;
            display: block;
        }
        #splash-label {
            font-size: 18dp;
            margin-top: 48dp;
            display: block;
            color: #c9c2b2;
        }
        #splash-track {
            display: block;
            width: 40%;
            height: 4dp;
            margin: 12dp auto 0 auto;
            background-color: #2a2d36;
        }
        #splash-bar {
            display: block;
            height: 4dp;
            width: 0%;
            background-color: #e3c77a;
        }
        #splash-foot {
            position: absolute;
            left: 0;
            right: 0;
            bottom: 28dp;
            font-size: 15dp;
            color: #8f897b;
        }
        #splash-free {
            display: block;
            margin-bottom: 6dp;
            color: #c9a85a;
        }
    </style>
</head>
<body>
    <div id="splash-box">
        <span id="splash-title">Alek's Ultimate NX Edition</span>
        <span id="splash-author">by Alexgg1014</span>
        <span id="splash-label">Starting</span>
        <div id="splash-track"><div id="splash-bar"></div></div>
    </div>
    <div id="splash-foot">
        <span id="splash-free">Free &#8212; if you paid for this, you got scammed</span>
        <span>Based on Dusklight by Twilit Realm &#183; Powered by Aurora</span>
    </div>
</body>
</rml>
)RML";

class BootSplash final : public Document {
public:
    BootSplash() : Document(kSource, true, DocumentScope::None) {
        mLabel = mDocument != nullptr ? mDocument->GetElementById("splash-label") : nullptr;
        mBar = mDocument != nullptr ? mDocument->GetElementById("splash-bar") : nullptr;
    }
    bool obscures_game() const override { return true; }
    void set(const char* label, int done, int total) {
        if (mLabel != nullptr && label != nullptr) {
            mLabel->SetInnerRML(escape(label));
        }
        if (mBar != nullptr && total > 0) {
            const int pct = std::clamp(done * 100 / total, 0, 100);
            mBar->SetProperty("width", fmt::format("{}%", pct));
        }
    }

private:
    Rml::Element* mLabel = nullptr;
    Rml::Element* mBar = nullptr;
};

BootSplash* s_splash = nullptr;
std::chrono::steady_clock::time_point s_lastFrame{};

// One frame, as launchUILoop presents them; events are drained and left alone (no input during the boot).
void present() noexcept {
    const AuroraEvent* event = aurora_update();
    while (event != nullptr && event->type != AURORA_NONE) {
        event++;
    }
    if (!aurora_begin_frame()) {
        return;  // skipped: the next call tries again
    }
    ui::update();
    g_imguiConsole.PreDraw();
    g_imguiConsole.PostDraw();
    aurora_end_frame();
}

}  // namespace

void show() noexcept {
#ifdef __SWITCH__
    if (s_splash != nullptr) {
        return;
    }
    auto doc = std::make_unique<BootSplash>();
    s_splash = doc.get();
    push_document(std::move(doc), true, true);
    present();
    s_lastFrame = std::chrono::steady_clock::now();
#endif
}

void progress(const char* label, int done, int total) noexcept {
#ifdef __SWITCH__
    if (s_splash == nullptr) {
        return;
    }
    s_splash->set(label, done, total);
    const auto now = std::chrono::steady_clock::now();
    if (now - s_lastFrame < std::chrono::milliseconds{30}) {
        return;
    }
    s_lastFrame = now;
    present();
#else
    (void)label;
    (void)done;
    (void)total;
#endif
}

void hide() noexcept {
#ifdef __SWITCH__
    if (s_splash == nullptr) {
        return;
    }
    s_splash->set("Starting the game", 1, 1);
    present();
    s_splash->hide(true);
    s_splash = nullptr;
#endif
}

}  // namespace dusk::ui::boot_splash
