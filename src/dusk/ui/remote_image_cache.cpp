// H34: see remote_image_cache.hpp.
#include "remote_image_cache.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <system_error>
#include <tuple>

namespace dusk::ui::remote_image {
namespace {

bool parse_u32(std::string_view text, std::uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

}  // namespace

std::string make_source(
    std::string_view url, std::uint32_t width, std::uint32_t height, std::uint32_t maxDimension) {
    url = url.substr(0, url.find('#'));
    std::string source;
    source.reserve(url.size() + 32);
    source.append(url);
    source.append("#size=");
    source.append(std::to_string(width == 0 ? 1 : width));
    source.push_back('x');
    source.append(std::to_string(height == 0 ? 1 : height));
    if (maxDimension != 0) {
        source.append(kMaxMarker);
        source.append(std::to_string(maxDimension));
    }
    return source;
}

SourceInfo parse_source(std::string_view source, std::uint32_t placeholderMaxDimension) {
    constexpr std::string_view marker{"#size="};
    const auto fragment = source.find('#');
    SourceInfo result{.requestUrl = source.substr(0, fragment)};
    if (fragment == std::string_view::npos || !source.substr(fragment).starts_with(marker)) {
        return result;
    }

    auto dimensions = source.substr(fragment + marker.size());
    std::uint32_t maxDimension = 0;
    // The canonical separator first; the H34-c1 forms are only read, never written.
    for (const std::string_view maxMarker : {kMaxMarker, kLegacyEscapedMaxMarker, kLegacyMaxMarker}) {
        if (const auto maxPos = dimensions.find(maxMarker); maxPos != std::string_view::npos) {
            if (!parse_u32(dimensions.substr(maxPos + maxMarker.size()), maxDimension)) {
                maxDimension = 0;
            }
            dimensions = dimensions.substr(0, maxPos);
            break;
        }
    }
    const auto separator = dimensions.find('x');
    if (separator == std::string_view::npos) {
        return result;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!parse_u32(dimensions.substr(0, separator), width) ||
        !parse_u32(dimensions.substr(separator + 1), height) || width == 0 || height == 0)
    {
        return result;
    }
    result.maxDimension = maxDimension;

    const auto largest = std::max(width, height);
    if (placeholderMaxDimension != 0 && largest > placeholderMaxDimension) {
        width = std::max(1u, static_cast<std::uint32_t>(
                                 (static_cast<std::uint64_t>(width) * placeholderMaxDimension +
                                  largest / 2) /
                                 largest));
        height = std::max(1u, static_cast<std::uint32_t>(
                                  (static_cast<std::uint64_t>(height) * placeholderMaxDimension +
                                   largest / 2) /
                                  largest));
    }
    result.placeholderWidth = width;
    result.placeholderHeight = height;
    return result;
}

std::string canonical_source(std::string_view source) {
    const auto fragment = source.find('#');
    if (fragment == std::string_view::npos || !source.substr(fragment).starts_with("#size=")) {
        return std::string{source};
    }
    const auto info = parse_source(source, 0);
    if (info.placeholderWidth == 1 && info.placeholderHeight == 1 &&
        !source.substr(fragment).starts_with("#size=1x1"))
    {
        return std::string{source};  // malformed size: not ours to rewrite
    }
    return make_source(info.requestUrl, info.placeholderWidth, info.placeholderHeight, info.maxDimension);
}

Image downscale_to_fit(Image image, std::uint32_t maxDimension) {
    const std::uint32_t width = image.width;
    const std::uint32_t height = image.height;
    if (maxDimension == 0 || width == 0 || height == 0 ||
        (width <= maxDimension && height <= maxDimension) ||
        image.pixels.size() < static_cast<std::size_t>(width) * height * 4)
    {
        return image;
    }
    const std::uint32_t largest = std::max(width, height);
    const auto scaled = [&](std::uint32_t edge) {
        return std::max(1u, static_cast<std::uint32_t>(
                                (static_cast<std::uint64_t>(edge) * maxDimension + largest / 2) /
                                largest));
    };
    const std::uint32_t outWidth = std::min(scaled(width), maxDimension);
    const std::uint32_t outHeight = std::min(scaled(height), maxDimension);

    Image out;
    out.width = outWidth;
    out.height = outHeight;
    out.pixels.resize(static_cast<std::size_t>(outWidth) * outHeight * 4);
    for (std::uint32_t oy = 0; oy < outHeight; ++oy) {
        const auto y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(oy) * height / outHeight);
        auto y1 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(oy + 1) * height / outHeight);
        y1 = std::max(y1, y0 + 1);
        for (std::uint32_t ox = 0; ox < outWidth; ++ox) {
            const auto x0 =
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(ox) * width / outWidth);
            auto x1 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(ox + 1) * width / outWidth);
            x1 = std::max(x1, x0 + 1);
            std::uint64_t sum[4] = {0, 0, 0, 0};
            for (std::uint32_t y = y0; y < y1; ++y) {
                const std::uint8_t* row = image.pixels.data() + (static_cast<std::size_t>(y) * width + x0) * 4;
                for (std::uint32_t x = x0; x < x1; ++x, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                }
            }
            const std::uint64_t count = static_cast<std::uint64_t>(y1 - y0) * (x1 - x0);
            std::uint8_t* dst = out.pixels.data() + (static_cast<std::size_t>(oy) * outWidth + ox) * 4;
            for (int c = 0; c < 4; ++c) {
                dst[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
            }
        }
    }
    return out;
}

