// H34 host test for src/dusk/ui/remote_image_cache.cpp: the scheduling, ticketing,
// publishing and memory policy behind the mod browser's images.
//   platforms/switch/tools/test-remote-image-cache.sh
//
// What this can and cannot show: the cache logic is plain C++ and runs here in full.
// The HTTP request, PNG decode, RmlUi and the GPU upload are not simulated; the test
// feeds FetchOutcome values the way the worker would and checks what the cache does
// with them.
#include "dusk/ui/remote_image_cache.hpp"
#include "remote_image_flicker_sim.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace ri = dusk::ui::remote_image;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

namespace {

std::string src(int i, std::uint32_t max = 0) {
    return ri::make_source("https://twilitrealm.dev/images/v1/img" + std::to_string(i) + ".png",
                           640, 360, max);
}

ri::FetchOutcome ok_image(std::uint32_t w, std::uint32_t h) {
    ri::FetchOutcome outcome;
    outcome.kind = ri::FetchOutcome::Kind::Ok;
    outcome.image.width = w;
    outcome.image.height = h;
    outcome.image.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0x80);
    outcome.decodedWidth = w;
    outcome.decodedHeight = h;
    outcome.bodyBytes = 1000;
    outcome.networkMs = 12.5;
    outcome.decodeMs = 3.0;
    return outcome;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::ranges::find(v, s) != v.end();
}

const ri::Start* find_start(const std::vector<ri::Start>& starts, const std::string& source) {
    for (const auto& start : starts) {
        if (start.source == source) {
            return &start;
        }
    }
    return nullptr;
}

void test_source_format() {
    const auto s = ri::make_source("https://a/b.png#frag", 1280, 720, 640);
    CHECK(s == "https://a/b.png#size=1280x720,max=640");
    auto info = ri::parse_source(s, 256);
    CHECK(info.requestUrl == "https://a/b.png");
    CHECK(info.placeholderWidth == 256 && info.placeholderHeight == 144);
    CHECK(info.maxDimension == 640);

    // The pre-H34 form still parses, with no limit.
    info = ri::parse_source("https://a/b.png#size=64x32", 256);
    CHECK(info.placeholderWidth == 64 && info.placeholderHeight == 32 && info.maxDimension == 0);
    CHECK(ri::make_source("https://a/b.png", 0, 0, 0) == "https://a/b.png#size=1x1");

    // Malformed fragments fall back to a 1x1 placeholder and no limit.
    for (const char* bad : {"https://a/b.png", "https://a/b.png#size=", "https://a/b.png#size=0x5",
                            "https://a/b.png#size=5x", "https://a/b.png#size=axb",
                            "https://a/b.png#other=5x5"}) {
        info = ri::parse_source(bad, 256);
        CHECK(info.placeholderWidth == 1 && info.placeholderHeight == 1 && info.maxDimension == 0);
        CHECK(info.requestUrl == "https://a/b.png");
    }
    info = ri::parse_source("https://a/b.png#size=10x10,max=zz", 256);
    CHECK(info.placeholderWidth == 10 && info.maxDimension == 0);
}

