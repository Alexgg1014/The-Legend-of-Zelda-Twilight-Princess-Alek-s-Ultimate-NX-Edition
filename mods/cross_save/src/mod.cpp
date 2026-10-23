// Cloud Save (cross-save) mod, lab build. One package for PC, Android and Switch (Switch runs the
// linux-aarch64 library through the native mod bridge). The sync logic lives in core/ (provider
// neutral, host-tested), Firebase in firebase/, the SDK glue in sdk_adapters.cpp; this file wires
// them to the game: the save observer, the frame tick and the panel in the Mods window.

#include "../core/sync_engine.hpp"
#include "../firebase/firebase_backend.hpp"
#include "platform_io.hpp"
#include "file_local_store.hpp"
#include "sdk_adapters.hpp"

#include <mods/service.hpp>
#include <mods/svc/config.h>
#include <mods/svc/host.h>
#include <mods/svc/http.h>
#include <mods/svc/log.h>
#include <mods/svc/save.h>
#include <mods/svc/save_snapshot.h>
#include <mods/svc/ui.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

DEFINE_MOD();
// The lowest minor versions this mod really uses, so official Dusklight releases load it.
IMPORT_SERVICE_VERSION(HostService, svc_host, 2);  // data_dir
IMPORT_SERVICE_VERSION(LogService, svc_log, 0);
IMPORT_SERVICE_VERSION(ConfigService, svc_config, 0);
IMPORT_SERVICE_VERSION(UiService, svc_ui, 0);
IMPORT_SERVICE_VERSION(SaveService, svc_save, 0);
IMPORT_SERVICE_VERSION(HttpService, svc_http, 0);
// Optional: builds without it (the official PC and Android releases) use the compatibility mode,
// which works on the memory card folder directly (file_local_store.hpp).
IMPORT_OPTIONAL_SERVICE_VERSION(SaveSnapshotService, svc_save_snapshot, 0);

using namespace crosssave;

namespace {

constexpr int64_t kPairingTtlMs = 10 * 60 * 1000;
constexpr int64_t kPairingPollMs = 5000;

struct State {
    std::string dataDir;
    std::string platform;
    std::unique_ptr<io::Random> random;
    std::unique_ptr<LocalStore> local;
    compat::FileLocalStore* compatStore = nullptr;  // set in compatibility mode
    std::string startupMessage;
    std::unique_ptr<sdk::HttpServiceTransport> http;
    std::unique_ptr<sdk::FileSessionStore> sessions;
    std::unique_ptr<firebase::FirebaseBackend> backend;
    std::unique_ptr<SyncEngine> engine;

    ConfigVarHandle syncEnabled = 0;
    ConfigVarHandle automatic = 0;
    ConfigVarHandle deviceName = 0;

    // Sign-in form. The password lives here only until the request is sent.
    std::string email;
    std::string password;
    std::string approveCode;
    std::string authMessage;
    bool authBusy = false;
    int64_t nextPairingPoll = 0;
    bool pairingDialogShown = false;
    Phase lastPhase = Phase::Disabled;
    ConflictKind shownConflict = ConflictKind::None;

    UiElementHandle statusText = 0;
    UiElementHandle detailText = 0;
    UiElementHandle accountText = 0;
};

State* g = nullptr;

void log(LogLevel level, const std::string& message) {
    svc_log->write(mod_ctx, level, message.c_str());
}

bool config_bool(ConfigVarHandle var, bool fallback) {
    bool value = fallback;
    svc_config->get_bool(mod_ctx, var, &value);
    return value;
}

std::string device_name() {
    char buffer[128] = {};
    if (svc_config->get_string(mod_ctx, g->deviceName, buffer, sizeof(buffer), nullptr) == MOD_OK &&
        buffer[0] != '\0')
    {
        return buffer;
    }
    return g->platform;
}

// Device names and labels come from other devices: never let them become markup.
std::string rml(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '&':
            out += "&amp;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

std::string format_wall(int64_t ms) {
    if (ms <= 0) {
        return "never";
    }
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &seconds);
#else
    gmtime_r(&seconds, &value);
#endif
    char buffer[40];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &value);
    return buffer;
}

