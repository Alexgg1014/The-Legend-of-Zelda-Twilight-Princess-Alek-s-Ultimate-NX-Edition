#include "remote_texture_provider.hpp"

#include <fmt/format.h>

namespace dusk::ui {

std::string remote_image_source(std::string_view url, uint32_t width, uint32_t height) {
    return remote_image::make_source(url, width, height, 0);
}

std::string remote_image_source(
    std::string_view url, uint32_t width, uint32_t height, uint32_t maxDimension) {
    return remote_image::make_source(url, width, height, maxDimension);
}

std::string remote_image_telemetry_summary(
    const RemoteImageTelemetry& since, const RemoteImageTelemetry& now) {
    const auto& a = since.cache;
    const auto& b = now.cache;
    const auto decoded = b.decoded - a.decoded;
    const auto downloaded = b.downloaded - a.downloaded;
    const double networkAvg =
        downloaded + (b.networkFailed - a.networkFailed) == 0 ?
            0.0 :
            (b.networkMsTotal - a.networkMsTotal) /
                static_cast<double>(downloaded + (b.networkFailed - a.networkFailed));
    const double decodeAvg =
        decoded == 0 ? 0.0 : (b.decodeMsTotal - a.decodeMsTotal) / static_cast<double>(decoded);
    const auto lookups = now.lookups - since.lookups;
    return fmt::format(
        "images: requested={} started={} downloaded={} ({} KiB) decoded={} downscaled={} "
        "reused={} published={} canceled={} stale={} failed_net={} failed_decode={} "
        "rejected={} evicted={} dropped={} | pending_max={} queued_max={} publish_queue_max={} | "
        "memory={}/{} KiB peak={} KiB entries={} | network_ms avg={:.1f} max={:.1f} "
        "decode_ms(worker) avg={:.1f} max={:.1f} | main: lookup_ms avg={:.3f} max={:.2f} "
        "update_ms_max={:.2f} frame_ms_max={:.1f} frame_after_publish_ms_max={:.1f} "
        "publish_frames={} frames={}",
        b.created - a.created, b.started - a.started, downloaded,
        (b.downloadBytes - a.downloadBytes) / 1024, decoded, b.downscaled - a.downscaled,
        b.hits - a.hits, b.published - a.published, b.canceled - a.canceled, b.stale - a.stale,
        b.networkFailed - a.networkFailed, b.decodeFailed - a.decodeFailed,
        b.rejected - a.rejected, b.evicted - a.evicted, b.dropped - a.dropped, b.maxPending,
        b.maxQueued, b.maxPublishQueue, now.readyBytes / 1024, now.readyBytesLimit / 1024,
        b.peakBytes / 1024, now.readyEntries, networkAvg, b.networkMsMax, decodeAvg,
        b.decodeMsMax,
        lookups == 0 ? 0.0 : (now.lookupMsTotal - since.lookupMsTotal) / static_cast<double>(lookups),
        now.lookupMsMax, now.updateMsMax, now.frameMsMax, now.frameAfterPublishMsMax,
        now.publishFrames - since.publishFrames, now.frames - since.frames);
}

}  // namespace dusk::ui

#ifdef AURORA_ENABLE_RMLUI

#include "dusk/app_info.hpp"
#include "runtime_image.hpp"

#include <RmlUi/Core.h>
#include <aurora/rmlui.hpp>
#include <borealis/http.hpp>
#include <borealis/log.hpp>
#include <borealis/task.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dusk::ui {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace ri = remote_image;

constexpr borealis::Log Log{"dusk::ui"};
constexpr std::string_view kScheme = "https";
constexpr std::string_view kAllowedPrefix = "https://twilitrealm.dev/images/v1/";
constexpr size_t kMaxImageFileSize = 16 * 1024 * 1024;
// RmlUi caches the first texture dimensions in image decorators, so the async
// placeholder must preserve the final image's aspect ratio.
constexpr uint32_t kPlaceholderMaxDimension = 256;
constexpr std::array<std::byte, kPlaceholderMaxDimension * kPlaceholderMaxDimension * 4>
    kTransparentPixels{};

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The request in flight for a source. The Task's destructor asks the worker to stop;
// the worker owns everything it touches, so dropping one is always safe.
struct InFlight {
    uint64_t ticket = 0;
    borealis::Task<ri::FetchOutcome> task;
};

