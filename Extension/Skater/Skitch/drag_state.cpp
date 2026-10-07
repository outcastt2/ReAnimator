#include "drag_state.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/offboard_drag.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace dingosdk::player_skitch {
namespace {
using namespace addr::offboard_drag;
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
// Published by the client tick, consumed inside the engine's physics step. The
// lease keeps a lost tick stream from freezing the ragdoll: the native motion
// resumes when it lapses.
struct Shared {
    std::atomic<bool> ready{};
    std::atomic<bool> armed{};
    std::atomic<bool> active{};
    std::atomic<std::uint64_t> lease_until{};
    std::atomic<std::uintptr_t> base{};
    std::array<std::atomic<float>, 3> goal{};
    std::array<std::atomic<float>, 3> goal_velocity{};
    std::atomic<std::uint64_t> updates{};
    std::atomic<std::uint64_t> probe_until{};
    std::atomic<std::uintptr_t> last_state{};
    std::array<std::atomic<float>, 3> last_position{};
    std::array<std::atomic<float>, 3> last_goal{};
    std::array<std::atomic<float>, 3> last_velocity{};
};
Shared& shared() { static Shared value; return value; }

bool writable(std::uintptr_t address, std::size_t size) noexcept {
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page)) ||
        page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(page.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
        return false;
    const auto base = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
    return address >= base && address - base <= page.RegionSize - size;
}
bool write_bytes(std::uintptr_t address, const void* source, std::size_t size) noexcept {
    if (!writable(address, size)) return false;
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), source, size, &written) &&
           written == size;
}
float dt_of(std::uintptr_t context) noexcept {
    float dt{};
    if (context && memory::peek(context + context_dt_offset, dt) && std::isfinite(dt) && dt > 0.001f && dt < 0.1f)
        return dt;
    return 1.f / 60.f;
}
bool is_ragdoll_vtable(std::uintptr_t base, std::uintptr_t vtable) noexcept {
    return vtable == base + falling_vtable_rva || vtable == base + follow_ragdoll_vtable_rva ||
           vtable == base + follow_animated_ragdoll_vtable_rva;
}
} // namespace

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
// Inside the bail state's own update, before it integrates the velocity into
// the position and publishes that placement. Replacing the velocity here is the
// lever the engine's own jump scaling uses for the walking states.
void drag_state_apply(std::uintptr_t core) noexcept {
    auto& s = shared();
    if (!s.active.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    if (now > s.lease_until.load(std::memory_order_acquire)) {
        s.active.store(false, std::memory_order_relaxed);
        return;
    }
    const auto base = s.base.load(std::memory_order_relaxed);
    if (!base || core < 0x10000) return;
    std::uintptr_t machine{}, vtable{}, active{}, context{};
    if (!memory::peek(core + core_machine_offset, machine) || machine < 0x10000) return;
    if (!memory::peek(machine, vtable) || vtable != base + machine_vtable_rva) return;
    if (!memory::peek(machine + machine_active_offset, active) || active < 0x10000) return;
    if (!memory::peek(active, vtable) || !is_ragdoll_vtable(base, vtable)) return;
    if (!memory::peek(machine + machine_context_offset, context) || context < 0x10000) return;
    std::array<float, 3> position{}, goal{}, goal_velocity{}, want{};
    for (std::size_t i = 0; i < 3; ++i) {
        goal[i] = s.goal[i].load(std::memory_order_relaxed);
        goal_velocity[i] = s.goal_velocity[i].load(std::memory_order_relaxed);
    }
    if (!memory::peek(active + state_position_offset, position)) return;
    for (const auto value : position)
        if (!std::isfinite(value) || std::abs(value) > 100000.f) return;
    constexpr float stiffness = 8.f;
    constexpr float max_speed = 60.f;
    for (std::size_t i = 0; i < 3; ++i) want[i] = (goal[i] - position[i]) * stiffness + goal_velocity[i];
    const float speed = std::sqrt(want[0] * want[0] + want[1] * want[1] + want[2] * want[2]);
    if (speed > max_speed)
        for (auto& value : want) value *= max_speed / speed;
    if (!write_bytes(active + state_velocity_offset, want.data(), sizeof(float) * 3)) return;
    s.updates.fetch_add(1, std::memory_order_relaxed);
    s.last_state.store(active, std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i) {
        s.last_position[i].store(position[i], std::memory_order_relaxed);
        s.last_goal[i].store(goal[i], std::memory_order_relaxed);
        s.last_velocity[i].store(want[i], std::memory_order_relaxed);
    }
    const auto until = s.probe_until.load(std::memory_order_relaxed);
    if (until && now < until) {
        static std::atomic<std::uint64_t> last_log{};
        auto previous = last_log.load(std::memory_order_relaxed);
        if (now - previous >= 500 && last_log.compare_exchange_strong(previous, now)) {
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Drag state: state=0x{:x} pos=({:.2f},{:.2f},{:.2f}) goal=({:.2f},{:.2f},{:.2f}) vel=({:.2f},{:.2f},{:.2f}).",
                active, position[0], position[1], position[2], goal[0], goal[1], goal[2], want[0], want[1], want[2]);
        }
    }
}
std::string drag_state_status() {
    auto& s = shared();
    if (!s.ready.load(std::memory_order_acquire)) return "Drag state: unavailable (state contracts did not match)";
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
        "Drag state: %s, %s, %llu updates, state=0x%llx pos=(%.2f,%.2f,%.2f) goal=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f)",
        s.armed.load() ? "armed" : "off", s.active.load() ? "dragging" : "idle",
        static_cast<unsigned long long>(s.updates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(s.last_state.load(std::memory_order_relaxed)),
        s.last_position[0].load(std::memory_order_relaxed), s.last_position[1].load(std::memory_order_relaxed),
        s.last_position[2].load(std::memory_order_relaxed),
        s.last_goal[0].load(std::memory_order_relaxed), s.last_goal[1].load(std::memory_order_relaxed),
        s.last_goal[2].load(std::memory_order_relaxed),
        s.last_velocity[0].load(std::memory_order_relaxed), s.last_velocity[1].load(std::memory_order_relaxed),
        s.last_velocity[2].load(std::memory_order_relaxed));
    return buffer;
}

bool start_drag_state(std::uintptr_t base) noexcept {
    LastError error;
    auto& s = shared();
    if (s.ready.load()) return true;
    try {
        // Each ragdoll substate's vtable must still dispatch its motion update
        // at +0x18, and the machine's own vtable must be the one the physics
        // core stores at +0x3b0. No hook is needed: the physics-step hook the
        // runtime already installs runs inside these updates.
        const std::array<std::pair<std::uintptr_t, std::uintptr_t>, 3> contracts{{
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
            "Drag state ready: the bail motion states (Falling, FollowRagdoll, FollowAnimatedRagdoll) "
            "can be driven from the tether ('dragstate on').");
        return true;
    } catch (...) {}
    return false;
}
} // namespace dingosdk::player_skitch