Priority Cache::priority_locked(const std::string& source) const {
    auto best = Priority::Normal;
    bool hinted = false;
    for (const auto& [owner, hints] : mHints) {
        if (const auto it = hints.find(source); it != hints.end()) {
            best = hinted ? std::min(best, it->second) : it->second;
            hinted = true;
        }
    }
    return best;
}

Priority Cache::priority_of(std::string_view source) const {
    return priority_locked(std::string{source});
}

State Cache::state_of(std::string_view source) const {
    const auto it = mEntries.find(std::string{source});
    return it == mEntries.end() ? State::Unrequested : it->second.state;
}

bool Cache::pinned(const std::string& source) const {
    const auto priority = priority_locked(source);
    return priority == Priority::Visible || priority == Priority::Ahead;
}

Cache::Lookup Cache::lookup(
    std::string_view source, std::uint32_t placeholderWidth, std::uint32_t placeholderHeight) {
    ++mStats.lookups;
    ++mClock;
    const std::string key{source};
    auto it = mEntries.find(key);
    if (it == mEntries.end()) {
        if (mEntries.size() >= mLimits.maxTrackedEntries) {
            trim_tracked();
        }
        Entry entry;
        entry.sequence = mClock;
        entry.placeholderWidth = std::max(1u, placeholderWidth);
        entry.placeholderHeight = std::max(1u, placeholderHeight);
        it = mEntries.emplace(key, std::move(entry)).first;
        ++mStats.created;
    }
    auto& entry = it->second;
    entry.lastUsed = mClock;
    entry.lookedUp = true;
    Lookup result{
        .state = entry.state,
        .placeholderWidth = entry.placeholderWidth,
        .placeholderHeight = entry.placeholderHeight,
    };
    if (entry.state == State::Ready) {
        ++mStats.hits;
        entry.publishPending = false;
        result.image = &entry.image;
    }
    return result;
}

void Cache::forget(std::unordered_map<std::string, Entry>::iterator it, bool countAsEviction) {
    auto& entry = it->second;
    if (entry.state == State::Ready) {
        mReadyBytes -= entry.image.pixels.size();
        --mReadyEntries;
    }
    if (entry.state == State::Pending) {
        mCancels.push_back({.source = it->first, .ticket = entry.ticket});
        --mPending;
        ++mStats.canceled;
    }
    if (countAsEviction) {
        ++mStats.evicted;
    } else {
        ++mStats.dropped;
    }
    mReleases.push_back(it->first);
    mEntries.erase(it);
}

void Cache::set_hints(
    std::uint64_t owner, const std::vector<std::pair<std::string, Priority>>& hints) {
    std::unordered_map<std::string, Priority> next;
    next.reserve(hints.size());
    for (const auto& [source, priority] : hints) {
        auto [it, inserted] = next.emplace(source, priority);
        if (!inserted) {
            it->second = std::min(it->second, priority);
        }
    }
    std::unordered_map<std::string, Priority> previous;
    if (const auto it = mHints.find(owner); it != mHints.end()) {
        previous = std::move(it->second);
        mHints.erase(it);
    }
    if (!next.empty()) {
        mHints.emplace(owner, std::move(next));
    }
    ++mClock;
    for (const auto& [source, priority] : previous) {
        const auto& current = mHints.find(owner);
        if (current != mHints.end() && current->second.contains(source)) {
            continue;
        }
        bool otherwiseHinted = false;
        for (const auto& [other, otherHints] : mHints) {
            if (otherHints.contains(source)) {
                otherwiseHinted = true;
                break;
            }
        }
        if (otherwiseHinted) {
            continue;
        }
        if (const auto entry = mEntries.find(source);
            entry != mEntries.end() && entry->second.state != State::Ready)
        {
            forget(entry, false);
        }
    }
    if (const auto current = mHints.find(owner); current != mHints.end()) {
        for (const auto& [source, priority] : current->second) {
            if (priority != Priority::Visible && priority != Priority::Ahead) {
                continue;
            }
            auto entry = mEntries.find(source);
            if (entry == mEntries.end()) {
                if (mEntries.size() >= mLimits.maxTrackedEntries) {
                    trim_tracked();
                }
                const auto info = parse_source(source, mLimits.placeholderMaxDimension);
                Entry created;
                created.sequence = mClock;
                created.placeholderWidth = info.placeholderWidth;
                created.placeholderHeight = info.placeholderHeight;
                entry = mEntries.emplace(source, std::move(created)).first;
                ++mStats.created;
            }
            entry->second.lastUsed = mClock;
        }
    }
}

