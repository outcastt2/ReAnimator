#include "drag_state.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/offboard_drag.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dingosdk::player_skitch {
namespace {
using namespace addr::offboard_drag;
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
// Published by the client tick, read inside the engine's physics step. The
// lease keeps a lost tick stream from freezing the ragdoll: the native motion
// resumes when it lapses.
struct Shared {
    std::atomic<bool> ready{};
    std::atomic<bool> armed{};
    std::atomic<bool> active{};
    std::atomic<std::uint64_t> lease_until{};
    std::array<std::atomic<float>, 3> goal{};
    std::array<std::atomic<float>, 3> goal_velocity{};
    std::atomic<std::uint64_t> updates{};
    std::atomic<std::uint64_t> probe_until{};
    std::array<std::atomic<float>, 3> last_position{};
    std::array<std::atomic<float>, 3> last_goal{};
};
Shared& shared() { static Shared value; return value; }

using UpdateFn = void (*)(void*, void*);
UpdateFn original_falling{};
UpdateFn original_follow_ragdoll{};
UpdateFn original_follow_animated_ragdoll{};

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
// Runs after the native update, so every transition, flag and publish the state
// made still happened; only the motion is replaced.
void drag_after_update(void* state, void* machine) noexcept {
    auto& s = shared();
    if (!s.active.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    if (now > s.lease_until.load(std::memory_order_acquire)) {
        s.active.store(false, std::memory_order_relaxed);
        return;
    }
    const auto state_address = reinterpret_cast<std::uintptr_t>(state);
    const auto machine_address = reinterpret_cast<std::uintptr_t>(machine);
    if (state_address < 0x10000 || machine_address < 0x10000) return;
    std::uintptr_t context{};
    if (!memory::peek(machine_address + machine_context_offset, context) || context < 0x10000) return;
    std::array<float, 3> position{}, goal{}, want{}, moved{};
    for (std::size_t i = 0; i < 3; ++i) goal[i] = s.goal[i].load(std::memory_order_relaxed);
    if (!memory::peek(state_address + state_position_offset, position)) return;
    for (const auto value : position)
        if (!std::isfinite(value) || std::abs(value) > 100000.f) return;
    const auto dt = dt_of(context);
    constexpr float stiffness = 12.f;
    constexpr float max_speed = 40.f;
    for (std::size_t i = 0; i < 3; ++i) want[i] = (goal[i] - position[i]) * stiffness;
    const float speed = std::sqrt(want[0] * want[0] + want[1] * want[1] + want[2] * want[2]);
    if (speed > max_speed)
        for (auto& value : want) value *= max_speed / speed;
    for (std::size_t i = 0; i < 3; ++i) moved[i] = position[i] + want[i] * dt;
    if (!write_bytes(state_address + state_position_offset, moved.data(), sizeof(float) * 3) ||
        !write_bytes(state_address + state_velocity_offset, want.data(), sizeof(float) * 3) ||
        !write_bytes(machine_address + machine_publish_offset, moved.data(), sizeof(float) * 3)) return;
    s.updates.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i) {
        s.last_position[i].store(moved[i], std::memory_order_relaxed);
        s.last_goal[i].store(goal[i], std::memory_order_relaxed);
    }
    const auto until = s.probe_until.load(std::memory_order_relaxed);
    if (until && now < until) {
        static std::atomic<std::uint64_t> last_log{};
        auto previous = last_log.load(std::memory_order_relaxed);
        if (now - previous >= 500 && last_log.compare_exchange_strong(previous, now)) {
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Drag state: state=0x{:x} pos=({:.2f},{:.2f},{:.2f}) goal=({:.2f},{:.2f},{:.2f}) vel=({:.2f},{:.2f},{:.2f}).",
                state_address, moved[0], moved[1], moved[2], goal[0], goal[1], goal[2], want[0], want[1], want[2]);
        }
    }
}
void detour_falling(void* state, void* machine) {
    if (original_falling) original_falling(state, machine);
    drag_after_update(state, machine);
}
void detour_follow_ragdoll(void* state, void* machine) {
    if (original_follow_ragdoll) original_follow_ragdoll(state, machine);
    drag_after_update(state, machine);
}
void detour_follow_animated_ragdoll(void* state, void* machine) {
    if (original_follow_animated_ragdoll) original_follow_animated_ragdoll(state, machine);
    drag_after_update(state, machine);
}
bool contract(std::uintptr_t base, std::uintptr_t rva, const unsigned char* bytes) noexcept {
    std::array<unsigned char, 16> actual{};
    return memory::read(base + rva, actual) && std::memcmp(actual.data(), bytes, actual.size()) == 0;
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
std::string drag_state_status() {
    auto& s = shared();
    if (!s.ready.load(std::memory_order_acquire)) return "Drag state: unavailable (hook contracts did not match)";
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
        "Drag state: %s, %s, %llu updates, pos=(%.2f,%.2f,%.2f) goal=(%.2f,%.2f,%.2f)",
        s.armed.load() ? "armed" : "off", s.active.load() ? "dragging" : "idle",
        static_cast<unsigned long long>(s.updates.load(std::memory_order_relaxed)),
        s.last_position[0].load(std::memory_order_relaxed), s.last_position[1].load(std::memory_order_relaxed),
        s.last_position[2].load(std::memory_order_relaxed),
        s.last_goal[0].load(std::memory_order_relaxed), s.last_goal[1].load(std::memory_order_relaxed),
        s.last_goal[2].load(std::memory_order_relaxed));
    return buffer;
}

