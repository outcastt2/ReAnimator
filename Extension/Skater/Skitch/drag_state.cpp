#include "drag_state.h"
#include "../no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/offboard_drag.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace dingosdk::player_skitch {
namespace {
using namespace addr::offboard_drag;
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
struct Shared {
    std::atomic<bool> ready{};
    std::atomic<bool> armed{};
    std::atomic<bool> active{};
    std::atomic<std::uint64_t> lease_until{};
    std::atomic<std::uintptr_t> base{};
    std::atomic<std::uintptr_t> core{};
    std::array<std::atomic<float>, 3> goal{};
    std::array<std::atomic<float>, 3> goal_velocity{};
    std::atomic<std::uint64_t> dumps{};
    std::atomic<std::uint64_t> motion_pulls{};
    std::atomic<std::uint64_t> probe_until{};
    std::atomic<std::uintptr_t> last_state{};
};
Shared& shared() { static Shared value; return value; }

bool is_ragdoll_vtable(std::uintptr_t base, std::uintptr_t vtable) noexcept {
    return vtable == base + falling_vtable_rva || vtable == base + follow_ragdoll_vtable_rva ||
           vtable == base + follow_animated_ragdoll_vtable_rva;
}
// The context is the address the machine constructor received, core+0x3c0,
// stored at machine+8 (see the wipeout notes: the member updates read
// [machine+8], which the constructor sets from rdx = core+0x3c0). Read it from
// machine+8 first; fall back to the address itself if that slot is unusable.
std::uintptr_t read_context(std::uintptr_t core) noexcept {
    std::uintptr_t machine{}, context{};
    if (memory::peek(core + core_machine_offset, machine) && machine >= 0x10000) {
        memory::peek(machine + machine_context_offset, context);
        if (context >= 0x10000) return context;
    }
    return core + 0x3c0 >= 0x10000 ? core + 0x3c0 : 0;
}
// Rate-limited motion log, only while the probe runs.
void motion_log(std::uintptr_t member, const std::array<float, 3>& from,
    const std::array<float, 3>& to, const std::array<float, 3>& goal) noexcept {
    auto& s = shared();
    const auto now = GetTickCount64();
    if (s.probe_until.load(std::memory_order_relaxed) <= now) return;
    static std::atomic<std::uint64_t> last_log{};
    auto previous = last_log.load(std::memory_order_relaxed);
    if (now - previous < 250 || !last_log.compare_exchange_strong(previous, now)) return;
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Drag state MOTION: member=0x{:x} target ({:.1f},{:.1f},{:.1f}) -> ({:.1f},{:.1f},{:.1f}) goal ({:.1f},{:.1f},{:.1f})",
        member, from[0], from[1], from[2], to[0], to[1], to[2], goal[0], goal[1], goal[2]);
}
void probe_line(const char* tag) noexcept {
    auto& s = shared();
    const auto now = GetTickCount64();
    // One line per point, ever: tells "never called" apart from "called but
    // filtered", which is exactly what the last probe could not say.
    static std::atomic<bool> first[2]{};
    const auto slot = tag[0] == 'A' ? 1 : 0;
    if (!first[slot].exchange(true)) {
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Drag state: {} point reached.", tag[0] == 'A' ? "animation" : "physics");
    }
    const auto until = s.probe_until.load(std::memory_order_relaxed);
    if (!until || now >= until) return;
    static std::atomic<std::uint64_t> last_log[2]{};
    auto previous = last_log[slot].load(std::memory_order_relaxed);
    if (now - previous < 250 || !last_log[slot].compare_exchange_strong(previous, now)) return;
    const auto base = s.base.load(std::memory_order_relaxed);
    const auto core = s.core.load(std::memory_order_relaxed);
    if (!base || core < 0x10000) {
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Drag state ctx[{}]: no base/core yet (base=0x{:x} core=0x{:x}).", tag, base, core);
        return;
    }
    std::uintptr_t machine{}, machine_vtable{}, active{}, active_vtable{};
    memory::peek(core + core_machine_offset, machine);
    if (machine >= 0x10000) memory::peek(machine, machine_vtable);
    if (machine >= 0x10000) memory::peek(machine + machine_active_offset, active);
    if (active >= 0x10000) memory::peek(active, active_vtable);
    // Raw context sources for the log: [machine+8] is the documented slot (the
    // constructor stored core+0x3c0 there); [core+0x3c0] is the first field of
    // the context itself, so reading it as a pointer is only a fallback check.
    std::uintptr_t raw_core{}, raw_machine{};
    memory::peek(core + 0x3c0, raw_core);
    if (machine >= 0x10000) memory::peek(machine + machine_context_offset, raw_machine);
    const auto context = read_context(core);
    const bool machine_ok = machine_vtable == base + machine_vtable_rva;
    const bool ragdoll = is_ragdoll_vtable(base, active_vtable);
    const std::array<std::uintptr_t, 7> fields{0x7a0, 0x7b0, 0x7c0, 0x7d0, 0x7e0, 0x7f0, 0x830};
    char body[512]{};
    std::size_t at = 0;
    if (context >= 0x10000) {
        for (const auto offset : fields) {
            std::array<float, 4> values{};
            if (!memory::peek(context + offset, values)) break;
            at += static_cast<std::size_t>(std::snprintf(body + at, sizeof(body) - at,
                " %x=(%.1f,%.1f,%.1f,%.1f)", static_cast<unsigned>(offset), values[0], values[1], values[2], values[3]));
        }
    }
    std::array<float, 3> state_velocity{}, state_position{}, published{};
    if (active >= 0x10000) memory::peek(active + state_velocity_offset, state_velocity);
    if (active >= 0x10000) memory::peek(active + state_position_offset, state_position);
    if (machine >= 0x10000) memory::peek(machine + machine_publish_offset, published);
    s.dumps.fetch_add(1, std::memory_order_relaxed);
    s.last_state.store(active, std::memory_order_relaxed);
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Drag state ctx[{}]: phys={} core=0x{:x} machine=0x{:x} mvt=0x{:x}{} active=0x{:x} avt=0x{:x}{} "
        "ctx=0x{:x} c3c0=0x{:x} m8=0x{:x} | v=({:.1f},{:.1f},{:.1f}) p=({:.1f},{:.1f},{:.1f}) "
        "pub=({:.1f},{:.1f},{:.1f}) |{}",
        tag, observed_physics_state(), core, machine, machine_vtable,
        machine_ok ? "" : " (expected 0x" + std::to_string(base + machine_vtable_rva) + ")",
        active, active_vtable, ragdoll ? " RAGDOLL" : "",
        context, raw_core, raw_machine,
        state_velocity[0], state_velocity[1], state_velocity[2],
        state_position[0], state_position[1], state_position[2],
        published[0], published[1], published[2], body);
}
} // namespace

