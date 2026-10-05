#pragma once

#include "dusk/mods/catalog.hpp"
#include "remote_texture_provider.hpp"
#include "window.hpp"

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <optional>
#include <string>

namespace dusk::ui {

class ModBrowser final : public Window {
public:
    ModBrowser();
    ~ModBrowser() override;

    void update() override;

private:
    enum class State {
        Loading,
        Ready,
        Error,
        Unavailable,
    };
    enum class FocusTarget {
        Default,
        Search,
        Category,
        Sort,
        Device,
        Results,
        Retry,
    };

    void build_content(Rml::Element* content);
    void begin_fetch(FocusTarget focusTarget);
    void finish_fetch(mods::catalog::FetchResult result);
    void update_image_hints(bool force);

    mods::catalog::Query mQuery;
    std::optional<mods::catalog::Page> mPage;
    borealis::Task<mods::catalog::FetchResult> mFetch;
    std::string mError;
    State mState = State::Loading;
    FocusTarget mFocusTarget = FocusTarget::Default;
    uint64_t mLoaderGeneration = 0;
    bool mRebuildRequested = false;

    // H34: image scheduling and telemetry (remote_texture_provider.hpp).
    uint64_t mImageOwner = 0;
    std::unordered_map<std::string, std::vector<std::string>> mCardImages;
    bool mHintsCovered = false;
    float mHintsScrollTop = -1.0f;
    float mHintsClientHeight = -1.0f;
    uint64_t mHintUpdates = 0;
    RemoteImageTelemetry mTelemetryStart;
    std::chrono::steady_clock::time_point mFetchStart;
    double mFetchMsLast = 0;
    double mFetchMsMax = 0;
    uint64_t mFetchCount = 0;
    double mRebuildMsMax = 0;
    double mRebuildMsTotal = 0;
    uint64_t mRebuildCount = 0;
};

}  // namespace dusk::ui