bool start_drag_state(std::uintptr_t base) noexcept {
    LastError error;
    auto& s = shared();
    if (s.ready.load()) return true;
    try {
        if (!contract(base, falling_update_rva, falling_update_prologue.data()) ||
            !contract(base, follow_ragdoll_update_rva, follow_ragdoll_update_prologue.data()) ||
            !contract(base, follow_animated_ragdoll_update_rva, follow_animated_ragdoll_update_prologue.data())) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                "Drag state is unavailable: the ragdoll update contracts did not match.");
            return false;
        }
        const std::array targets{
            reinterpret_cast<void*>(base + falling_update_rva),
            reinterpret_cast<void*>(base + follow_ragdoll_update_rva),
            reinterpret_cast<void*>(base + follow_animated_ragdoll_update_rva)};
        const std::array replacements{
            reinterpret_cast<void*>(&detour_falling),
            reinterpret_cast<void*>(&detour_follow_ragdoll),
            reinterpret_cast<void*>(&detour_follow_animated_ragdoll)};
        std::array<void*, 3> originals{};
        auto status = HookOk;
        std::size_t prepared{};
        for (; prepared < targets.size(); ++prepared) {
            status = hook_prepare(targets[prepared], replacements[prepared], &originals[prepared]);
            if (status != HookOk || !originals[prepared]) break;
        }
        if (status == HookOk) {
            // Publish every relay before enabling any target.
            original_falling = reinterpret_cast<UpdateFn>(originals[0]);
            original_follow_ragdoll = reinterpret_cast<UpdateFn>(originals[1]);
            original_follow_animated_ragdoll = reinterpret_cast<UpdateFn>(originals[2]);
            for (auto target : targets) {
                status = hook_enable(target);
                if (status != HookOk) break;
            }
            if (status == HookOk) {
                s.ready.store(true, std::memory_order_release);
                logging::write(logging::Level::info, logging::Channel::skater,
                    "Drag state ready: the bail motion states (Falling, FollowRagdoll, FollowAnimatedRagdoll) "
                    "can be driven from the tether ('dragstate on').");
                return true;
            }
        }
        logging::log(logging::Level::warning, logging::Channel::skater,
            "Drag state hook setup failed (status {}); native bail motion stays.", static_cast<LONG>(status));
        while (prepared) (void)hook_remove(targets[--prepared]);
    } catch (...) {}
    return false;
}
} // namespace dingosdk::player_skitch