struct ProviderState {
    ri::Cache cache;
    std::unordered_map<std::string, InFlight> inFlight;
    RemoteImageTelemetry telemetry;
    std::optional<Clock::time_point> lastUpdate;
    bool publishedLastFrame = false;
    uint64_t nextOwner = 1;
};

ProviderState& provider_state() {
    static ProviderState state;
    return state;
}

aurora::rmlui::RuntimeTexture transparent_texture(uint32_t width, uint32_t height) {
    width = std::clamp(width, 1u, kPlaceholderMaxDimension);
    height = std::clamp(height, 1u, kPlaceholderMaxDimension);
    const auto size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    return {
        .width = width,
        .height = height,
        .rgba8 = std::span{kTransparentPixels}.first(size),
        .premultipliedAlpha = true,
    };
}

// Runs on the borealis worker that completed the HTTP request (Task::map). Everything
// here is local: the response body, the decoded buffer, the strings it was given.
ri::FetchOutcome finish_fetch(const std::string& source, uint32_t maxDimension,
    Clock::time_point requested, borealis::http::Result result) {
    ri::FetchOutcome outcome;
    outcome.networkMs = ms_since(requested);
    if (result.error == borealis::http::Error::Canceled) {
        outcome.kind = ri::FetchOutcome::Kind::Canceled;
        outcome.message = result.message;
        return outcome;
    }
    if (result.error != borealis::http::Error::None) {
        outcome.kind = ri::FetchOutcome::Kind::NetworkError;
        outcome.message = result.message;
        return outcome;
    }
    if (result.response.statusCode != 200) {
        outcome.kind = ri::FetchOutcome::Kind::HttpError;
        outcome.message = fmt::format("HTTP {}", result.response.statusCode);
        return outcome;
    }
    outcome.bodyBytes = result.response.body.size();
    const auto decodeStart = Clock::now();
    const auto* data = reinterpret_cast<const uint8_t*>(result.response.body.data());
    auto decoded = decode_png(std::span{data, result.response.body.size()}, source);
    if (!decoded) {
        outcome.kind = ri::FetchOutcome::Kind::DecodeError;
        outcome.decodeMs = ms_since(decodeStart);
        return outcome;
    }
    outcome.decodedWidth = decoded->width;
    outcome.decodedHeight = decoded->height;
    ri::Image image{
        .pixels = std::move(decoded->pixels),
        .width = decoded->width,
        .height = decoded->height,
    };
    outcome.image = ri::downscale_to_fit(std::move(image), maxDimension);
    outcome.decodeMs = ms_since(decodeStart);
    outcome.kind = ri::FetchOutcome::Kind::Ok;
    return outcome;
}

borealis::Task<ri::FetchOutcome> start_request(const ri::Start& start) {
    const auto requested = Clock::now();
    return borealis::http::start({
                                     .url = start.url,
                                     .headers =
                                         {
                                             {.name = "User-Agent",
                                                 .value = borealis::user_agent(dusk::AppInfo)},
                                             {.name = "Accept", .value = "image/png"},
                                         },
                                     .connectTimeout = 10s,
                                     .idleTimeout = 10s,
                                     .totalTimeout = 30s,
                                     .maxBodyBytes = kMaxImageFileSize,
                                 })
        .map([source = start.source, maxDimension = start.maxDimension, requested](
                 borealis::http::Result result) {
            return finish_fetch(source, maxDimension, requested, std::move(result));
        });
}

