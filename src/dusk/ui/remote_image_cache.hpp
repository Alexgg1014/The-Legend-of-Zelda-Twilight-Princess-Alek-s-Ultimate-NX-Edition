#pragma once
// H34: the bookkeeping behind the remote (https://) texture provider, without RmlUi,
// borealis or the GPU, so the host test can drive it.
//
// Where the work happens (see remote_texture_provider.cpp for the glue):
//
//   main thread   lookup() from RmlUi's texture load, hints from the mod browser,
//                 take_starts() / complete() / take_publishes() / take_releases() once
//                 per UI update. All of Cache is single-threaded: only that thread
//                 touches it, so it has no locks.
//   worker        the HTTP request, PNG decode, premultiply and downscale_to_fit(),
//                 inside the borealis task (Task::map runs on the completing worker).
//                 The worker owns its buffers and hands a finished FetchOutcome back;
//                 it never sees the Cache.
//   render        RmlUi creates and uploads the GPU texture when it reloads a source
//                 (aurora's WebGPURenderInterface::LoadTexture). That stays where RmlUi
//                 calls it; the cache only decides *when* a finished image is released
//                 to RmlUi, under a per-frame byte budget, visible images first.
//
// Requests carry a ticket. A result whose entry was dropped, restarted or cleared
// since the request began is stale and discarded, which is how a page that was left
// cannot write into the cache after the fact.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dusk::ui::remote_image {

enum class Priority : std::uint8_t {
    Visible = 0,   ///< on screen now
    Ahead = 1,     ///< the row just past the viewport
    Normal = 2,    ///< nobody said: detail pages, queue icons, anything unhinted
    Deferred = 3,  ///< a browser card off screen: listed, not fetched
};

enum class State : std::uint8_t { Unrequested, Pending, Ready, Failed };

/// Premultiplied RGBA8.
struct Image {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// What a source string says: the URL to fetch, the placeholder size (the image's
/// aspect ratio, which RmlUi keeps from the first texture it is given) and the largest
/// edge the caller will ever draw it at (0 = no limit).
struct SourceInfo {
    std::string_view requestUrl;
    std::uint32_t placeholderWidth = 1;
    std::uint32_t placeholderHeight = 1;
    std::uint32_t maxDimension = 0;
};

/// The canonical source is "<url>#size=WxH" or "<url>#size=WxH,max=N".
///
/// H34-c2: the separator before `max` is a comma. The source is written into a decorator
/// property through ui::escape(), and RmlUi does not unescape property values, so the key
/// RmlUi hands the provider is the escaped text verbatim. H34-c1 used "&max=", which
/// became "&amp;max=" on the way: the hints and RmlUi then named the same thumbnail with
/// two keys, the on-screen one was never pinned, got evicted and flickered back to its
/// placeholder. A comma survives escape() unchanged and RmlUi only splits decorators on
/// commas outside parentheses, so every stage now sees the same string.
inline constexpr std::string_view kMaxMarker{",max="};
/// Read for robustness only; make_source never writes them.
inline constexpr std::string_view kLegacyMaxMarker{"&max="};
inline constexpr std::string_view kLegacyEscapedMaxMarker{"&amp;max="};

std::string make_source(
    std::string_view url, std::uint32_t width, std::uint32_t height, std::uint32_t maxDimension);
SourceInfo parse_source(std::string_view source, std::uint32_t placeholderMaxDimension);
/// The canonical form of a source (a legacy "&max=" or "&amp;max=" becomes ",max=").
/// Anything without a valid "#size=" fragment is returned unchanged.
std::string canonical_source(std::string_view source);

/// Area-average downscale so neither edge exceeds `maxDimension`, preserving the
/// aspect ratio (rounded, at least 1x1). Returns the input untouched when it already
/// fits or `maxDimension` is 0. Works on premultiplied pixels, where averaging is
/// correct.
Image downscale_to_fit(Image image, std::uint32_t maxDimension);

struct Limits {
    std::size_t maxReadyEntries = 64;               ///< images held decoded
    std::size_t maxReadyBytes = 64u * 1024 * 1024;  ///< their pixels
    std::size_t maxTrackedEntries = 256;            ///< including placeholders and failures
    std::size_t maxPending = 4;                     ///< requests in flight
    std::size_t publishBytesPerFrame = 1024 * 1024; ///< at least one image per frame
    std::size_t maxPublishesPerFrame = 4;
    std::uint32_t placeholderMaxDimension = 256;    ///< for entries a hint creates
};

struct FetchOutcome {
    enum class Kind : std::uint8_t { Ok, NetworkError, HttpError, DecodeError, Canceled };
    Kind kind = Kind::NetworkError;
    Image image;
    std::string message;
    std::size_t bodyBytes = 0;
    double networkMs = 0;  ///< measured on the worker
    double decodeMs = 0;   ///< decode + premultiply + downscale, on the worker
    std::uint32_t decodedWidth = 0;   ///< before downscale
    std::uint32_t decodedHeight = 0;
};

struct Start {
    std::string source;
    std::string url;
    std::uint32_t maxDimension = 0;
    std::uint64_t ticket = 0;
};

struct Cancel {
    std::string source;
    std::uint64_t ticket = 0;
};

struct Stats {
    std::uint64_t lookups = 0;
    std::uint64_t created = 0;      ///< distinct sources first seen
    std::uint64_t hits = 0;         ///< lookups answered with a decoded image
    std::uint64_t started = 0;      ///< network requests
    std::uint64_t downloaded = 0;   ///< successful responses
    std::uint64_t downloadBytes = 0;
    std::uint64_t decoded = 0;
    std::uint64_t downscaled = 0;
    std::uint64_t decodeFailed = 0;
    std::uint64_t networkFailed = 0;
    std::uint64_t canceled = 0;     ///< in flight when nobody wanted them any more
    std::uint64_t stale = 0;        ///< results discarded by ticket
    std::uint64_t rejected = 0;     ///< did not fit the memory budget
    std::uint64_t published = 0;
    std::uint64_t evicted = 0;
    std::uint64_t dropped = 0;      ///< placeholders/failures forgotten
    std::size_t maxPending = 0;
    std::size_t maxQueued = 0;      ///< startable entries waiting for a slot
    std::size_t maxPublishQueue = 0;
    std::size_t peakBytes = 0;
    double networkMsTotal = 0;
    double networkMsMax = 0;
    double decodeMsTotal = 0;
    double decodeMsMax = 0;
};

class Cache {
public:
    explicit Cache(Limits limits = {}) : mLimits(limits) {}