std::string format_gc_time(uint32_t value) {
    // GameCube time: seconds since 2000-01-01.
    return value == 0 ? std::string{"unknown"} : format_wall((946684800LL + value) * 1000);
}

void show_toast(const std::string& title, const std::string& body) {
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    const auto safeBody = rml(body);
    toast.title_rml = title.c_str();
    toast.body_rml = safeBody.c_str();
    svc_ui->push_toast(mod_ctx, &toast);
}

// ---- conflict dialog ----------------------------------------------------------------------------

void resolve_local(ModContext*, UiDialogHandle, void*) {
    g->engine->resolve(Choice::KeepLocal, io::monotonic_ms());
}
void resolve_remote(ModContext*, UiDialogHandle, void*) {
    g->engine->resolve(Choice::KeepRemote, io::monotonic_ms());
}
void resolve_cancel(ModContext*, UiDialogHandle, void*) {
    g->engine->resolve(Choice::Cancel, io::monotonic_ms());
}

std::string conflict_body() {
    const auto& status = g->engine->status();
    const auto& conflict = status.conflict;
    std::string body = rml(status.message) + "<br/><br/>";
    if (conflict.hasLocal) {
        body += "This device: saved " + format_gc_time(conflict.localModifiedTime) + " (" +
                rml(conflict.localGame) + ")<br/>";
    } else {
        body += "This device: no save<br/>";
    }
    if (conflict.remote) {
        body += "Cloud: revision " + std::to_string(conflict.remote->revision) + " from " +
                rml(conflict.remote->deviceName.empty() ? std::string{"?"} : conflict.remote->deviceName) +
                ", uploaded " + format_wall(conflict.remote->createdAtMs) + " (" + rml(conflict.remote->game) +
                ")<br/>";
    } else {
        body += "Cloud: no copy<br/>";
    }
    body += "<br/>Whatever you choose, the other copy is kept: a local backup in the Saves window, "
            "or an older revision in the cloud.";
    return body;
}

void show_conflict_dialog() {
    const auto kind = g->engine->status().conflict.kind;
    std::vector<UiDialogAction> actions;
    UiDialogAction keepLocal = UI_DIALOG_ACTION_INIT;
    keepLocal.label = kind == ConflictKind::RemoteMissing ? "Upload this device's save" : "Keep this device's save";
    keepLocal.on_pressed = resolve_local;
    UiDialogAction keepRemote = UI_DIALOG_ACTION_INIT;
    keepRemote.label = kind == ConflictKind::NeedsConversion ? "Convert and use the cloud save" : "Use the cloud save";
    keepRemote.on_pressed = resolve_remote;
    UiDialogAction later = UI_DIALOG_ACTION_INIT;
    later.label = "Decide later";
    later.on_pressed = resolve_cancel;
    if (kind != ConflictKind::LocalMissing) {
        actions.push_back(keepLocal);
    }
    if (kind != ConflictKind::RemoteMissing) {
        actions.push_back(keepRemote);
    }
    actions.push_back(later);
    const auto body = conflict_body();
    UiDialogDesc dialog = UI_DIALOG_DESC_INIT;
    dialog.title = "Cloud save conflict";
    dialog.body_rml = body.c_str();
    dialog.variant = UI_DIALOG_WARNING;
    dialog.actions = actions.data();
    dialog.action_count = actions.size();
    dialog.on_dismiss = resolve_cancel;
    svc_ui->dialog_push(mod_ctx, &dialog, nullptr);
}

// ---- account actions ----------------------------------------------------------------------------

void after_auth(const firebase::AuthResult& result, const std::string& success) {
    g->authBusy = false;
    g->authMessage = result.error == RemoteError::None ? success : result.message;
    g->engine->onAccountChanged(io::monotonic_ms());
}

void sign_in(ModContext*, void*) {
    if (g->authBusy || g->email.empty() || g->password.empty()) {
        return;
    }
    g->authBusy = true;
    g->authMessage = "Signing in...";
    const auto password = std::move(g->password);
    g->password.clear();
    g->backend->signInWithPassword(g->email, password, [](firebase::AuthResult result) {
        after_auth(result, "Signed in.");
    });
}