void test_downscale() {
    ri::Image image;
    image.width = 1280;
    image.height = 720;
    image.pixels.assign(1280u * 720u * 4u, 200);
    const auto out = ri::downscale_to_fit(image, 640);
    CHECK(out.width == 640 && out.height == 360);
    CHECK(out.pixels.size() == 640u * 360u * 4u);
    CHECK(std::ranges::all_of(out.pixels, [](std::uint8_t v) { return v == 200; }));

    // Portrait: the long edge is the one limited.
    image.width = 300;
    image.height = 1000;
    image.pixels.assign(300u * 1000u * 4u, 10);
    const auto tall = ri::downscale_to_fit(image, 100);
    CHECK(tall.height == 100 && tall.width == 30);

    // Fits already, or no limit: untouched.
    image.width = 64;
    image.height = 64;
    image.pixels.assign(64u * 64u * 4u, 1);
    CHECK(ri::downscale_to_fit(image, 128).width == 64);
    CHECK(ri::downscale_to_fit(image, 0).width == 64);

    // Averaging: a 2x1 black/white image becomes one mid-grey pixel.
    ri::Image two;
    two.width = 2;
    two.height = 1;
    two.pixels = {0, 0, 0, 255, 255, 255, 255, 255};
    const auto one = ri::downscale_to_fit(two, 1);
    CHECK(one.width == 1 && one.height == 1);
    CHECK(one.pixels[0] == 128 && one.pixels[3] == 255);

    // Extreme aspect never collapses below 1 pixel.
    ri::Image strip;
    strip.width = 4000;
    strip.height = 2;
    strip.pixels.assign(4000u * 2u * 4u, 7);
    const auto thin = ri::downscale_to_fit(strip, 100);
    CHECK(thin.width == 100 && thin.height == 1);

    // A buffer too small for its declared size is returned as is, not read past.
    ri::Image broken;
    broken.width = 100;
    broken.height = 100;
    broken.pixels.assign(16, 0);
    CHECK(ri::downscale_to_fit(broken, 10).width == 100);
}

void test_dedup_and_priority() {
    ri::Cache cache({.maxPending = 2});
    const auto a = src(1), b = src(2), c = src(3), d = src(4);
    // Seen in document order, several times: one entry each.
    for (int round = 0; round < 3; ++round) {
        for (const auto& s : {a, b, c, d}) {
            cache.lookup(s, 64, 36);
        }
    }
    CHECK(cache.tracked() == 4);
    CHECK(cache.stats().created == 4 && cache.stats().lookups == 12);

    // The browser says d is on screen, c is the row ahead, a and b are off screen.
    cache.set_hints(1, {{a, ri::Priority::Deferred}, {b, ri::Priority::Deferred},
                        {c, ri::Priority::Ahead}, {d, ri::Priority::Visible}});
    auto starts = cache.take_starts();
    CHECK(starts.size() == 2);  // the concurrency limit
    CHECK(starts.size() == 2 && starts[0].source == d && starts[1].source == c);
    CHECK(cache.pending() == 2);
    // Nothing more while both slots are busy, and Deferred never starts.
    CHECK(cache.take_starts().empty());
    CHECK(cache.complete(d, starts[0].ticket, ok_image(64, 36)) == ri::Cache::Completion::Stored);
    CHECK(cache.complete(c, starts[1].ticket, ok_image(64, 36)) == ri::Cache::Completion::Stored);
    CHECK(cache.take_starts().empty());
    CHECK(cache.stats().maxPending == 2);

    // Scrolling makes a visible: it starts; b is still deferred.
    cache.set_hints(1, {{a, ri::Priority::Visible}, {b, ri::Priority::Deferred},
                        {c, ri::Priority::Deferred}, {d, ri::Priority::Deferred}});
    starts = cache.take_starts();
    CHECK(starts.size() == 1 && starts[0].source == a);

    // An unhinted source (a detail page, a queue icon) is Normal: after visible and
    // ahead, before deferred, in first-seen order.
    cache.lookup(src(9), 1, 1);
    CHECK(cache.priority_of(src(9)) == ri::Priority::Normal);
    starts = cache.take_starts();
    CHECK(starts.size() == 1 && starts[0].source == src(9));

    // Two owners: the better hint wins.
    cache.set_hints(2, {{b, ri::Priority::Visible}});
    CHECK(cache.priority_of(b) == ri::Priority::Visible);
    cache.clear_hints(2);
    CHECK(cache.priority_of(b) == ri::Priority::Deferred);
}

