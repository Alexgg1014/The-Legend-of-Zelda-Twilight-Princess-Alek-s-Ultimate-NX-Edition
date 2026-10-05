#pragma once

#include "remote_image_cache.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dusk::ui {

[[nodiscard]] std::string remote_image_source(
    std::string_view url, uint32_t width, uint32_t height);
/// H34: as above, and the image is never decoded larger than `maxDimension` on its
/// longer edge (downscaled on the worker). 0 means no limit.
[[nodiscard]] std::string remote_image_source(
    std::string_view url, uint32_t width, uint32_t height, uint32_t maxDimension);

void register_remote_texture_provider() noexcept;
void unregister_remote_texture_provider() noexcept;
void update_remote_texture_provider() noexcept;

/// H34: scheduling hints from a view that knows what is on screen (the mod browser).
/// Each owner's call replaces its previous hints; an owner that goes away must call
/// clear_remote_image_hints so its unfinished requests are dropped. UI thread only.
[[nodiscard]] uint64_t new_remote_image_owner() noexcept;
void set_remote_image_hints(
    uint64_t owner, const std::vector<std::pair<std::string, remote_image::Priority>>& hints) noexcept;
void clear_remote_image_hints(uint64_t owner) noexcept;

/// H34: aggregated, low-cost telemetry. A snapshot is cheap to take; the summary is one
/// log-friendly line of what happened between two snapshots. UI thread only.
struct RemoteImageTelemetry {
    remote_image::Stats cache;
    std::uint64_t frames = 0;
    std::uint64_t lookups = 0;
    double lookupMsTotal = 0;
    double lookupMsMax = 0;
    double updateMsMax = 0;            ///< completions + starts + publishes, one UI update
    double frameMsMax = 0;             ///< UI update to UI update, any frame
    double frameAfterPublishMsMax = 0; ///< the frame in which RmlUi re-created published textures
    std::uint64_t publishFrames = 0;
    std::size_t readyBytes = 0;
    std::size_t readyBytesLimit = 0;
    std::size_t readyEntries = 0;
};
[[nodiscard]] RemoteImageTelemetry remote_image_telemetry() noexcept;
/// Starts a window: resets the peaks (the maxima) and returns the snapshot to diff
/// the counters against.
[[nodiscard]] RemoteImageTelemetry begin_remote_image_telemetry() noexcept;
[[nodiscard]] std::string remote_image_telemetry_summary(
    const RemoteImageTelemetry& since, const RemoteImageTelemetry& now);

}  // namespace dusk::ui