void confirm_link(ModContext*, UiDialogHandle, void*) {
    g->backend->confirmPairing([](firebase::AuthResult result) { after_auth(result, "This device is linked."); });
}

void reject_link(ModContext*, UiDialogHandle, void*) {
    g->backend->cancelPairing();
    g->authMessage = "Link cancelled.";
    g->pairingDialogShown = false;
}

void start_link(ModContext*, void*) {
    if (g->authBusy) {
        return;
    }
    g->authBusy = true;
    g->authMessage = "Requesting a link code...";
    g->backend->startPairing(device_name(), g->platform, kPairingTtlMs,
        [](firebase::AuthResult result, std::string code) {
            g->authBusy = false;
            if (result.error != RemoteError::None) {
                g->authMessage = result.message;
                return;
            }
            g->authMessage = "On a device signed in to your account, open Cloud Save and approve code " +
                             code.substr(0, 4) + "-" + code.substr(4) + " (valid 10 minutes).";
            g->nextPairingPoll = io::monotonic_ms() + kPairingPollMs;
            g->pairingDialogShown = false;
        });
}

void poll_pairing(int64_t now) {
    if (g->backend->pairingCode().empty() || g->authBusy || now < g->nextPairingPoll || g->pairingDialogShown) {
        return;
    }
    g->nextPairingPoll = now + kPairingPollMs;
    g->authBusy = true;
    g->backend->pollPairing([](firebase::PairingStatus status) {
        g->authBusy = false;
        if (status.expired) {
            g->backend->cancelPairing();
            g->authMessage = "The link code expired. Request a new one.";
            return;
        }
        if (!status.approved) {
            return;
        }
        // The player must recognize the account before this device syncs with it: a code someone
        // else approved shows their label here.
        g->pairingDialogShown = true;
        const std::string body = "Link this device to the cloud saves of " + rml(status.ownerLabel) + "?";
        UiDialogAction yes = UI_DIALOG_ACTION_INIT;
        yes.label = "Link";
        yes.on_pressed = confirm_link;
        UiDialogAction no = UI_DIALOG_ACTION_INIT;
        no.label = "That is not my account";
        no.on_pressed = reject_link;
        UiDialogAction actions[] = {yes, no};
        UiDialogDesc dialog = UI_DIALOG_DESC_INIT;
        dialog.title = "Confirm account";
        dialog.body_rml = body.c_str();
        dialog.actions = actions;
        dialog.action_count = 2;
        dialog.on_dismiss = reject_link;
        svc_ui->dialog_push(mod_ctx, &dialog, nullptr);
    });
}

void approve_code(ModContext*, void*) {
    if (g->authBusy || g->approveCode.empty()) {
        return;
    }
    g->authBusy = true;
    g->authMessage = "Approving...";
    g->backend->approvePairing(g->approveCode, [](firebase::AuthResult result) {
        g->authBusy = false;
        g->approveCode.clear();
        g->authMessage = result.error == RemoteError::None ?
                             "Approved. Confirm on the other device." :
                             result.message;
    });
}

std::vector<firebase::LinkedDevice> g_devices;

void unlink_device(ModContext*, UiDialogHandle, void* userData) {
    const auto index = reinterpret_cast<uintptr_t>(userData);
    if (index >= g_devices.size()) {
        return;
    }
    g->backend->revokeDevice(g_devices[index].uid, [](firebase::AuthResult result) {
        g->authMessage = result.error == RemoteError::None ? "Device unlinked." : result.message;
    });
}

void close_dialog(ModContext*, UiDialogHandle, void*) {}

