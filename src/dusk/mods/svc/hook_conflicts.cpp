// aleks2 (H37): see hook_conflicts.hpp.
#include "hook_conflicts.hpp"

#include <mutex>

namespace dusk::mods::hooks {
namespace {

std::mutex s_mutex;
std::vector<Conflict> s_conflicts;
#if defined(__SWITCH__)
bool s_displacementAllowed = false;
#else
bool s_displacementAllowed = true;
#endif

}  // namespace

Decision decide(std::string_view target, std::string_view holder, std::string_view newcomer,
    ReplacePolicy policy, int32_t newcomerPriority, int32_t holderPriority) {
    Decision d;
    if (holder == newcomer) {
        // A mod replacing its own hook again: the SDK's policies apply as they always did.
        d.allow = policy == ReplacePolicy::Override ||
                  (policy == ReplacePolicy::Priority && newcomerPriority > holderPriority);
        return d;
    }
    const bool wouldDisplace = policy == ReplacePolicy::Override ||
                               (policy == ReplacePolicy::Priority && newcomerPriority > holderPriority);
    d.allow = wouldDisplace && displacement_allowed();
    d.record = Conflict{std::string(target), std::string(holder), std::string(newcomer), policy,
        d.allow ? Outcome::Displaced : Outcome::Refused};
    return d;
}

void set_displacement_allowed(bool allowed) {
    std::lock_guard lock(s_mutex);
    s_displacementAllowed = allowed;
}

bool displacement_allowed() {
    std::lock_guard lock(s_mutex);
    return s_displacementAllowed;
}

void record(Conflict conflict) {
    std::lock_guard lock(s_mutex);
    for (const auto& c : s_conflicts) {
        if (c.target == conflict.target && c.holder == conflict.holder && c.newcomer == conflict.newcomer) {
            return;  // the same pair on the same target, e.g. after a reload
        }
    }
    s_conflicts.push_back(std::move(conflict));
}

std::vector<Conflict> all() {
    std::lock_guard lock(s_mutex);
    return s_conflicts;
}

std::vector<Conflict> involving(std::string_view modId) {
    std::lock_guard lock(s_mutex);
    std::vector<Conflict> out;
    for (const auto& c : s_conflicts) {
        if (c.holder == modId || c.newcomer == modId) {
            out.push_back(c);
        }
    }
    return out;
}

std::string describe(const Conflict& c) {
    if (c.outcome == Outcome::Displaced) {
        return "'" + c.newcomer + "' took over '" + c.target + "' from '" + c.holder +
               "': both replace it, and only one replacement can run";
    }
    return "'" + c.newcomer + "' and '" + c.holder + "' both replace '" + c.target +
           "'; only one of them can be enabled at a time";
}

void clear() {
    std::lock_guard lock(s_mutex);
    s_conflicts.clear();
}

}  // namespace dusk::mods::hooks
