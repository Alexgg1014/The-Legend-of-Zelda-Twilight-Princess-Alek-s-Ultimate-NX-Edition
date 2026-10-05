// borealis::data for Horizon.
//
// borealis::data decides where an application keeps its files: a per-user folder
// on a desktop, a portable folder beside the executable, whatever the system
// provides on a phone. The console has one card and one convention (homebrew
// keeps its data in sdmc:/switch/<name>), so there is almost nothing to decide.
// What the interface still has to answer is answered here: the one fixed
// location, an optional override that is honoured, and "not supported" for
// everything that needs a desktop (portable mode, a file manager).
#include <borealis/data.hpp>

#include <borealis/io.hpp>

#include <string_view>
#include <system_error>

namespace borealis::data {
namespace detail {

// State behind Manager. `userOverride` is set when the user picked a folder of
// their own, which only changes the answer to "is this the default location".
class ManagerBackend {
public:
  Paths paths;
  bool userOverride = false;

  // Creates the data folder and its cache folder and records them. The first
  // folder that cannot be created is the one reported.
  Status adopt(const std::filesystem::path& root) {
    for (const auto& dir : {root, root / "cache"}) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        return failure(ErrorCode::CreateDirectoryFailed, dir, ec);
      }
    }
    paths = Paths{root, root / "cache"};
    return {};
  }

  static Status failure(ErrorCode code, std::filesystem::path path = {}, std::error_code ec = {}) {
    Status s;
    s.code = code;
    s.path = std::move(path);
    s.systemError = ec;
    return s;
  }
};

}  // namespace detail

namespace {

constexpr std::string_view kDefaultRoot = "sdmc:/switch/dusklight";
constexpr std::string_view kShippedFiles = "romfs:/res";
constexpr std::string_view kCardRoot = "sdmc:/";

}  // namespace

Manager::Manager(AppInfo, Options) : backend_{std::make_unique<detail::ManagerBackend>()} {}
Manager::~Manager() = default;
Manager::Manager(Manager&&) noexcept = default;
Manager& Manager::operator=(Manager&&) noexcept = default;

Status Manager::initialize(const std::filesystem::path& userDirectoryOverride) {
  backend_->userOverride = !userDirectoryOverride.empty();
  return backend_->adopt(backend_->userOverride ? userDirectoryOverride : std::filesystem::path{kDefaultRoot});
}

const Paths& Manager::paths() const noexcept { return backend_->paths; }

// "Configured" and "active" differ on desktops when a change waits for a restart.
// Nothing here does, so they are the same folder and nothing is ever pending.
const std::filesystem::path& Manager::active_data_path() const noexcept { return backend_->paths.userPath; }
const std::filesystem::path& Manager::configured_data_path() const noexcept { return backend_->paths.userPath; }
bool Manager::is_data_path_restart_pending() const { return false; }

LocationMode Manager::configured_mode() const noexcept {
  return backend_->userOverride ? LocationMode::Custom : LocationMode::Default;
}
bool Manager::has_user_directory_override() const noexcept { return backend_->userOverride; }
bool Manager::is_default_data_path() const { return !backend_->userOverride; }

// No portable mode and no file manager to open a folder in, so nothing is on offer.
Capabilities Manager::capabilities() const noexcept { return {}; }

Status Manager::set_custom_data_path(const std::filesystem::path& path) {
  if (path.empty()) {
    return detail::ManagerBackend::failure(ErrorCode::EmptyPath);
  }
  backend_->userOverride = true;
  return backend_->adopt(path);
}

Status Manager::reset_data_path() {
  backend_->userOverride = false;
  return backend_->adopt(std::filesystem::path{kDefaultRoot});
}

// Portable would mean "beside the executable", and an NRO runs from wherever the
// launcher found it.
Status Manager::set_portable_data_path() { return detail::ManagerBackend::failure(ErrorCode::Unsupported); }
Status Manager::open_active_data_path() const { return detail::ManagerBackend::failure(ErrorCode::Unsupported); }
Status Manager::open_folder(const std::filesystem::path&) const {
  return detail::ManagerBackend::failure(ErrorCode::Unsupported);
}

// What ships with the program is in the NRO's romfs.
std::filesystem::path Manager::base_path_relative(const std::filesystem::path& path) const {
  return std::filesystem::path{kShippedFiles} / path;
}

std::filesystem::path Manager::user_home_path() const { return std::filesystem::path{kCardRoot}; }

std::filesystem::path Manager::normalized_display_path(const std::filesystem::path& path) const { return path; }

// For the log: a path inside the data folder is printed relative to it, anything
// else in full.
std::string Manager::abbreviated_path_string(const std::filesystem::path& path) const {
  const std::string full = io::fs_path_to_string(path);
  const std::string root = io::fs_path_to_string(backend_->paths.userPath);
  const bool inside = !root.empty() && full.size() > root.size() + 1 && full.starts_with(root) &&
                      (full[root.size()] == '/' || full[root.size()] == '\\');
  return inside ? full.substr(root.size() + 1) : full;
}

}  // namespace borealis::data
