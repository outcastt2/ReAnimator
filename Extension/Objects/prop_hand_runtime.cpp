#include "prop_hand_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Objects/local_placements_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Objects/object_placements.h"
#include "Engine/Core/Log/logging.h"
#include <algorithm>
#include <cctype>
#include <mutex>

namespace dingosdk::profile_runtime {
namespace {
struct Follow {
    std::mutex mutex;
    // Requests written by the client tick, consumed on the park tick.
    std::string want;
    bool want_arm{}, want_release{};
    // Live state, only touched on the park tick (under mutex).
    bool active{}, restore_pending{};
    std::uint64_t entity{}, token{}, saved_id{};
    profile::PlacedObject original;
    std::array<float, 3> target_position{};
    std::array<float, 4> target_rotation{0, 0, 0, 1};
    bool target_valid{};
    unsigned failures{};
    std::string status;
};
Follow &follow() {
    static auto *value = new Follow;
    return *value;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The last matching row is the most recently placed object, which is the one
// the dropper user just put down.
std::uint64_t pick_entity(const std::string &want) {
    auto &r = placements_runtime();
    PlacementRow *picked = nullptr;
    for (auto &row : r.rows) {
        if (!row.spawned || !row.entity) continue;
        if (!want.empty() && lower(row.object.item).find(want) == std::string::npos) continue;
        picked = &row;
    }
    if (!picked) return 0;
    auto &s = follow();
    s.entity = picked->entity;
    s.token = picked->token;
    s.saved_id = picked->saved_id;
    s.original = picked->object;
    // The held pose is the wrist, not the layout's; the poll that reads back
    // row poses must not queue an autosave for the hand movement. The row keeps
    // its token so the flag is restored exactly on release.
    picked->saved_id = 0;
    return s.entity;
}

bool move_one(const profile::PlacedObject &object) {
    try {
        move_network_object(follow().entity, object);
        return true;
    } catch (const std::exception &e) {
        static std::string last;
        const std::string text = e.what();
        if (text != last) {
            last = text;
            logging::log(logging::Level::warning, logging::Channel::objects,
                         "Hand props: cannot move the held object: {}.", text);
        }
        return false;
    }
}

// Put the save flag back on the followed row, wherever the row now lives: the
// entity may have been reconciled to 0 or replaced by a respawn.
void restore_saved_flag() {
    auto &s = follow();
    if (!s.saved_id || !s.token) return;
    auto &r = placements_runtime();
    for (auto &row : r.rows) {
        if (row.token != s.token) continue;
        row.saved_id = s.saved_id;
        return;
    }
}

void arm() {
    auto &s = follow();
    if (s.active) {
        // Re-targeting: the old row gets its layout flag back without a move.
        restore_saved_flag();
        s.active = false;
    }
    const auto entity = pick_entity(s.want);
    if (!entity) {
        s.status = "no placed object matches \"" + s.want + "\".";
        logging::log(logging::Level::warning, logging::Channel::objects,
                     "Hand props: no placed object matches \"{}\".", s.want);
        return;
    }
    s.active = true;
    s.restore_pending = false;
    s.failures = 0;
    s.target_valid = false;
    s.status = "following " + s.original.item;
    logging::log(logging::Level::info, logging::Channel::objects,
                 "Hand props: following \"{}\" (entity {:#x}); release with 'prop hand off'.", s.original.item,
                 entity);
}
} // namespace

void request_prop_hand(std::string substring) {
    auto &s = follow();
    std::lock_guard lock(s.mutex);
    s.want = lower(std::move(substring));
    s.want_arm = true;
    s.want_release = false;
}
void request_prop_hand_release() {
    auto &s = follow();
    std::lock_guard lock(s.mutex);
    s.want_arm = false;
    s.want_release = true;
}
void set_prop_hand_target(const std::array<float, 3> &position, const std::array<float, 4> &rotation) {
    auto &s = follow();
    std::lock_guard lock(s.mutex);
    s.target_position = position;
    s.target_rotation = rotation;
    s.target_valid = true;
}

void service_prop_hand() {
    auto &s = follow();
    std::lock_guard lock(s.mutex);
    auto &r = placements_runtime();
    if (s.want_arm) {
        s.want_arm = false;
        arm();
    }
    if (s.want_release) {
        s.want_release = false;
        if (s.active) s.restore_pending = true;
    }
    if (!s.active) return;
    if (r.clearing || park_editor_owns_placements()) return;
    if (!network_object_native_ready()) {
        static bool logged{};
        if (!logged) {
            logged = true;
            logging::log(logging::Level::warning, logging::Channel::objects,
                         "Hand props: the object-move natives are not ready; the follow is waiting.");
        }
        return;
    }
    if (s.restore_pending) {
        (void)move_one(s.original);
        restore_saved_flag();
        s.active = false;
        s.restore_pending = false;
        s.target_valid = false;
        s.status.clear();
        logging::log(logging::Level::info, logging::Channel::objects,
                     "Hand props: released; the object is back at its saved spot.");
        return;
    }
    if (!s.target_valid) return;
    profile::PlacedObject object = s.original;
    object.position = s.target_position;
    object.rotation = s.target_rotation;
    if (move_one(object)) {
        s.failures = 0;
        return;
    }
    if (++s.failures == 30) {
        restore_saved_flag();
        s.active = false;
        s.target_valid = false;
        s.status = "lost the held object; the follow stopped.";
        logging::log(logging::Level::warning, logging::Channel::objects,
                     "Hand props: the held object stopped resolving; the follow was released.");
    }
}

std::string prop_hand_status() {
    auto &s = follow();
    std::lock_guard lock(s.mutex);
    return s.status;
}
bool prop_hand_following() {
    auto &s = follow();
    std::lock_guard lock(s.mutex);
    return s.active;
}
} // namespace dingosdk::profile_runtime