// The engine's own root drive. Every offboard state update calls skater_motion
// (0x4776a80) with the target transform its native spring pulls the skater's
// root toward -- the ground and ragdoll states included. While the grip holds a
// ragdoll plan, replace the translation with a bounded step toward the tether
// follow slot: the engine itself carries the root, and the camera with it,
// instead of a placement write the evaluation overwrites in-frame.
bool drag_state_motion(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float, 16>* supplied, std::array<float, 16>& target) noexcept {
    auto& s = shared();
    // `active` is the tether plan's own ragdoll flag: drag_state_goal is only
    // published while the grip holds a ragdoll plan, so this is the bail
    // condition. (The selector's observed state is the caller's remapped
    // offboard value -- 504 for both walking and a wipeout -- so it cannot gate
    // this.)
    if (!s.armed.load(std::memory_order_relaxed) || !s.active.load(std::memory_order_relaxed)) return false;
    const auto now = GetTickCount64();
    if (now > s.lease_until.load(std::memory_order_relaxed)) return false;
    const auto core = s.core.load(std::memory_order_relaxed);
    if (core < 0x10000 || !supplied) return false;
    // Identity: the rig wrapper links back to the context and to the core.
    std::uintptr_t linked{};
    if (!memory::peek(rig, linked) || linked != context) return false;
    if (!memory::peek(rig + 0x4630, linked) || linked != core) return false;
    target = *supplied;
    // The drive derives the rig velocity from (target - previous target)/dt, so
    // a constant offset cancels after one step. Advance from the rig's previous
    // target instead, exactly like the flight's root + velocity*step: the drive
    // then moves the rig at `drag_speed` toward the goal every step.
    std::array<float, 4> previous{};
    if (!memory::peek(rig + 0x4750, previous)) return false;
    const std::array<float, 3> from{previous[0], previous[1], previous[2]};
    for (const auto value : from)
        if (!std::isfinite(value) || std::abs(value) > 100000.f) return false;
    std::array<float, 3> goal{};
    for (std::size_t i = 0; i < 3; ++i) goal[i] = s.goal[i].load(std::memory_order_relaxed);
    std::array<float, 3> delta{};
    for (std::size_t i = 0; i < 3; ++i) delta[i] = goal[i] - from[i];
    const float distance = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    if (!std::isfinite(distance) || distance > 60.f) return false;
    // The native drive moves the root toward this target over the step, so a
    // step of speed*dt drags the body at that speed; dt comes from the context
    // so the speed stays frame-rate independent.
    float dt = 1.f / 60.f;
    memory::peek(context + context_dt_offset, dt);
    if (!std::isfinite(dt) || dt <= 0.f || dt > 0.5f) dt = 1.f / 60.f;
    constexpr float drag_speed = 2.5f;
    // Always write from the previous target -- never fall back to the state's
    // own supplied position. At the goal the step is zero and the target must
    // hold there; falling back would snap the drive to the ragdoll every frame.
    const float step = std::min(distance, drag_speed * dt);
    const float scale = distance > 1e-4f ? step / distance : 0.f;
    for (std::size_t i = 0; i < 3; ++i) target[12 + i] = from[i] + delta[i] * scale;
    // The active member identifies which state's motion call this was.
    std::uintptr_t machine{}, active{};
    if (memory::peek(core + core_machine_offset, machine) && machine >= 0x10000)
        memory::peek(machine + machine_active_offset, active);
    const auto member = active >= machine && machine >= 0x10000 ? active - machine : 0;
    s.motion_pulls.fetch_add(1, std::memory_order_relaxed);
    motion_log(member, from, {target[12], target[13], target[14]}, goal);
    return true;
}