void test_tickets_and_late_results() {
    ri::Cache cache({.maxPending = 4});
    const auto a = src(1);
    cache.lookup(a, 1, 1);
    auto starts = cache.take_starts();
    CHECK(starts.size() == 1);
    const auto first = starts[0].ticket;

    // A wrong ticket is stale and changes nothing.
    CHECK(cache.complete(a, first + 99, ok_image(4, 4)) == ri::Cache::Completion::Stale);
    CHECK(cache.state_of(a) == ri::State::Pending && cache.pending() == 1);

    // The owner that hinted it stops caring before it finishes: it is canceled...
    cache.set_hints(7, {{a, ri::Priority::Visible}});
    cache.clear_hints(7);
    const auto cancels = cache.take_cancels();
    CHECK(cancels.size() == 1 && cancels[0].source == a && cancels[0].ticket == first);
    CHECK(cache.pending() == 0 && cache.stats().canceled == 1);
    CHECK(contains(cache.take_releases(), a));  // RmlUi forgets the placeholder
    // ...and the result that arrives afterwards is discarded.
    CHECK(cache.complete(a, first, ok_image(4, 4)) == ri::Cache::Completion::Stale);
    CHECK(cache.tracked() == 0 && cache.ready_bytes() == 0);
    CHECK(cache.stats().stale == 2);

    // Shown again later: a new entry and a new ticket.
    cache.lookup(a, 1, 1);
    starts = cache.take_starts();
    CHECK(starts.size() == 1 && starts[0].ticket != first);
    CHECK(cache.complete(a, first, ok_image(4, 4)) == ri::Cache::Completion::Stale);
    CHECK(cache.complete(a, starts[0].ticket, ok_image(4, 4)) == ri::Cache::Completion::Stored);
    // A second completion with the same ticket is stale too.
    CHECK(cache.complete(a, starts[0].ticket, ok_image(4, 4)) == ri::Cache::Completion::Stale);
}

void test_failures() {
    ri::Cache cache;
    const auto net = src(1), http = src(2), bad = src(3), empty = src(4);
    for (const auto& s : {net, http, bad, empty}) {
        cache.lookup(s, 1, 1);
    }
    const auto starts = cache.take_starts();
    CHECK(starts.size() == 4);
    ri::FetchOutcome failure;
    failure.kind = ri::FetchOutcome::Kind::NetworkError;
    CHECK(cache.complete(net, find_start(starts, net)->ticket, failure) == ri::Cache::Completion::Failed);
    failure.kind = ri::FetchOutcome::Kind::HttpError;
    CHECK(cache.complete(http, find_start(starts, http)->ticket, failure) == ri::Cache::Completion::Failed);
    failure.kind = ri::FetchOutcome::Kind::DecodeError;
    CHECK(cache.complete(bad, find_start(starts, bad)->ticket, failure) == ri::Cache::Completion::Failed);
    // "Ok" with no pixels is treated as a bad image, never stored.
    ri::FetchOutcome hollow;
    hollow.kind = ri::FetchOutcome::Kind::Ok;
    hollow.image.width = 10;
    hollow.image.height = 10;
    CHECK(cache.complete(empty, find_start(starts, empty)->ticket, hollow) == ri::Cache::Completion::Failed);
    CHECK(cache.stats().networkFailed == 2 && cache.stats().decodeFailed == 2);
    CHECK(cache.pending() == 0 && cache.ready_entries() == 0);
    // A failure is not retried in a loop: it keeps its placeholder.
    CHECK(cache.take_starts().empty());
    CHECK(cache.lookup(net, 1, 1).state == ri::State::Failed);
    CHECK(cache.take_publishes().empty());
}

