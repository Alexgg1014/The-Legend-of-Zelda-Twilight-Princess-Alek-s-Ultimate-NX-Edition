#include "dusk/guide/fetch.hpp"

#include <atomic>
#include <memory>
#include <thread>

#include <fmt/format.h>

#include "dusk/guide/store.hpp"
#include "dusk/logging.h"
#include <borealis/http.hpp>
#include <chrono>
#include <borealis/app_info.hpp>

namespace dusk::guide {

// Internal: the import worker's image supplier. Was in the header, but nothing
// outside this file ever used it.
ImageSource network_image_source();

namespace {


std::string user_agent() {
    return fmt::format("Dusklight/{} (+guide-import)", BOREALIS_APP_DESCRIBE);
}





// Same shape as FetchTask: worker thread, atomic done flag, joined on take.
class ImportTask {
public:
    ImportTask() {
        mWorker = std::thread([this] {
            int n = 0;
            // Separate guards, and both report. Sharing one catch-all meant a
            // throw in the relist cancelled the import as well, and saying
            // nothing meant a silent no-op looked identical to "nothing to do".
            try {
                rebuild_index_from_disk();
            } catch (const std::exception& e) {
                DuskLog.error("guide relist failed: {}", e.what());
            } catch (...) {
                DuskLog.error("guide relist failed");
            }
            try {
                n = scan_import_folder(network_image_source());
            } catch (const std::exception& e) {
                DuskLog.error("guide import failed: {}", e.what());
                n = 0;
            } catch (...) {
                DuskLog.error("guide import failed");
                n = 0;
            }
            mCount = n;
            mDone.store(true, std::memory_order_release);
        });
    }
    ~ImportTask() {
        if (mWorker.joinable()) {
            mWorker.join();
        }
    }
    ImportTask(const ImportTask&) = delete;
    ImportTask& operator=(const ImportTask&) = delete;
    bool finished() const { return mDone.load(std::memory_order_acquire); }
    int count() const { return mCount; }

private:
    std::thread mWorker;
    std::atomic<bool> mDone{false};
    int mCount = 0;
};

std::unique_ptr<ImportTask> s_import;
unsigned s_importGen = 0;
int s_lastImportCount = 0;

// Joins and clears a finished worker, exactly once, whichever consumer asks
// first. Game thread only.
void reap_import() {
    if (s_import != nullptr && s_import->finished()) {
        s_lastImportCount = s_import->count();
        s_import.reset();  // joins
        s_importGen++;
    }
}

}  // namespace

void begin_import() {
    if (s_import == nullptr) {
        s_import = std::make_unique<ImportTask>();
    }
}

bool import_in_progress() {
    reap_import();
    return s_import != nullptr;
}

unsigned import_generation() {
    reap_import();
    return s_importGen;
}

int last_import_count() {
    return s_lastImportCount;
}

ImageSource network_image_source() {
    // aleks2: Dusklight 2.0 replaced dusk::http with borealis::http, which is
    // asynchronous (Task<Result>). The guide importer runs on its own thread
    // and wants a blocking fetch, so spin on ready() with a small sleep. On
    // Switch there is no backend yet (available() == false) and this returns
    // an empty source, so imports stay offline until the mods/network work.
    if (!borealis::http::available()) {
        return {};
    }
    return [](const std::string& url, std::string& out) {
        if (url.compare(0, 8, "https://") != 0) {
            return false;
        }
        borealis::http::Request ir{
            .url = url,
            .headers = {{.name = "User-Agent", .value = user_agent()}},
            .totalTimeout = std::chrono::milliseconds(10000),
            .maxBodyBytes = 4u * 1024u * 1024u,
        };
        auto task = borealis::http::start(std::move(ir));
        while (task && !task.ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::optional<borealis::http::Result> r;
        try {
            r = task.try_take();
        } catch (...) {
            return false;
        }
        if (!r || r->error != borealis::http::Error::None || r->response.statusCode != 200) {
            return false;
        }
        out = std::move(r->response.body);
        return true;
    };
}

}  // namespace dusk::guide