    struct Lookup {
        State state = State::Unrequested;
        const Image* image = nullptr;  ///< valid until the next mutating call
        std::uint32_t placeholderWidth = 1;
        std::uint32_t placeholderHeight = 1;
    };

    /// RmlUi wants `source`. Creates the entry on first sight; never starts anything
    /// itself. A Ready answer counts as the image being handed to RmlUi.
    Lookup lookup(std::string_view source, std::uint32_t placeholderWidth,
                  std::uint32_t placeholderHeight);

    /// Replaces everything `owner` said before. A Visible or Ahead source that has no
    /// entry yet gets one: RmlUi only loads a decorator's texture once the element is
    /// inside the clipping region, so without this the row ahead would never be
    /// fetched before it scrolls into view. Entries the owner hinted earlier and
    /// no longer mentions, that nobody else hints and that are not decoded, are
    /// forgotten: an in-flight request becomes a Cancel, and the source goes to
    /// take_releases() so RmlUi drops its placeholder and asks again if it is still
    /// shown somewhere.
    void set_hints(std::uint64_t owner, const std::vector<std::pair<std::string, Priority>>& hints);
    void clear_hints(std::uint64_t owner) { set_hints(owner, {}); }

    /// Requests to begin now, best priority first, oldest first within a priority.
    /// Deferred entries never start.
    std::vector<Start> take_starts();

    enum class Completion : std::uint8_t { Stored, Stale, Failed, Rejected };
    Completion complete(const std::string& source, std::uint64_t ticket, FetchOutcome outcome);

    /// Decoded images to hand to RmlUi this frame (it reloads a source after
    /// Rml::ReleaseTexture), within the per-frame budget.
    std::vector<std::string> take_publishes();

    /// Sources RmlUi must forget (evicted, or placeholders whose entry was dropped).
    std::vector<std::string> take_releases() { return std::exchange(mReleases, {}); }
    std::vector<Cancel> take_cancels() { return std::exchange(mCancels, {}); }

    /// Forgets everything. The caller cancels its own requests; late results are stale.
    std::vector<std::string> clear();

    const Stats& stats() const { return mStats; }
    /// Starts a new telemetry window for the maxima; counters keep counting.
    void reset_peaks() {
        mStats.maxPending = mPending;
        mStats.maxQueued = 0;
        mStats.maxPublishQueue = 0;
        mStats.peakBytes = mReadyBytes;
        mStats.networkMsMax = 0;
        mStats.decodeMsMax = 0;
    }
    std::size_t pending() const { return mPending; }
    std::size_t ready_bytes() const { return mReadyBytes; }
    std::size_t ready_entries() const { return mReadyEntries; }
    std::size_t tracked() const { return mEntries.size(); }
    /// For tests and telemetry: whether `source` has an entry (in any state).
    bool contains(std::string_view source) const { return mEntries.contains(std::string{source}); }
    const Limits& limits() const { return mLimits; }
    /// For tests and telemetry: the priority an entry would be scheduled with.
    Priority priority_of(std::string_view source) const;
    State state_of(std::string_view source) const;

private:
    struct Entry {
        State state = State::Unrequested;
        Image image;
        std::uint64_t sequence = 0;   ///< first seen, for FIFO within a priority
        std::uint64_t lastUsed = 0;
        std::uint64_t ticket = 0;     ///< of the request in flight
        std::uint64_t readyOrder = 0; ///< completion order, for publishing
        std::uint32_t placeholderWidth = 1;
        std::uint32_t placeholderHeight = 1;
        bool publishPending = false;
        bool lookedUp = false;        ///< RmlUi asked for it (holds a placeholder)
    };

    Priority priority_locked(const std::string& source) const;
    bool pinned(const std::string& source) const;
    void forget(std::unordered_map<std::string, Entry>::iterator it, bool countAsEviction);
    bool make_room(std::size_t incomingBytes, const std::string& incoming);
    void trim_tracked();

    Limits mLimits;
    Stats mStats;
    std::unordered_map<std::string, Entry> mEntries;
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, Priority>> mHints;
    std::vector<std::string> mReleases;
    std::vector<Cancel> mCancels;
    std::uint64_t mClock = 0;
    std::uint64_t mTickets = 0;
    std::size_t mPending = 0;
    std::size_t mReadyBytes = 0;
    std::size_t mReadyEntries = 0;
};

}  // namespace dusk::ui::remote_image