// V1-rc5: with maxAttempts, transient failures come back after a delay; permanent ones do not.
void test_retries() {
    ri::Limits limits;
    limits.maxAttempts = 3;
    ri::Cache cache{limits};
    const auto net = src(1), gone = src(2), busy = src(3);
    for (const auto& s : {net, gone, busy}) {
        cache.lookup(s, 1, 1);
    }
    auto starts = cache.take_starts();
    CHECK(starts.size() == 3);
    ri::FetchOutcome failure;
    failure.kind = ri::FetchOutcome::Kind::NetworkError;
    CHECK(cache.complete(net, find_start(starts, net)->ticket, failure) == ri::Cache::Completion::Retry);
    failure.kind = ri::FetchOutcome::Kind::HttpError;
    failure.httpStatus = 404;
    CHECK(cache.complete(gone, find_start(starts, gone)->ticket, failure) == ri::Cache::Completion::Failed);
    failure.httpStatus = 503;
    CHECK(cache.complete(busy, find_start(starts, busy)->ticket, failure) == ri::Cache::Completion::Retry);
    CHECK(cache.take_starts().empty());  // not before its delay
    cache.set_now(1.5);
    starts = cache.take_starts();
    CHECK(starts.size() == 2 && cache.attempts_of(net) == 2);
    CHECK(cache.complete(busy, find_start(starts, busy)->ticket, ok_image(4, 4)) == ri::Cache::Completion::Stored);
    failure.kind = ri::FetchOutcome::Kind::NetworkError;
    CHECK(cache.complete(net, find_start(starts, net)->ticket, failure) == ri::Cache::Completion::Retry);
    cache.set_now(3.0);
    CHECK(cache.take_starts().empty());  // the second delay is longer
    cache.set_now(5.0);
    starts = cache.take_starts();
    CHECK(starts.size() == 1);
    CHECK(cache.complete(net, starts[0].ticket, failure) == ri::Cache::Completion::Failed);
    CHECK(cache.attempts_of(net) == 3 && cache.state_of(net) == ri::State::Failed);
    cache.set_now(100.0);
    CHECK(cache.take_starts().empty());
}

void test_hits_and_publish_budget() {
    ri::Cache cache({.publishBytesPerFrame = 1024 * 1024, .maxPublishesPerFrame = 4});
    std::vector<std::string> sources;
    for (int i = 0; i < 6; ++i) {
        sources.push_back(src(i));
        cache.lookup(sources.back(), 1, 1);
    }
    auto starts = cache.take_starts();
    CHECK(starts.size() == 4);
    // Two big banners (~0.9 MiB) and two small icons finish in the same frame.
    for (const auto& start : starts) {
        const bool big = start.source == sources[0] || start.source == sources[1];
        cache.complete(start.source, start.ticket, big ? ok_image(640, 360) : ok_image(64, 64));
    }
    // Two big images never go out in the same frame; a small one may follow a big
    // one while the frame's budget allows it.
    auto first = cache.take_publishes();
    CHECK(first.size() == 1 && first[0] == sources[0]);
    auto second = cache.take_publishes();
    CHECK(second.size() == 3 && second[0] == sources[1] && second[1] == sources[2] &&
          second[2] == sources[3]);
    CHECK(cache.take_publishes().empty());
    CHECK(cache.stats().published == 4 && cache.stats().maxPublishQueue == 4);

    // RmlUi reloading a published source is a hit, served from memory with no request.
    const auto hit = cache.lookup(sources[0], 1, 1);
    CHECK(hit.state == ri::State::Ready && hit.image != nullptr && hit.image->width == 640);
    CHECK(cache.stats().hits == 1);
    const auto startedBefore = cache.stats().started;
    for (int i = 0; i < 5; ++i) {
        cache.lookup(sources[0], 1, 1);
    }
    CHECK(cache.stats().started == startedBefore);  // never downloaded twice
    CHECK(cache.stats().decoded == 4);              // never decoded twice

    // Visible publishes before Normal even when it finished later.
    starts = cache.take_starts();
    CHECK(starts.size() == 2);
    cache.set_hints(3, {{sources[5], ri::Priority::Visible}});
    cache.complete(sources[4], find_start(starts, sources[4])->ticket, ok_image(8, 8));
    cache.complete(sources[5], find_start(starts, sources[5])->ticket, ok_image(8, 8));
    const auto order = cache.take_publishes();
    CHECK(order.size() == 2 && order[0] == sources[5]);
    CHECK(cache.stats().networkMsMax == 12.5 && cache.stats().decodeMsTotal > 0);
}