std::optional<aurora::rmlui::RuntimeTexture> remote_texture_provider(std::string_view source) {
    const auto start = Clock::now();
    const auto parsed = ri::parse_source(source, kPlaceholderMaxDimension);
    if (!parsed.requestUrl.starts_with(kAllowedPrefix)) {
        return std::nullopt;
    }
    auto& state = provider_state();
    const auto lookup =
        state.cache.lookup(source, parsed.placeholderWidth, parsed.placeholderHeight);
    std::optional<aurora::rmlui::RuntimeTexture> texture;
    if (lookup.image != nullptr) {
        // Aurora copies these pixels before this call returns (GenerateTexture).
        texture = aurora::rmlui::RuntimeTexture{
            .width = lookup.image->width,
            .height = lookup.image->height,
            .rgba8 = std::span{reinterpret_cast<const std::byte*>(lookup.image->pixels.data()),
                lookup.image->pixels.size()},
            .premultipliedAlpha = true,
        };
    } else {
        texture = transparent_texture(lookup.placeholderWidth, lookup.placeholderHeight);
    }
    const double elapsed = ms_since(start);
    ++state.telemetry.lookups;
    state.telemetry.lookupMsTotal += elapsed;
    state.telemetry.lookupMsMax = std::max(state.telemetry.lookupMsMax, elapsed);
    return texture;
}

void apply_cancels(ProviderState& state) {
    for (const auto& cancel : state.cache.take_cancels()) {
        if (const auto it = state.inFlight.find(cancel.source);
            it != state.inFlight.end() && it->second.ticket == cancel.ticket)
        {
            it->second.task.cancel();
            state.inFlight.erase(it);
        }
    }
}

void release_sources(ProviderState& state) {
    for (const auto& source : state.cache.take_releases()) {
        Rml::ReleaseTexture(source);
    }
}

}  // namespace

void register_remote_texture_provider() noexcept {
    aurora::rmlui::register_texture_provider(std::string{kScheme}, remote_texture_provider);
}

void unregister_remote_texture_provider() noexcept {
    aurora::rmlui::unregister_texture_provider(kScheme);
    auto& state = provider_state();
    for (auto& [source, request] : state.inFlight) {
        request.task.cancel();
    }
    state.inFlight.clear();
    for (const auto& source : state.cache.clear()) {
        Rml::ReleaseTexture(source);
    }
    state.lastUpdate.reset();
    state.publishedLastFrame = false;
}

