#include "effect_attach.h"
#include "Extension/Multiplayer/Remote/native_skater_internal.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/skater_entities.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

// Prototype: spawn a native EffectBlueprint as an entity and re-place it on a
// skater joint every frame. See SkateAnimationStudio/README.md 10.7.
namespace dingosdk::skater {
namespace {
namespace nsd = multiplayer::native_skater_detail;
namespace entities = addr::skater_entities;
using nsd::ptr;
using nsd::read;
using nsd::readable;
using nsd::require;

constexpr std::uint16_t head_joint = 103;
constexpr std::array<std::uint16_t, 10> head_chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};
constexpr std::size_t skater_joint_bound = 512;

struct Request {
    std::mutex mutex;
    std::string blueprint;
    float offset{};
    bool attach{};
    bool detach{};
};
struct Attached {
    bool active{};
    std::uintptr_t base{}, context{}, entity{}, parent{};
    std::string blueprint, status;
};
Request &request() { static Request r; return r; }
Attached &attached() { static Attached a; return a; }
std::mutex &status_mutex() { static std::mutex m; return m; }
void set_status(const std::string &text) {
    std::lock_guard lock(status_mutex());
    attached().status = text;
}
void reset(Attached &a) {
    a.active = false;
    a.base = a.context = a.entity = a.parent = 0;
    a.blueprint.clear();
}

struct WorldJoint {
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> position{};
    float scale{1};
};
std::array<float, 3> rotate(const std::array<float, 4> &q, const std::array<float, 3> &v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
                tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz),
            v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
WorldJoint child(const WorldJoint &parent, std::uintptr_t buffer, std::uint16_t index) {
    std::array<float, 12> bone{};
    require(readable(buffer + index * 0x30ULL, bone.data(), sizeof(bone)), "Pose joint is unreadable.");
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        require(std::isfinite(bone[i]), "Pose joint is not ready.");
    const auto &q = parent.rotation;
    const auto offset = rotate(q, {bone[8] * parent.scale, bone[9] * parent.scale, bone[10] * parent.scale});
    WorldJoint out;
    for (std::size_t i = 0; i < 3; ++i) out.position[i] = parent.position[i] + offset[i];
    out.rotation = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5],
                    q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
                    q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7],
                    q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    out.scale = parent.scale * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return out;
}

struct LocalSkater {
    std::uintptr_t context{}, entity{}, parent{}, holder{};
};
LocalSkater find_local(std::uintptr_t base, std::uintptr_t client) {
    require(ptr(client) == base + addr::engine::client_vtable, "Local client is unavailable.");
    LocalSkater local;
    local.context = ptr(client, 8);
    const auto offset = read<std::uint32_t>(base, addr::engine::context_player_manager_offset);
    require(offset <= 0x1000000, "Player manager offset changed.");
    const auto manager = ptr(local.context, offset);
    require(ptr(manager) == base + addr::engine::local_player_manager_vtable, "Local player manager is unavailable.");
    const auto begin = ptr(manager, 0x4c8), end = ptr(manager, 0x4d0);
    require(end >= begin && end - begin == 8, "Expected exactly one local player.");
    const auto player = ptr(begin);
    require(ptr(player) == base + addr::engine::local_player_vtable, "Local player is unavailable.");
    local.entity = ptr(player, 0xb8);
    require(local.entity && ptr(local.entity) == base + addr::engine::skater_entity_vtable &&
                ptr(local.entity, 0x20) == local.context,
            "Local skater is not spawned.");
    local.parent = ptr(local.entity, 0x40);
    const auto component = ptr(local.entity, 0x628);
    require(ptr(component) == base + addr::engine::skater_component_vtable, "Skater animation component is unavailable.");
    local.holder = ptr(component, 0xa0);
    return local;
}

WorldJoint head_world(std::uintptr_t base, std::uintptr_t holder) {
    const auto reader = [](std::uintptr_t address, void *out, std::size_t size) {
        return readable(address, out, size);
    };
    const auto pose = multiplayer::read_native_pose_layout(reader, base, holder, skater_joint_bound);
    require(pose.buffer && pose.count > head_joint, "The standard skater skeleton is not ready.");
    WorldJoint joint;
    for (const auto index : head_chain) joint = child(joint, pose.buffer, index);
    return joint;
}