void test_memory_and_eviction() {
    // Room for three 64x64 images (16 KiB each).
    ri::Cache cache({.maxReadyEntries = 3, .maxReadyBytes = 3 * 64 * 64 * 4, .maxPending = 8});
    std::vector<std::string> s;
    for (int i = 0; i < 5; ++i) {
        s.push_back(src(i));
    }
    cache.set_hints(1, {{s[0], ri::Priority::Visible}});
    for (int i = 0; i < 3; ++i) {
        cache.lookup(s[i], 1, 1);
    }
    auto starts = cache.take_starts();
    for (const auto& start : starts) {
        cache.complete(start.source, start.ticket, ok_image(64, 64));
    }
    cache.take_publishes();
    CHECK(cache.ready_entries() == 3 && cache.ready_bytes() == 3u * 64 * 64 * 4);
    cache.lookup(s[2], 1, 1);  // s[1] is now least recently used
    cache.lookup(s[3], 1, 1);
    starts = cache.take_starts();
    CHECK(starts.size() == 1);
    CHECK(cache.complete(s[3], starts[0].ticket, ok_image(64, 64)) == ri::Cache::Completion::Stored);
    // The visible one (s[0]) is pinned; the LRU unpinned one (s[1]) goes.
    CHECK(cache.state_of(s[1]) == ri::State::Unrequested);  // gone
    CHECK(cache.state_of(s[0]) == ri::State::Ready && cache.state_of(s[2]) == ri::State::Ready);
    CHECK(contains(cache.take_releases(), s[1]));
    CHECK(cache.stats().evicted == 1);
    CHECK(cache.ready_entries() == 3 && cache.ready_bytes() <= cache.limits().maxReadyBytes);

    // An image bigger than the whole budget is refused, nothing else is evicted.
    cache.lookup(s[4], 1, 1);
    starts = cache.take_starts();
    CHECK(cache.complete(s[4], starts[0].ticket, ok_image(200, 200)) == ri::Cache::Completion::Rejected);
    CHECK(cache.state_of(s[4]) == ri::State::Failed && cache.stats().rejected == 1);
    CHECK(cache.ready_entries() == 3);

    // When everything held is pinned, a new image is refused instead of evicting
    // what is on screen.
    cache.set_hints(1, {{s[0], ri::Priority::Visible}, {s[2], ri::Priority::Visible},
                        {s[3], ri::Priority::Ahead}});
    const auto extra = src(42);
    cache.lookup(extra, 1, 1);
    starts = cache.take_starts();
    CHECK(starts.size() == 1);
    CHECK(cache.complete(extra, starts[0].ticket, ok_image(64, 64)) == ri::Cache::Completion::Rejected);
    CHECK(cache.state_of(s[0]) == ri::State::Ready && cache.state_of(s[3]) == ri::State::Ready);
    CHECK(cache.stats().peakBytes <= cache.limits().maxReadyBytes);
}

void test_tracked_limit() {
    ri::Cache cache({.maxTrackedEntries = 8, .maxPending = 0});
    for (int i = 0; i < 50; ++i) {
        cache.lookup(src(i), 1, 1);
    }
    CHECK(cache.tracked() <= 8);
    CHECK(cache.stats().dropped >= 42);
    CHECK(cache.take_releases().size() >= 42);
}