void manage_devices(ModContext*, void*) {
    g->backend->listDevices([](RemoteError error, std::vector<firebase::LinkedDevice> devices, std::string message) {
        if (error != RemoteError::None) {
            g->authMessage = "Could not list devices (" + message + ").";
            return;
        }
        g_devices = std::move(devices);
        static std::vector<std::string> labels;
        labels.clear();
        std::vector<UiDialogAction> actions;
        for (size_t i = 0; i < g_devices.size() && i < 8; ++i) {
            labels.push_back("Unlink " + (g_devices[i].name.empty() ? g_devices[i].uid : g_devices[i].name) +
                             " (" + g_devices[i].platform + ")");
        }
        for (size_t i = 0; i < labels.size(); ++i) {
            UiDialogAction action = UI_DIALOG_ACTION_INIT;
            action.label = labels[i].c_str();
            action.on_pressed = unlink_device;
            action.user_data = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
            actions.push_back(action);
        }
        UiDialogAction close = UI_DIALOG_ACTION_INIT;
        close.label = "Close";
        close.on_pressed = close_dialog;
        actions.push_back(close);
        const std::string body = g_devices.empty() ?
                                     std::string{"No other device is linked to this account."} :
                                     "Devices linked with a code. Unlinking one stops it syncing at once; "
                                     "its saves stay on it.";
        UiDialogDesc dialog = UI_DIALOG_DESC_INIT;
        dialog.title = "Linked devices";
        dialog.body_rml = body.c_str();
        dialog.actions = actions.data();
        dialog.action_count = actions.size();
        svc_ui->dialog_push(mod_ctx, &dialog, nullptr);
    });
}

void sign_out(ModContext*, void*) {
    g->backend->cancelPairing();
    g->backend->signOut();
    g->authMessage = "Signed out on this device. Your saves stay on it and in the cloud.";
    g->engine->onAccountChanged(io::monotonic_ms());
}

void delete_confirmed(ModContext*, UiDialogHandle, void*) {
    g->engine->deleteRemote(io::monotonic_ms());
}

void delete_cloud(ModContext*, void*) {
    UiDialogAction yes = UI_DIALOG_ACTION_INIT;
    yes.label = "Delete cloud copies";
    yes.on_pressed = delete_confirmed;
    UiDialogAction no = UI_DIALOG_ACTION_INIT;
    no.label = "Cancel";
    no.on_pressed = close_dialog;
    UiDialogAction actions[] = {yes, no};
    UiDialogDesc dialog = UI_DIALOG_DESC_INIT;
    dialog.title = "Delete cloud copies?";
    dialog.body_rml = "Every cloud copy of this save is deleted from the account. Saves on your devices "
                      "are not touched. Cloud sync is turned off on this device afterwards.";
    dialog.variant = UI_DIALOG_DANGER;
    dialog.actions = actions;
    dialog.action_count = 2;
    svc_ui->dialog_push(mod_ctx, &dialog, nullptr);
}

void sync_now(ModContext*, void*) {
    g->engine->syncNow(io::monotonic_ms());
}

void open_conflict(ModContext*, void*) {
    if (g->engine->status().phase == Phase::Conflict) {
        show_conflict_dialog();
    }
}

// ---- predicates and string bindings -------------------------------------------------------------

bool not_configured() {
    return !g->backend->config().valid();
}
bool signed_in_owner() {
    const auto& session = g->backend->session();
    return session && !session->anonymous && !session->ownerUid.empty();
}
bool disabled_sign_in(ModContext*, void*) {
    return not_configured() || g->authBusy || g->backend->signedIn();
}
bool disabled_signed_in(ModContext*, void*) {
    return not_configured() || g->authBusy || !g->backend->signedIn();
}
bool disabled_owner_only(ModContext*, void*) {
    return not_configured() || g->authBusy || !signed_in_owner();
}
bool disabled_sync(ModContext*, void*) {
    return !g->backend->signedIn() || !g->engine->enabled();
}
bool disabled_conflict(ModContext*, void*) {
    return g->engine->status().phase != Phase::Conflict;
}

void get_email(ModContext*, void*, UiControlValue* out) {
    out->string_value = g->email.c_str();
}
void set_email(ModContext*, void*, const UiControlValue* value) {
    g->email = value->string_value != nullptr ? value->string_value : "";
}
void get_password(ModContext*, void*, UiControlValue* out) {
    // Never echo it back.
    out->string_value = "";
}
void set_password(ModContext*, void*, const UiControlValue* value) {
    g->password = value->string_value != nullptr ? value->string_value : "";
}
void get_code(ModContext*, void*, UiControlValue* out) {
    out->string_value = g->approveCode.c_str();
}
void set_code(ModContext*, void*, const UiControlValue* value) {
    g->approveCode = value->string_value != nullptr ? value->string_value : "";
}