bool drag_state_available() noexcept { return shared().ready.load(std::memory_order_acquire); }
bool drag_state_armed() noexcept { return shared().armed.load(std::memory_order_relaxed); }
bool drag_state_active() noexcept { return shared().active.load(std::memory_order_relaxed); }

void set_drag_state(bool on) noexcept {
    auto& s = shared();
    s.armed.store(on, std::memory_order_relaxed);
    if (!on) drag_state_release();
}
void drag_state_goal(float x, float y, float z, float vx, float vy, float vz) noexcept {
    auto& s = shared();
    if (!s.armed.load(std::memory_order_relaxed)) return;
    s.goal[0].store(x, std::memory_order_relaxed);
    s.goal[1].store(y, std::memory_order_relaxed);
    s.goal[2].store(z, std::memory_order_relaxed);
    s.goal_velocity[0].store(vx, std::memory_order_relaxed);
    s.goal_velocity[1].store(vy, std::memory_order_relaxed);
    s.goal_velocity[2].store(vz, std::memory_order_relaxed);
    s.lease_until.store(GetTickCount64() + 250, std::memory_order_relaxed);
    s.active.store(true, std::memory_order_relaxed);
}
void drag_state_release() noexcept {
    auto& s = shared();
    s.active.store(false, std::memory_order_relaxed);
    s.lease_until.store(0, std::memory_order_relaxed);
}
void drag_state_probe(unsigned seconds) noexcept {
    auto& s = shared();
    s.probe_until.store(GetTickCount64() + static_cast<std::uint64_t>(seconds) * 1000, std::memory_order_relaxed);
}
bool drag_state_probing() noexcept {
    return shared().probe_until.load(std::memory_order_relaxed) > GetTickCount64();
}
// Physics-step point: caches the local core and dumps the wipeout context.
void drag_state_apply(std::uintptr_t core) noexcept {
    auto& s = shared();
    if (core >= 0x10000) s.core.store(core, std::memory_order_relaxed);
    probe_line("P");
}
// Animation point: the same dump from the animation callback, so the two
// timestamps per frame show where the evaluation writes the context.
void drag_state_animation_probe() noexcept { probe_line("A"); }
std::string drag_state_status() {
    auto& s = shared();
    if (!s.ready.load(std::memory_order_acquire)) return "Drag state: unavailable (state contracts did not match)";
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
        "Drag state: %s, %s, probe %s, %llu dumps, %llu motion pulls, state=0x%llx",
        s.armed.load() ? "armed" : "off", s.active.load() ? "plan" : "no plan",
        s.probe_until.load(std::memory_order_relaxed) > GetTickCount64() ? "on" : "off",
        static_cast<unsigned long long>(s.dumps.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(s.motion_pulls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(s.last_state.load(std::memory_order_relaxed)));
    return buffer;
}

bool start_drag_state(std::uintptr_t base) noexcept {
    LastError error;
    auto& s = shared();
    if (s.ready.load()) return true;
    try {
        const std::array<std::pair<std::uintptr_t, std::uintptr_t>, 4> contracts{{
            {follow_trajectory_vtable_rva, follow_trajectory_update_rva},
            {falling_vtable_rva, falling_update_rva},
            {follow_ragdoll_vtable_rva, follow_ragdoll_update_rva},
            {follow_animated_ragdoll_vtable_rva, follow_animated_ragdoll_update_rva}}};
        for (const auto& contract : contracts) {
            std::uintptr_t update{};
            if (!memory::read(base + contract.first + 0x18, update) || update != base + contract.second) {
                logging::write(logging::Level::warning, logging::Channel::skater,
                    "Drag state is unavailable: a ragdoll substate contract did not match.");
                return false;
            }
        }
        s.base.store(base, std::memory_order_release);
        s.ready.store(true, std::memory_order_release);
        logging::write(logging::Level::info, logging::Channel::skater,
            "Drag state ready: the wipeout context (core+0x3c0, +0x7a0..+0x8e0) can be probed "
            "from the physics and animation points ('dragstate on', 'dragstate probe 30').");
        return true;
    } catch (...) {}
    return false;
}
} // namespace dingosdk::player_skitch
