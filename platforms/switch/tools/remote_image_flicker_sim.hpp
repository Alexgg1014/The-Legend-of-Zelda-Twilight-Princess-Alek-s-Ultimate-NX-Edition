#pragma once
// H34-c2: the simulation that reproduces the mod browser's thumbnail flicker.
//
// A fake RmlUi keeps a texture database keyed by the exact string the decorator was
// given (RmlUi does not unescape property values), asks the provider (the Cache) for a
// texture the first time a visible element needs one, and forgets a key when the
// provider releases it. The browser hints visible / ahead / deferred cards each frame.
// Requests finish a few frames after they start. The only thing that varies is which
// string the hints use and which string RmlUi hands the provider: identical keys are the
// fix, the H34-c1 "&max=" key escaped to "&amp;max=" is the bug.
//
// Header-only and RmlUi-free, used by remote_image_cache_test.cpp.
#include "dusk/ui/remote_image_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flicker_sim {

namespace ri = dusk::ui::remote_image;

struct Result {
    std::uint64_t requested = 0;   ///< distinct sources the cache saw
    std::uint64_t downloads = 0;   ///< requests started
    std::uint64_t evictions = 0;
    std::uint64_t published = 0;
    std::uint64_t holdDownloads = 0;       ///< requests started while the list stood still
    std::uint64_t visibleDisappeared = 0;  ///< an on-screen image went back to a placeholder
    std::uint64_t releasesUnknownToRml = 0;  ///< released keys RmlUi never loaded
    std::uint64_t maxEntriesPerThumbnail = 0;
    std::uint64_t thumbnails = 0;
};

// One card per mod, two columns, two images per card (banner art and icon).
inline Result run(const std::function<std::string(int)>& hintKey,
                  const std::function<std::string(int)>& rmlKey, int cards = 48, int visibleRows = 3,
                  int holdFrames = 600) {
    ri::Cache cache;  // the shipped limits: 64 ready, 256 tracked, 4 in flight
    const int rows = (cards + 1) / 2;
    const int images = cards * 2;
    std::unordered_map<std::string, bool> rml;  // key -> holds the real image (false: placeholder)
    std::vector<bool> shownReal(static_cast<std::size_t>(images), false);
    struct Pending {
        std::string source;
        std::uint64_t ticket;
        int doneAt;
    };
    std::vector<Pending> pending;
    Result result;

    // Scroll one row down every 10 frames to the bottom, back to the top, then hold.
    std::vector<int> scroll;
    for (int s = 0; s + visibleRows <= rows; ++s) {
        scroll.insert(scroll.end(), 10, s);
    }
    for (int s = rows - visibleRows; s >= 0; --s) {
        scroll.insert(scroll.end(), 10, s);
    }
    const std::size_t holdStart = scroll.size();
    scroll.insert(scroll.end(), static_cast<std::size_t>(holdFrames), 0);

    for (std::size_t frame = 0; frame < scroll.size(); ++frame) {
        const int top = scroll[frame];
        const bool holding = frame >= holdStart;

        // Provider update: finished requests, cancels, starts, publishes, releases.
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->doneAt > static_cast<int>(frame)) {
                ++it;
                continue;
            }
            const auto info = ri::parse_source(it->source, 0);
            const bool banner = info.placeholderWidth > info.placeholderHeight;
            ri::FetchOutcome outcome;
            outcome.kind = ri::FetchOutcome::Kind::Ok;
            outcome.image.width = banner ? 1280 : 256;
            outcome.image.height = banner ? 720 : 256;
            outcome.decodedWidth = outcome.image.width;
            outcome.decodedHeight = outcome.image.height;
            // The worker's downscale, as the provider does it.
            outcome.image.pixels.assign(static_cast<std::size_t>(outcome.image.width) * outcome.image.height * 4, 1);
            outcome.image = ri::downscale_to_fit(std::move(outcome.image), info.maxDimension);
            cache.complete(it->source, it->ticket, std::move(outcome));
            it = pending.erase(it);
        }
        for (const auto& cancel : cache.take_cancels()) {
            std::erase_if(pending, [&](const Pending& p) { return p.source == cancel.source && p.ticket == cancel.ticket; });
        }
        for (const auto& start : cache.take_starts()) {
            pending.push_back({start.source, start.ticket, static_cast<int>(frame) + 3});
            if (holding) {
                ++result.holdDownloads;
            }
        }
        auto release = [&](const std::string& key) {
            if (rml.erase(key) == 0) {
                ++result.releasesUnknownToRml;
            }
        };
        for (const auto& key : cache.take_publishes()) {
            release(key);
        }
        for (const auto& key : cache.take_releases()) {
            rml.erase(key);  // forgotten placeholders may never have been loaded
        }

        // The browser's hints (recomputed every frame, as when the layout jitters).
        std::vector<std::pair<std::string, ri::Priority>> hints;
        for (int card = 0; card < cards; ++card) {
            const int row = card / 2;
            auto priority = ri::Priority::Deferred;
            if (row >= top && row < top + visibleRows) {
                priority = ri::Priority::Visible;
            } else if (row == top - 1 || row == top + visibleRows) {
                priority = ri::Priority::Ahead;
            }
            hints.emplace_back(hintKey(card * 2), priority);
            hints.emplace_back(hintKey(card * 2 + 1), priority);
        }
        cache.set_hints(1, hints);
        for (const auto& key : cache.take_releases()) {
            rml.erase(key);
        }
        for (const auto& cancel : cache.take_cancels()) {
            std::erase_if(pending, [&](const Pending& p) { return p.source == cancel.source && p.ticket == cancel.ticket; });
        }

        // RmlUi render: only what is inside the clip region asks for a texture.
        for (int card = top * 2; card < std::min(cards, (top + visibleRows) * 2); ++card) {
            for (int image : {card * 2, card * 2 + 1}) {
                const std::string key = rmlKey(image);
                auto it = rml.find(key);
                if (it == rml.end()) {
                    const auto info = ri::parse_source(key, 256);
                    const auto lookup = cache.lookup(key, info.placeholderWidth, info.placeholderHeight);
                    it = rml.emplace(key, lookup.image != nullptr).first;
                }
                if (shownReal[static_cast<std::size_t>(image)] && !it->second) {
                    ++result.visibleDisappeared;
                }
                shownReal[static_cast<std::size_t>(image)] = it->second;
            }
        }

        // How many cache entries one thumbnail occupies right now.
        for (int image = 0; image < images; ++image) {
            const std::uint64_t entries = (cache.contains(hintKey(image)) ? 1u : 0u) +
                                          (rmlKey(image) != hintKey(image) && cache.contains(rmlKey(image)) ? 1u : 0u);
            result.maxEntriesPerThumbnail = std::max(result.maxEntriesPerThumbnail, entries);
        }
    }
    const auto& stats = cache.stats();
    result.requested = stats.created;
    result.downloads = stats.started;
    result.evictions = stats.evicted;
    result.published = stats.published;
    result.thumbnails = static_cast<std::uint64_t>(images);
    return result;
}

}  // namespace flicker_sim