void on_config_changed(ModContext*, ConfigVarHandle var, const ConfigVarValue* value, const ConfigVarValue*, void*) {
    const int64_t now = io::monotonic_ms();
    if (var == g->syncEnabled) {
        g->engine->setEnabled(value->bool_value, now);
    } else if (var == g->automatic) {
        g->engine->setAutomatic(value->bool_value);
    }
}

// ---- panel --------------------------------------------------------------------------------------

UiElementHandle add_button(UiElementHandle pane, const char* label, UiPressedFn pressed,
    UiPredicateFn disabled, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = label;
    control.help_rml = help;
    control.on_pressed = pressed;
    control.is_disabled = disabled;
    UiElementHandle handle = 0;
    svc_ui->pane_add_control(mod_ctx, pane, &control, &handle);
    return handle;
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Cloud Save (lab build: not tested on console)");
    svc_ui->pane_add_text(mod_ctx, panel, "", &g->statusText);
    svc_ui->pane_add_text(mod_ctx, panel, "", &g->detailText);

    UiControlDesc enabled = UI_CONTROL_DESC_INIT;
    enabled.kind = UI_CONTROL_TOGGLE;
    enabled.label = "Cloud sync";
    enabled.help_rml = "Off until you link an account. Turning it off keeps every save where it is.";
    enabled.binding = UI_BINDING_CONFIG_VAR;
    enabled.config_var = g->syncEnabled;
    svc_ui->pane_add_control(mod_ctx, panel, &enabled, nullptr);

    UiControlDesc automatic = UI_CONTROL_DESC_INIT;
    automatic.kind = UI_CONTROL_TOGGLE;
    automatic.label = "Sync automatically";
    automatic.help_rml = "Upload a few seconds after each save and check the cloud when the game starts. "
                         "Off: only Sync now.";
    automatic.binding = UI_BINDING_CONFIG_VAR;
    automatic.config_var = g->automatic;
    svc_ui->pane_add_control(mod_ctx, panel, &automatic, nullptr);

    add_button(panel, "Sync now", sync_now, disabled_sync, "Check the cloud copy and sync this save now.");
    add_button(panel, "Resolve conflict...", open_conflict, disabled_conflict,
        "Choose which copy to keep. The other one is kept as a backup.");

    svc_ui->pane_add_section(mod_ctx, panel, "Account");
    svc_ui->pane_add_text(mod_ctx, panel, "", &g->accountText);

    UiControlDesc email = UI_CONTROL_DESC_INIT;
    email.kind = UI_CONTROL_STRING;
    email.label = "Email";
    email.get = get_email;
    email.set = set_email;
    email.is_disabled = disabled_sign_in;
    email.max_length = 128;
    svc_ui->pane_add_control(mod_ctx, panel, &email, nullptr);

    UiControlDesc password = UI_CONTROL_DESC_INIT;
    password.kind = UI_CONTROL_STRING;
    password.label = "Password";
    password.help_rml = "Sent once to Firebase to sign in; never stored. The field is not masked.";
    password.get = get_password;
    password.set = set_password;
    password.is_disabled = disabled_sign_in;
    password.max_length = 128;
    svc_ui->pane_add_control(mod_ctx, panel, &password, nullptr);

    add_button(panel, "Sign in", sign_in, disabled_sign_in,
        "Sign in with the account's email and password. The account is created in the Firebase console.");
    add_button(panel, "Link with a code instead", start_link, disabled_sign_in,
        "Shows a code. Approve it from a device already signed in; no password is typed here.");

    UiControlDesc code = UI_CONTROL_DESC_INIT;
    code.kind = UI_CONTROL_STRING;
    code.label = "Code from another device";
    code.get = get_code;
    code.set = set_code;
    code.is_disabled = disabled_owner_only;
    code.max_length = 16;
    svc_ui->pane_add_control(mod_ctx, panel, &code, nullptr);
    add_button(panel, "Approve code", approve_code, disabled_owner_only,
        "Lets the device showing this code sync this account's saves.");
    add_button(panel, "Linked devices...", manage_devices, disabled_owner_only,
        "See and unlink devices that were linked with a code.");
    add_button(panel, "Sign out on this device", sign_out, disabled_signed_in, nullptr);
    add_button(panel, "Delete cloud copies...", delete_cloud, disabled_sync,
        "Delete this save from the cloud (devices keep theirs).");
    return MOD_OK;
}