bool derives(std::uintptr_t object, std::uintptr_t expected) {
    auto type = ptr(object, 8);
    for (int depth = 0; depth < 16 && type; ++depth) {
        if (type == expected) return true;
        type = ptr(type, 0x20);
    }
    return false;
}

// find_asset only answers for loaded assets. The scan is wrapped in SEH: a
// domain whose registry is not in a queryable state must not take the game down.
std::uintptr_t scan_domains(std::uintptr_t base, const char *name, bool &faulted) noexcept {
    __try {
        const auto find = game::native_data().find_asset;
        if (!find) return 0;
        for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
            std::uintptr_t owner{};
            if (!memory::read_bytes(base + addr::engine::domain_owners + domain * 8ULL, &owner, 8) || !owner) continue;
            if (const auto asset = find(domain, name)) return asset;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return 0;
    }
}

std::array<float, 16> matrix_at(const WorldJoint &joint, float up) {
    multiplayer::Transform t;
    t.position = {joint.position[0], joint.position[1] + up, joint.position[2]};
    t.rotation = joint.rotation;
    return multiplayer::to_matrix(t);
}

// The descriptor and the two native calls are guarded with SEH: creating an
// entity from an asset the engine does not consider an entity blueprint must
// fail cleanly, not crash.
std::uintptr_t create_guarded(std::uintptr_t base, std::uintptr_t parent, std::uintptr_t blueprint,
                              const void *matrix, bool &faulted) noexcept {
    __try {
        alignas(16) std::array<std::uint8_t, 0x190> descriptor{};
        using Init = void *(*)(void *, std::uintptr_t, std::uintptr_t, const void *);
        reinterpret_cast<Init>(base + entities::descriptor_init)(descriptor.data(), 0, parent, matrix);
        const std::uint32_t id = 255;
        std::memcpy(descriptor.data() + 0x38, &id, sizeof(id));
        descriptor[0x151] = 0;
        const std::uintptr_t creation_list = 0;
        std::memcpy(descriptor.data() + 0x158, &creation_list, sizeof(creation_list));
        std::array<std::uintptr_t, 3> result{};
        using Create = void *(*)(void *, void *, std::uintptr_t, std::uintptr_t, std::uintptr_t);
        reinterpret_cast<Create>(base + entities::create_entity)(result.data(), descriptor.data(), blueprint, 0, 0);
        reinterpret_cast<void (*)(void *)>(base + entities::descriptor_destroy)(descriptor.data() + 0x10);
        if (result[1]) reinterpret_cast<void (*)(std::uintptr_t)>(base + entities::release_reference)(result[1]);
        return result[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return 0;
    }
}
bool initialize_guarded(std::uintptr_t base, std::uintptr_t entity, const void *matrix) noexcept {
    __try {
        reinterpret_cast<void (*)(std::uintptr_t, const void *, std::uintptr_t, std::uint8_t)>(
            base + entities::initialize_placement)(entity, matrix, 0, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void destroy(std::uintptr_t base, std::uintptr_t entity) {
    if (!entity) return;
    const auto owner = ptr(entity, 0x40);
    reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t)>(base + entities::destroy_entity)(entity, owner);
}
void place(std::uintptr_t base, std::uintptr_t entity, const std::array<float, 16> &matrix) {
    reinterpret_cast<void (*)(std::uintptr_t, const void *)>(base + entities::place_entity)(entity, matrix.data());
}
} // namespace

void request_effect_attach(std::string blueprint, float offset) {
    std::lock_guard lock(request().mutex);
    request().blueprint = std::move(blueprint);
    request().offset = offset;
    request().attach = true;
    request().detach = false;
}
void request_effect_attach_off() {
    std::lock_guard lock(request().mutex);
    request().detach = true;
    request().attach = false;
    request().blueprint.clear();
}
std::string effect_attach_status() {
    std::lock_guard lock(status_mutex());
    return attached().status;
}

std::string asset_loaded_report(std::string_view name) {
    try {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        std::uint32_t owners = 0, domains = 0;
        for (std::uint16_t domain = 0; domain < 0xbbf; ++domain) {
            std::uintptr_t owner{};
            if (!memory::read_bytes(base + addr::engine::domain_owners + domain * 8ULL, &owner, 8)) continue;
            ++domains;
            if (owner) ++owners;
        }
        const std::string text(name);
        bool faulted{};
        const auto asset = scan_domains(base, text.c_str(), faulted);
        return std::string(asset ? "Loaded: " : "Not loaded: ") + text +
               " (domains read " + std::to_string(domains) + ", with owners " + std::to_string(owners) +
               (faulted ? ", scan faulted)" : ")");
    } catch (const std::exception &e) {
        return std::string("findasset: ") + e.what();
    }
}

void tick_effect_attach(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        if (!base || !client) return;
        std::string blueprint;
        float offset{};
        bool attach{}, detach{};
        {
            std::lock_guard lock(request().mutex);
            blueprint = request().blueprint;
            offset = request().offset;
            attach = request().attach;
            detach = request().detach;
            request().attach = request().detach = false;
        }
        auto &a = attached();
        if (detach && a.active) {
            destroy(base, a.entity);
            reset(a);
            set_status("Effect detached.");
            logging::log(logging::Level::info, logging::Channel::skater, "Effect attach: detached.");
        }
        if (attach && !blueprint.empty()) {
            if (a.active) {
                destroy(base, a.entity);
                reset(a);
            }
            const auto local = find_local(base, client);
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Effect attach: local skater entity={:#x}, holder={:#x}.", local.entity, local.holder);
            const auto joint = head_world(base, local.holder);
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Effect attach: head world ({:.2f}, {:.2f}, {:.2f}).",
                         joint.position[0], joint.position[1], joint.position[2]);
            bool faulted{};
            const auto found = scan_domains(base, blueprint.c_str(), faulted);
            if (faulted)
                logging::log(logging::Level::warning, logging::Channel::skater,
                             "Effect attach: the asset scan faulted (caught); treating as not loaded.");
            if (!found) {
                set_status("Effect blueprint is not loaded: " + blueprint + ".");
                logging::log(logging::Level::warning, logging::Channel::skater, "Effect attach: {}", effect_attach_status());
                return;
            }
            const bool is_blueprint = derives(found, base + entities::blueprint_type);
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Effect attach: blueprint {:#x}, is_entity_blueprint={}.", found, is_blueprint);
            if (!is_blueprint) {
                set_status("Asset is not an entity blueprint, so it cannot be spawned this way: " + blueprint + ".");
                logging::log(logging::Level::warning, logging::Channel::skater, "Effect attach: {}", effect_attach_status());
                return;
            }
            const auto matrix = matrix_at(joint, offset);
            bool create_faulted{};
            const auto entity = create_guarded(base, local.parent, found, matrix.data(), create_faulted);
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Effect attach: create_entity -> {:#x} (faulted={}).", entity, create_faulted);
            if (!entity) {
                set_status(std::string("The engine did not create an entity for ") + blueprint +
                           (create_faulted ? " (the call faulted; caught)." : "."));
                logging::log(logging::Level::warning, logging::Channel::skater, "Effect attach: {}", effect_attach_status());
                return;
            }
            const bool placed = initialize_guarded(base, entity, matrix.data());
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Effect attach: initialize_placement ok={}.", placed);
            if (!placed) {
                destroy(base, entity);
                set_status("initialize_placement faulted for " + blueprint + " (caught; entity destroyed).");
                logging::log(logging::Level::warning, logging::Channel::skater, "Effect attach: {}", effect_attach_status());
                return;
            }
            a.active = true;
            a.base = base;
            a.context = local.context;
            a.entity = entity;
            a.parent = local.parent;
            a.blueprint = blueprint;
            set_status("Effect attached: " + blueprint + " at the head.");
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Effect attach: spawned {} as entity {:#x}.", blueprint, entity);
        }
        if (a.active) {
            if (a.context != ptr(client, 8)) {
                destroy(base, a.entity);
                reset(a);
                set_status("Effect detached (level changed).");
                return;
            }
            const auto local = find_local(base, client);
            place(base, a.entity, matrix_at(head_world(base, local.holder), offset));
        }
    } catch (const std::exception &e) {
        set_status(std::string("Effect attach: ") + e.what());
    } catch (...) {
        set_status("Effect attach: unknown failure.");
    }
}

} // namespace dingosdk::skater