void test_close_with_pending_work() {
    // A browser page with work in flight is closed: its requests are canceled, its
    // placeholders released, the decoded images stay for the next visit, and
    // results that arrive afterwards are discarded.
    ri::Cache cache({.maxPending = 2});
    const auto a = src(1), b = src(2), c = src(3), ready = src(4);
    cache.set_hints(5, {{ready, ri::Priority::Visible}});
    cache.lookup(ready, 1, 1);
    auto starts = cache.take_starts();
    cache.complete(ready, starts[0].ticket, ok_image(8, 8));
    cache.set_hints(5, {{ready, ri::Priority::Visible}, {a, ri::Priority::Visible},
                        {b, ri::Priority::Visible}, {c, ri::Priority::Ahead}});
    for (const auto& s : {a, b, c}) {
        cache.lookup(s, 1, 1);
    }
    starts = cache.take_starts();
    CHECK(starts.size() == 2);
    cache.clear_hints(5);  // the browser's destructor
    const auto cancels = cache.take_cancels();
    CHECK(cancels.size() == 2);
    const auto releases = cache.take_releases();
    CHECK(contains(releases, a) && contains(releases, b) && contains(releases, c));
    CHECK(!contains(releases, ready));
    CHECK(cache.state_of(ready) == ri::State::Ready);
    CHECK(cache.pending() == 0 && cache.take_starts().empty());
    for (const auto& start : starts) {
        CHECK(cache.complete(start.source, start.ticket, ok_image(8, 8)) == ri::Cache::Completion::Stale);
    }
    CHECK(cache.ready_entries() == 1);

    // Unregistering the provider: everything is forgotten, late results are stale.
    cache.lookup(a, 1, 1);
    starts = cache.take_starts();
    const auto all = cache.clear();
    CHECK(all.size() == 2 && cache.tracked() == 0 && cache.pending() == 0 && cache.ready_bytes() == 0);
    CHECK(cache.complete(a, starts[0].ticket, ok_image(8, 8)) == ri::Cache::Completion::Stale);
}

void test_grid_does_not_wait_for_images() {
    // Twenty cards, two columns: the list is usable while every image is still a
    // placeholder, and one slow image does not hold the others back.
    ri::Cache cache({.maxPending = 4});
    std::vector<std::pair<std::string, ri::Priority>> hints;
    for (int i = 0; i < 20; ++i) {
        const auto s = src(i);
        const auto lookup = cache.lookup(s, 64, 36);
        CHECK(lookup.state == ri::State::Unrequested && lookup.image == nullptr);
        CHECK(lookup.placeholderWidth == 64 && lookup.placeholderHeight == 36);
        hints.emplace_back(s, i < 4 ? ri::Priority::Visible :
                              i < 6 ? ri::Priority::Ahead : ri::Priority::Deferred);
    }
    cache.set_hints(1, hints);
    auto starts = cache.take_starts();
    CHECK(starts.size() == 4);
    const auto slow = starts[0];
    // The other three finish; their slots go to the row ahead while the slow one
    // is still out.
    for (std::size_t i = 1; i < starts.size(); ++i) {
        cache.complete(starts[i].source, starts[i].ticket, ok_image(64, 36));
    }
    const auto next = cache.take_starts();
    CHECK(next.size() == 2);  // exactly the Ahead row; Deferred stays put
    CHECK(cache.take_publishes().size() == 3);
    CHECK(cache.state_of(slow.source) == ri::State::Pending);
}

void test_prefetch_ahead() {
    // RmlUi only asks for textures inside the clipping region, so the row ahead has no
    // entry until a hint creates it. It is fetched, never "published" (RmlUi holds no
    // placeholder for it), and its first lookup is a hit with no placeholder at all.
    ri::Cache cache({.maxPending = 4});
    const auto visible = src(1), ahead = src(2), offscreen = src(3);
    cache.lookup(visible, 256, 144);
    cache.set_hints(1, {{visible, ri::Priority::Visible}, {ahead, ri::Priority::Ahead},
                        {offscreen, ri::Priority::Deferred}});
    CHECK(cache.tracked() == 2);  // visible + ahead; Deferred creates nothing
    const auto starts = cache.take_starts();
    CHECK(starts.size() == 2 && starts[0].source == visible && starts[1].source == ahead);
    for (const auto& start : starts) {
        cache.complete(start.source, start.ticket, ok_image(64, 36));
    }
    const auto published = cache.take_publishes();
    CHECK(published.size() == 1 && published[0] == visible);
    const auto first = cache.lookup(ahead, 256, 144);
    CHECK(first.state == ri::State::Ready && first.image != nullptr);
    CHECK(cache.take_publishes().empty());
    CHECK(cache.stats().started == 2 && cache.stats().decoded == 2);
    // A hint-created placeholder still carries the image's aspect ratio.
    cache.set_hints(1, {{src(4), ri::Priority::Ahead}});
    CHECK(cache.take_releases().empty());  // visible/ahead were Ready: kept
    const auto made = cache.lookup(src(4), 1, 1);
    CHECK(made.placeholderWidth == 256 && made.placeholderHeight == 144);
}