ModResult update_panel(ModContext*, void*, ModError*) {
    const auto& status = g->engine->status();
    std::string line = std::string{phase_name(status.phase)} + ": " + status.message;
    if (status.phase == Phase::Retrying && status.nextAttemptAtMs > 0) {
        const auto seconds = (status.nextAttemptAtMs - io::monotonic_ms()) / 1000;
        line += " Next try in " + std::to_string(seconds > 0 ? seconds : 0) + " s.";
    }
    svc_ui->elem_set_text(mod_ctx, g->statusText, line.c_str());
    std::string detail = "Last sync: " + format_wall(status.lastSyncWallMs);
    if (status.remoteRevision > 0) {
        detail += " | Cloud: revision " + std::to_string(status.remoteRevision) + " from " +
                  (status.remoteDevice.empty() ? std::string{"?"} : status.remoteDevice);
    }
    if (status.localChangesPending) {
        detail += " | Local changes pending";
    }
    if (!status.lastBackup.empty()) {
        detail += " | Last backup: " + status.lastBackup;
    }
    if (status.restartRequired) {
        detail += " | Restart the game before playing";
    }
    svc_ui->elem_set_text(mod_ctx, g->detailText, detail.c_str());
    std::string account;
    if (not_configured()) {
        account = "Firebase project not configured. Put firebase.json ({\"apiKey\", \"projectId\"}) in " +
                  g->dataDir + ".";
    } else if (const auto& session = g->backend->session(); session && !session->ownerUid.empty()) {
        account = (session->anonymous ? "Linked to " : "Signed in as ") +
                  (session->label.empty() ? std::string{"an account"} : session->label) + " on " + device_name() + ".";
    } else {
        account = "Not linked.";
    }
    if (!g->authMessage.empty()) {
        account += " " + g->authMessage;
    }
    svc_ui->elem_set_text(mod_ctx, g->accountText, account.c_str());
    return MOD_OK;
}

// ---- save events --------------------------------------------------------------------------------

void on_save_written(ModContext*, uint32_t, void*) {
    if (g->compatStore != nullptr) {
        ++g->compatStore->changeCount;
    }
    g->engine->onSaveWritten(io::monotonic_ms());
}

}  // namespace