std::vector<Start> Cache::take_starts() {
    struct Candidate {
        Priority priority;
        std::uint64_t sequence;
        const std::string* source;
    };
    std::vector<Candidate> candidates;
    for (const auto& [source, entry] : mEntries) {
        if (entry.state != State::Unrequested || entry.retryAt > mNow) {
            continue;
        }
        const auto priority = priority_locked(source);
        if (priority == Priority::Deferred) {
            continue;
        }
        candidates.push_back({priority, entry.sequence, &source});
    }
    mStats.maxQueued = std::max(mStats.maxQueued, candidates.size());
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return std::tie(a.priority, a.sequence) < std::tie(b.priority, b.sequence);
    });

    std::vector<Start> starts;
    for (const auto& candidate : candidates) {
        if (mPending >= mLimits.maxPending) {
            break;
        }
        auto& entry = mEntries.at(*candidate.source);
        entry.state = State::Pending;
        entry.ticket = ++mTickets;
        ++entry.attempts;
        ++mPending;
        ++mStats.started;
        const auto info = parse_source(*candidate.source, 0);
        starts.push_back({
            .source = *candidate.source,
            .url = std::string{info.requestUrl},
            .maxDimension = info.maxDimension,
            .ticket = entry.ticket,
        });
    }
    mStats.maxPending = std::max(mStats.maxPending, mPending);
    return starts;
}

bool Cache::make_room(std::size_t incomingBytes, const std::string& incoming) {
    if (incomingBytes > mLimits.maxReadyBytes) {
        return false;
    }
    while (mReadyBytes + incomingBytes > mLimits.maxReadyBytes ||
           mReadyEntries + 1 > mLimits.maxReadyEntries)
    {
        auto victim = mEntries.end();
        for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
            const auto& entry = it->second;
            if (entry.state != State::Ready || it->first == incoming || pinned(it->first)) {
                continue;
            }
            if (victim == mEntries.end()) {
                victim = it;
                continue;
            }
            // Already handed to RmlUi before still waiting, then least recently used.
            const auto key = [](const Entry& e) { return std::tuple{e.publishPending, e.lastUsed}; };
            if (key(entry) < key(victim->second)) {
                victim = it;
            }
        }
        if (victim == mEntries.end()) {
            return false;
        }
        forget(victim, true);
    }
    return true;
}

void Cache::trim_tracked() {
    while (mEntries.size() >= mLimits.maxTrackedEntries) {
        auto victim = mEntries.end();
        for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
            const auto state = it->second.state;
            if ((state != State::Unrequested && state != State::Failed) || pinned(it->first)) {
                continue;
            }
            if (victim == mEntries.end() || it->second.lastUsed < victim->second.lastUsed) {
                victim = it;
            }
        }
        if (victim == mEntries.end()) {
            return;
        }
        forget(victim, false);
    }
}

std::uint32_t Cache::attempts_of(std::string_view source) const {
    const auto it = mEntries.find(std::string{source});
    return it == mEntries.end() ? 0 : it->second.attempts;
}