// The same transformation as dusk::ui::escape (src/dusk/ui/ui.cpp), which the mod browser
// applies to the source before it goes into the decorator property. RmlUi keeps the
// result verbatim, so this output is the key the provider receives.
std::string ui_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string thumb_url(int image) {
    return "https://twilitrealm.dev/images/v1/mod" + std::to_string(image / 2) +
           (image % 2 ? "/icon.png" : "/banner.png");
}

// What the mod browser emits for a card image (CatalogCard: banner art 640, icon 128).
std::string card_source(int image) {
    return image % 2 ? ri::make_source(thumb_url(image), 256, 256, 128)
                     : ri::make_source(thumb_url(image), 1280, 720, 640);
}

void test_canonical_keys() {
    const std::string source = ri::make_source("https://twilitrealm.dev/images/v1/x/banner.png", 1280, 720, 640);
    CHECK(source == "https://twilitrealm.dev/images/v1/x/banner.png#size=1280x720,max=640");
    CHECK(source.find("&max=") == std::string::npos);
    CHECK(source.find("&amp;max=") == std::string::npos);
    CHECK(source.find('&') == std::string::npos);

    // The hint key (what set_image returns) and the key RmlUi hands the provider are the
    // same string: escaping a canonical source changes nothing.
    const std::string rmlKey = ui_escape(source);
    CHECK(rmlKey == source);
    for (int image = 0; image < 20; ++image) {
        CHECK(ui_escape(card_source(image)) == card_source(image));
    }

    // Parsed from the key RmlUi delivers: the decode limit and the placeholder survive.
    const auto info = ri::parse_source(rmlKey, 256);
    CHECK(info.maxDimension == 640);
    CHECK(info.placeholderWidth == 256 && info.placeholderHeight == 144);  // 16:9 kept
    CHECK(info.requestUrl == "https://twilitrealm.dev/images/v1/x/banner.png");
    const auto icon = ri::parse_source(ui_escape(card_source(1)), 256);
    CHECK(icon.maxDimension == 128 && icon.placeholderWidth == 256 && icon.placeholderHeight == 256);

    // The H34-c1 forms are still read, and normalize to the canonical one...
    const std::string legacy = "https://twilitrealm.dev/images/v1/x/banner.png#size=1280x720&max=640";
    const std::string legacyEscaped = ui_escape(legacy);
    CHECK(legacyEscaped.find("&amp;max=640") != std::string::npos);
    for (const auto& old : {legacy, legacyEscaped}) {
        const auto parsed = ri::parse_source(old, 256);
        CHECK(parsed.maxDimension == 640);
        CHECK(parsed.placeholderWidth == 256 && parsed.placeholderHeight == 144);
        CHECK(ri::canonical_source(old) == source);
    }
    // ...the canonical form is its own canonical form, and anything else is left alone.
    CHECK(ri::canonical_source(source) == source);
    CHECK(ri::canonical_source("https://a/b.png") == "https://a/b.png");
    CHECK(ri::canonical_source("https://a/b.png#size=axb") == "https://a/b.png#size=axb");
    CHECK(ri::canonical_source("https://a/b.png#size=1x1") == "https://a/b.png#size=1x1");
    CHECK(ri::make_source("https://a/b.png", 64, 64, 0) == "https://a/b.png#size=64x64");
}