void update_remote_texture_provider() noexcept {
    auto& state = provider_state();
    const auto start = Clock::now();
    auto& telemetry = state.telemetry;
    if (state.lastUpdate) {
        const double frameMs =
            std::chrono::duration<double, std::milli>(start - *state.lastUpdate).count();
        telemetry.frameMsMax = std::max(telemetry.frameMsMax, frameMs);
        // RmlUi recreates a released texture the next time it renders it, which is
        // between the update that published it and this one.
        if (state.publishedLastFrame) {
            telemetry.frameAfterPublishMsMax = std::max(telemetry.frameAfterPublishMsMax, frameMs);
        }
    }
    state.lastUpdate = start;
    ++telemetry.frames;

    // 1. Results the workers finished. Only a handful of pointer moves here: decode
    //    and downscale already happened on the worker.
    for (auto it = state.inFlight.begin(); it != state.inFlight.end();) {
        if (!it->second.task.ready()) {
            ++it;
            continue;
        }
        ri::FetchOutcome outcome;
        try {
            if (auto result = it->second.task.try_take()) {
                outcome = std::move(*result);
            } else {
                outcome.kind = ri::FetchOutcome::Kind::NetworkError;
                outcome.message = "no result";
            }
        } catch (const std::exception& exception) {
            outcome.kind = ri::FetchOutcome::Kind::NetworkError;
            outcome.message = exception.what();
        } catch (...) {
            outcome.kind = ri::FetchOutcome::Kind::NetworkError;
            outcome.message = "unknown error";
        }
        const std::string source = it->first;
        const uint64_t ticket = it->second.ticket;
        it = state.inFlight.erase(it);
        if (outcome.kind == ri::FetchOutcome::Kind::NetworkError ||
            outcome.kind == ri::FetchOutcome::Kind::HttpError)
        {
            Log.warn("Failed to fetch image '{}': {}", source, outcome.message);
        }
        if (state.cache.complete(source, ticket, std::move(outcome)) ==
            ri::Cache::Completion::Rejected)
        {
            Log.warn("Image '{}' exceeds the decoded image cache budget", source);
        }
    }

    // 2. Requests nobody wants any more (a page that was left, a closed browser).
    apply_cancels(state);

    // 3. New requests, visible first, within the concurrency limit.
    for (const auto& request : state.cache.take_starts()) {
        try {
            state.inFlight[request.source] = InFlight{
                .ticket = request.ticket,
                .task = start_request(request),
            };
        } catch (const std::exception& exception) {
            Log.warn("Failed to request image '{}': {}", request.source, exception.what());
            ri::FetchOutcome failed;
            failed.kind = ri::FetchOutcome::Kind::NetworkError;
            state.cache.complete(request.source, request.ticket, std::move(failed));
        } catch (...) {
            Log.warn("Failed to request image '{}'", request.source);
            ri::FetchOutcome failed;
            failed.kind = ri::FetchOutcome::Kind::NetworkError;
            state.cache.complete(request.source, request.ticket, std::move(failed));
        }
    }

    // 4. Hand finished images to RmlUi under the per-frame budget. RmlUi cached the
    //    transparent placeholder; releasing it makes RmlUi ask the provider again,
    //    which now answers from memory, and create the real texture on its own thread.
    const auto publishes = state.cache.take_publishes();
    for (const auto& source : publishes) {
        Rml::ReleaseTexture(source);
    }
    state.publishedLastFrame = !publishes.empty();
    if (state.publishedLastFrame) {
        ++telemetry.publishFrames;
    }

    // 5. Evicted images and forgotten placeholders.
    release_sources(state);

    telemetry.updateMsMax = std::max(telemetry.updateMsMax, ms_since(start));
}

uint64_t new_remote_image_owner() noexcept {
    return provider_state().nextOwner++;
}

void set_remote_image_hints(
    uint64_t owner, const std::vector<std::pair<std::string, ri::Priority>>& hints) noexcept {
    auto& state = provider_state();
    try {
        state.cache.set_hints(owner, hints);
    } catch (...) {
        // Only allocation can fail here; scheduling then simply stays as it was.
    }
    apply_cancels(state);
    release_sources(state);
}

void clear_remote_image_hints(uint64_t owner) noexcept {
    set_remote_image_hints(owner, {});
}

RemoteImageTelemetry remote_image_telemetry() noexcept {
    const auto& state = provider_state();
    RemoteImageTelemetry snapshot = state.telemetry;
    snapshot.cache = state.cache.stats();
    snapshot.readyBytes = state.cache.ready_bytes();
    snapshot.readyBytesLimit = state.cache.limits().maxReadyBytes;
    snapshot.readyEntries = state.cache.ready_entries();
    return snapshot;
}

RemoteImageTelemetry begin_remote_image_telemetry() noexcept {
    auto& state = provider_state();
    state.cache.reset_peaks();
    state.telemetry.lookupMsMax = 0;
    state.telemetry.updateMsMax = 0;
    state.telemetry.frameMsMax = 0;
    state.telemetry.frameAfterPublishMsMax = 0;
    return remote_image_telemetry();
}

}  // namespace dusk::ui

#else

namespace dusk::ui {

void register_remote_texture_provider() noexcept {}
void unregister_remote_texture_provider() noexcept {}
void update_remote_texture_provider() noexcept {}
uint64_t new_remote_image_owner() noexcept { return 0; }
void set_remote_image_hints(
    uint64_t, const std::vector<std::pair<std::string, remote_image::Priority>>&) noexcept {}
void clear_remote_image_hints(uint64_t) noexcept {}
RemoteImageTelemetry remote_image_telemetry() noexcept { return {}; }
RemoteImageTelemetry begin_remote_image_telemetry() noexcept { return {}; }

}  // namespace dusk::ui

#endif