Cache::Completion Cache::complete(
    const std::string& source, std::uint64_t ticket, FetchOutcome outcome) {
    const auto it = mEntries.find(source);
    if (it == mEntries.end() || it->second.state != State::Pending || it->second.ticket != ticket) {
        ++mStats.stale;
        return Completion::Stale;
    }
    auto& entry = it->second;
    --mPending;
    entry.ticket = 0;
    mStats.networkMsTotal += outcome.networkMs;
    mStats.networkMsMax = std::max(mStats.networkMsMax, outcome.networkMs);

    // V1-rc5: transient failures are requested again, later, up to maxAttempts.
    const bool retryable =
        outcome.kind == FetchOutcome::Kind::NetworkError || outcome.kind == FetchOutcome::Kind::DecodeError ||
        (outcome.kind == FetchOutcome::Kind::HttpError &&
            (outcome.httpStatus == 0 || outcome.httpStatus == 408 || outcome.httpStatus == 429 ||
                outcome.httpStatus >= 500));
    if (retryable && entry.attempts < mLimits.maxAttempts) {
        static constexpr double kBackoffSeconds[] = {1.0, 3.0, 8.0};
        const std::size_t step = std::min<std::size_t>(entry.attempts, std::size(kBackoffSeconds)) - 1;
        entry.state = State::Unrequested;
        entry.retryAt = mNow + kBackoffSeconds[step];
        return Completion::Retry;
    }

    switch (outcome.kind) {
    case FetchOutcome::Kind::Ok:
        break;
    case FetchOutcome::Kind::DecodeError:
        ++mStats.downloaded;
        mStats.downloadBytes += outcome.bodyBytes;
        ++mStats.decodeFailed;
        mStats.decodeMsTotal += outcome.decodeMs;
        mStats.decodeMsMax = std::max(mStats.decodeMsMax, outcome.decodeMs);
        entry.state = State::Failed;
        return Completion::Failed;
    case FetchOutcome::Kind::Canceled:
        ++mStats.canceled;
        entry.state = State::Failed;
        return Completion::Failed;
    case FetchOutcome::Kind::NetworkError:
    case FetchOutcome::Kind::HttpError:
        ++mStats.networkFailed;
        entry.state = State::Failed;
        return Completion::Failed;
    }

    ++mStats.downloaded;
    mStats.downloadBytes += outcome.bodyBytes;
    ++mStats.decoded;
    mStats.decodeMsTotal += outcome.decodeMs;
    mStats.decodeMsMax = std::max(mStats.decodeMsMax, outcome.decodeMs);
    if (outcome.decodedWidth != outcome.image.width || outcome.decodedHeight != outcome.image.height) {
        ++mStats.downscaled;
    }
    const auto bytes = outcome.image.pixels.size();
    if (outcome.image.width == 0 || outcome.image.height == 0 ||
        bytes < static_cast<std::size_t>(outcome.image.width) * outcome.image.height * 4)
    {
        ++mStats.decodeFailed;
        entry.state = State::Failed;
        return Completion::Failed;
    }
    if (!make_room(bytes, source)) {
        ++mStats.rejected;
        // make_room may have evicted others but never this entry.
        mEntries.at(source).state = State::Failed;
        return Completion::Rejected;
    }
    auto& stored = mEntries.at(source);
    stored.image = std::move(outcome.image);
    stored.state = State::Ready;
    stored.publishPending = true;
    stored.readyOrder = ++mClock;
    mReadyBytes += bytes;
    ++mReadyEntries;
    mStats.peakBytes = std::max(mStats.peakBytes, mReadyBytes);
    return Completion::Stored;
}

std::vector<std::string> Cache::take_publishes() {
    struct Candidate {
        Priority priority;
        std::uint64_t order;
        std::size_t bytes;
        const std::string* source;
    };
    std::vector<Candidate> candidates;
    for (const auto& [source, entry] : mEntries) {
        // An image RmlUi has not asked for yet needs no release: its first load will
        // find it ready.
        if (entry.state == State::Ready && entry.publishPending && entry.lookedUp) {
            candidates.push_back(
                {priority_locked(source), entry.readyOrder, entry.image.pixels.size(), &source});
        }
    }
    mStats.maxPublishQueue = std::max(mStats.maxPublishQueue, candidates.size());
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return std::tie(a.priority, a.order) < std::tie(b.priority, b.order);
    });
    std::vector<std::string> publishes;
    std::size_t bytes = 0;
    for (const auto& candidate : candidates) {
        if (publishes.size() >= mLimits.maxPublishesPerFrame) {
            break;
        }
        if (!publishes.empty() && bytes + candidate.bytes > mLimits.publishBytesPerFrame) {
            break;
        }
        bytes += candidate.bytes;
        mEntries.at(*candidate.source).publishPending = false;
        ++mStats.published;
        publishes.push_back(*candidate.source);
    }
    return publishes;
}

std::vector<std::string> Cache::clear() {
    std::vector<std::string> sources;
    sources.reserve(mEntries.size());
    for (const auto& [source, entry] : mEntries) {
        sources.push_back(source);
    }
    mEntries.clear();
    mHints.clear();
    mReleases.clear();
    mCancels.clear();
    mPending = 0;
    mReadyBytes = 0;
    mReadyEntries = 0;
    return sources;
}

}  // namespace dusk::ui::remote_image