void test_one_key_through_the_cache() {
    // Hint, lookup, publish and release all go through the one canonical key: one entry
    // per thumbnail, pinned while visible, and the release names what RmlUi loaded.
    ri::Cache cache;
    const std::string key = card_source(0);
    const std::string rmlKey = ui_escape(key);
    cache.set_hints(1, {{key, ri::Priority::Visible}});
    const auto lookup = cache.lookup(rmlKey, 256, 144);
    CHECK(lookup.state == ri::State::Unrequested);
    CHECK(cache.tracked() == 1);  // not two entries
    CHECK(cache.priority_of(rmlKey) == ri::Priority::Visible);  // pinned
    const auto starts = cache.take_starts();
    CHECK(starts.size() == 1 && starts[0].source == rmlKey && starts[0].maxDimension == 640);
    ri::FetchOutcome outcome;
    outcome.kind = ri::FetchOutcome::Kind::Ok;
    outcome.image.width = 640;
    outcome.image.height = 360;
    outcome.image.pixels.assign(640u * 360u * 4u, 1);
    outcome.decodedWidth = 1280;
    outcome.decodedHeight = 720;
    CHECK(cache.complete(starts[0].source, starts[0].ticket, std::move(outcome)) == ri::Cache::Completion::Stored);
    const auto published = cache.take_publishes();
    CHECK(published.size() == 1 && published[0] == rmlKey);  // ReleaseTexture uses RmlUi's key
    CHECK(cache.lookup(rmlKey, 256, 144).image != nullptr);
    CHECK(cache.tracked() == 1);
}

void print(const char* label, const flicker_sim::Result& r) {
    std::printf("  %-28s requested=%llu downloads=%llu evictions=%llu published=%llu hold_downloads=%llu "
                "visible_disappeared=%llu releases_unknown_to_rml=%llu max_entries_per_thumbnail=%llu "
                "thumbnails=%llu\n",
        label, static_cast<unsigned long long>(r.requested), static_cast<unsigned long long>(r.downloads),
        static_cast<unsigned long long>(r.evictions), static_cast<unsigned long long>(r.published),
        static_cast<unsigned long long>(r.holdDownloads), static_cast<unsigned long long>(r.visibleDisappeared),
        static_cast<unsigned long long>(r.releasesUnknownToRml),
        static_cast<unsigned long long>(r.maxEntriesPerThumbnail), static_cast<unsigned long long>(r.thumbnails));
}

void test_flicker_simulation() {
    std::printf("thumbnail flicker simulation (48 cards, 2 columns, 96 images, scroll down/up, 600 frames still):\n");
    // H34-c1: hints with "&max=", RmlUi with the escaped "&amp;max=".
    const auto legacyKey = [](int image) {
        std::string key = card_source(image);
        key.replace(key.find(",max="), 5, "&max=");
        return key;
    };
    const auto before = flicker_sim::run(legacyKey, [&](int image) { return ui_escape(legacyKey(image)); });
    print("H34-c1 keys (&max= / &amp;max=)", before);
    // The fix: one canonical key, unchanged by escape().
    const auto after = flicker_sim::run(card_source, [](int image) { return ui_escape(card_source(image)); });
    print("canonical keys (,max=)", after);

    // The bug reproduces with the old keys...
    CHECK(before.maxEntriesPerThumbnail == 2);
    CHECK(before.visibleDisappeared > 0);
    CHECK(before.holdDownloads > 0);
    // ...and is gone with the canonical one.
    CHECK(after.maxEntriesPerThumbnail == 1);
    CHECK(after.visibleDisappeared == 0);
    CHECK(after.holdDownloads == 0);           // a still list stops downloading
    CHECK(after.releasesUnknownToRml == 0);    // every release names a key RmlUi holds
    // 96 images do not fit 64 decoded entries, so scrolling back re-requests ones that
    // were legitimately evicted on the way down -- once each, never in a loop.
    CHECK(after.requested <= after.thumbnails + after.evictions);
    CHECK(after.downloads == after.requested);
    CHECK(after.downloads < before.downloads);
}

}  // namespace

int main() {
    test_source_format();
    test_downscale();
    test_dedup_and_priority();
    test_tickets_and_late_results();
    test_failures();
    test_retries();
    test_hits_and_publish_budget();
    test_memory_and_eviction();
    test_tracked_limit();
    test_close_with_pending_work();
    test_grid_does_not_wait_for_images();
    test_prefetch_ahead();
    test_canonical_keys();
    test_one_key_through_the_cache();
    test_flicker_simulation();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "remote_image_cache: all checks passed\n";
    return 0;
}