MOD_EXPORT ModResult mod_initialize(ModError* outError) {
    const char* dataDir = nullptr;
    if (svc_host->data_dir(mod_ctx, &dataDir) != MOD_OK || dataDir == nullptr) {
        return mods::set_error(outError, MOD_UNAVAILABLE, "Cloud Save needs a data directory.");
    }
    g = new State{};
    g->dataDir = dataDir;
    g->platform = io::platform_name(g->dataDir);
    g->random = std::make_unique<io::Random>(io::join(g->dataDir, "seed.bin"));

    ConfigVarDesc enabled = CONFIG_VAR_DESC_INIT;
    enabled.name = "syncEnabled";
    enabled.type = CONFIG_VAR_BOOL;
    enabled.default_bool = false;
    svc_config->register_var(mod_ctx, &enabled, &g->syncEnabled);
    ConfigVarDesc automatic = CONFIG_VAR_DESC_INIT;
    automatic.name = "automatic";
    automatic.type = CONFIG_VAR_BOOL;
    automatic.default_bool = true;
    svc_config->register_var(mod_ctx, &automatic, &g->automatic);
    ConfigVarDesc name = CONFIG_VAR_DESC_INIT;
    name.name = "deviceName";
    name.type = CONFIG_VAR_STRING;
    name.default_string = "";
    svc_config->register_var(mod_ctx, &name, &g->deviceName);
    svc_config->subscribe(mod_ctx, g->syncEnabled, on_config_changed, nullptr, nullptr);
    svc_config->subscribe(mod_ctx, g->automatic, on_config_changed, nullptr, nullptr);

    if (svc_save_snapshot != nullptr) {
        g->local = std::make_unique<sdk::SnapshotLocalStore>(g->dataDir);
    } else {
        auto store = std::make_unique<compat::FileLocalStore>(g->dataDir);
        // Mods initialize before the game mounts the memory card: the only safe moment to replace
        // the save in a build that cannot remount it.
        g->startupMessage = store->applyStaged();
        g->compatStore = store.get();
        g->local = std::move(store);
    }
    g->http = std::make_unique<sdk::HttpServiceTransport>();
    g->sessions = std::make_unique<sdk::FileSessionStore>(g->dataDir);
    g->backend = std::make_unique<firebase::FirebaseBackend>(*g->http, *g->sessions,
        sdk::load_firebase_config(g->dataDir), [] { return io::monotonic_ms(); }, [] { return io::wall_ms(); },
        [] { return g->random->next(); });

    // The device ID only tells this device's own uploads apart (lost-answer recovery); it is
    // random, persisted, and not tied to hardware.
    std::string deviceId;
    if (const auto stored = io::read_file(io::join(g->dataDir, "device-id"), 64); stored && stored->size() == 32) {
        deviceId.assign(stored->begin(), stored->end());
    } else {
        char buffer[33];
        std::snprintf(buffer, sizeof(buffer), "%016llx%016llx", static_cast<unsigned long long>(g->random->next()),
            static_cast<unsigned long long>(g->random->next()));
        deviceId = buffer;
        io::write_file_atomic(io::join(g->dataDir, "device-id"), deviceId.data(), deviceId.size());
    }
    EngineConfig config;
    config.deviceId = deviceId;
    config.deviceName = device_name();
    config.platform = g->platform;
    config.wallClockMs = [] { return io::wall_ms(); };
    config.random = [] { return g->random->next(); };
    g->engine = std::make_unique<SyncEngine>(*g->local, *g->backend, config);
    g->engine->setAutomatic(config_bool(g->automatic, true));
    const int64_t now = io::monotonic_ms();
    g->engine->setEnabled(config_bool(g->syncEnabled, false), now);
    g->engine->onSessionStart(now);

    svc_save->observe_saves(mod_ctx, nullptr, nullptr, on_save_written, nullptr, nullptr);

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    svc_ui->register_mods_panel(mod_ctx, &panel);
    log(LOG_LEVEL_INFO, "cloud save ready on " + g->platform +
                            (g->compatStore != nullptr ? " (compatibility mode: no save_snapshot service)" : "") +
                            (g->backend->config().valid() ? "" : " (no Firebase project configured)"));
    if (!g->startupMessage.empty()) {
        log(LOG_LEVEL_INFO, g->startupMessage);
        g->authMessage = g->startupMessage;
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    const int64_t now = io::monotonic_ms();
    g->engine->tick(now);
    poll_pairing(now);
    const auto& status = g->engine->status();
    if (status.phase == Phase::Conflict && g->shownConflict != status.conflict.kind) {
        g->shownConflict = status.conflict.kind;
        show_conflict_dialog();
    } else if (status.phase != Phase::Conflict) {
        g->shownConflict = ConflictKind::None;
    }
    if (status.phase != g->lastPhase) {
        if (status.phase == Phase::Idle && g->lastPhase == Phase::Working && status.message.rfind("Downloaded", 0) == 0) {
            show_toast("Cloud Save", status.message);
        } else if (status.phase == Phase::WaitingForMenu) {
            show_toast("Cloud Save", status.message);
        }
        if (status.phase == Phase::Error || status.phase == Phase::SignedOut) {
            log(LOG_LEVEL_WARN, std::string{"sync stopped: "} + status.message);
        }
        g->lastPhase = status.phase;
    }
    // Keep the config toggle in step when the engine turned itself off (cloud copies deleted).
    if (!g->engine->enabled() && config_bool(g->syncEnabled, false)) {
        svc_config->set_bool(mod_ctx, g->syncEnabled, false);
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    // In-flight HTTP callbacks do not run once the mod deactivates; their small state leaks.
    delete g;
    g = nullptr;
    return MOD_OK;
}
