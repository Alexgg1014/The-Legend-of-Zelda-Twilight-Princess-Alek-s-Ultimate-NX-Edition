// borealis::file_select for Horizon: there is no file picker to offer.
//
// The console has a software keyboard and the album, and neither lets someone
// browse the card and point at a file. Every request therefore ends as
// "canceled", the same result the desktop gives when its dialog is dismissed, and
// capabilities() says so up front so a caller can skip asking. That covers the
// game's own "choose an ISO" step (which has its own list on this platform) and the
// picker the mod API exposes.
#include <borealis/file_select.hpp>

namespace borealis::file_select {

namespace {

void answer_canceled(const Callback& done) {
  if (done) {
    done(Result{Status::Canceled, {}, "this console has no file picker"});
  }
}

}  // namespace

Capabilities capabilities() noexcept { return {}; }

bool busy() noexcept { return false; }

void open_file(FileOptions, Callback done) { answer_canceled(done); }
void open_folder(FolderOptions, Callback done) { answer_canceled(done); }
void export_file(ExportOptions, Callback done) { answer_canceled(done); }

}  // namespace borealis::file_select
