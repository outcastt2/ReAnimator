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
#include <cstdarg>
#include <cstdio>
#include <cstring>

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
    std::atomic<bool> hooked{};
    std::atomic<bool> armed{};
    std::atomic<bool> active{};
    std::atomic<std::uint64_t> lease_until{};
    std::atomic<std::uintptr_t> base{};
    std::atomic<std::uintptr_t> core{};
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
// One diagnostic line per interval while the probe runs: why the drag did not
// apply, not just that it did.
void probe_note(const char* format, ...) noexcept {
    auto& s = shared();
    const auto until = s.probe_until.load(std::memory_order_relaxed);
    const auto now = GetTickCount64();
    if (!until || now >= until) return;
    static std::atomic<std::uint64_t> last_log{};
    auto previous = last_log.load(std::memory_order_relaxed);
    if (now - previous < 500 || !last_log.compare_exchange_strong(previous, now)) return;
    char message[256]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    logging::write(logging::Level::info, logging::Channel::runtime, message);
}
// The velocity write, at the one point that matters: inside the ragdoll state's
// own update, after it has recomputed the velocity and before the integration.
// The active substate is reached through the machine the physics core owns.
void apply_velocity() noexcept {
    auto& s = shared();
    if (!s.active.load(std::memory_order_acquire)) return;
    const auto now = GetTickCount64();
    if (now > s.lease_until.load(std::memory_order_acquire)) {
        s.active.store(false, std::memory_order_relaxed);
        return;
    }
    const auto base = s.base.load(std::memory_order_relaxed);
    const auto core = s.core.load(std::memory_order_relaxed);
    if (!base || core < 0x10000) return;
    std::uintptr_t machine{}, vtable{}, active{}, context{};
    if (!memory::peek(core + core_machine_offset, machine) || machine < 0x10000) {
        probe_note("Drag state: no offboard machine for core 0x%llx.", static_cast<unsigned long long>(core));
        return;
    }
    if (!memory::peek(machine, vtable) || vtable != base + machine_vtable_rva) {
        probe_note("Drag state: machine 0x%llx vtable 0x%llx (expected 0x%llx).",
            static_cast<unsigned long long>(machine), static_cast<unsigned long long>(vtable),
            static_cast<unsigned long long>(base + machine_vtable_rva));
        return;
    }
    if (!memory::peek(machine + machine_active_offset, active) || active < 0x10000) {
        probe_note("Drag state: no active substate on machine 0x%llx.", static_cast<unsigned long long>(machine));
        return;
    }
    if (!memory::peek(active, vtable) || !is_ragdoll_vtable(base, vtable)) {
        probe_note("Drag state: active substate 0x%llx vtable 0x%llx is not a ragdoll state.",
            static_cast<unsigned long long>(active), static_cast<unsigned long long>(vtable));
        return;
    }
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
    if (!write_bytes(active + state_velocity_offset, want.data(), sizeof(float) * 3)) {
        probe_note("Drag state: velocity at 0x%llx is not writable.", static_cast<unsigned long long>(active));
        return;
    }
    s.updates.fetch_add(1, std::memory_order_relaxed);
    s.last_state.store(active, std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i) {
        s.last_position[i].store(position[i], std::memory_order_relaxed);
        s.last_goal[i].store(goal[i], std::memory_order_relaxed);
        s.last_velocity[i].store(want[i], std::memory_order_relaxed);
    }
    probe_note("Drag state: state=0x%llx pos=(%.2f,%.2f,%.2f) goal=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f).",
        static_cast<unsigned long long>(active), position[0], position[1], position[2], goal[0], goal[1], goal[2],
        want[0], want[1], want[2]);
}
// The ragdoll motion wrapper takes its inputs as a by-value block, so a normal
// detour cannot forward it. This relay runs the helper and tail-jumps to the
// original with rcx/rdx/r8/r9 restored and the caller's stack block untouched.
struct Relay {
    std::uintptr_t code{};
    std::uintptr_t helper_offset{};
    std::uintptr_t original_offset{};
};
Relay& relay() { static Relay value; return value; }
void drag_wrapper_helper() noexcept { apply_velocity(); }
bool build_relay() noexcept {
    auto& r = relay();
    if (r.code) return true;
    //  sub rsp,C8h | save xmm0-5 | save rcx/rdx/r8/r9 | mov rax,helper | call rax
    //  restore all | add rsp,C8h | mov rax,original | jmp rax
    // The motion wrapper takes floats: the volatile xmm registers are arguments
    // too, so they are saved alongside the integer ones. Everything sits above
    // the helper's 32-byte shadow space, and the caller's stack block is never
    // touched.
    const std::array<unsigned char, 108> prefix{
        0x48,0x81,0xEC,0xC8,0x00,0x00,0x00,
        0x0F,0x11,0x44,0x24,0x48,
        0x0F,0x11,0x4C,0x24,0x58,
        0x0F,0x11,0x54,0x24,0x68,
        0x0F,0x11,0x5C,0x24,0x78,
        0x0F,0x11,0x64,0x24,0x88,
        0x0F,0x11,0x6C,0x24,0x98,
        0x48,0x89,0x4C,0x24,0x28,
        0x48,0x89,0x54,0x24,0x30,
        0x4C,0x89,0x44,0x24,0x38,
        0x4C,0x89,0x4C,0x24,0x40,
        0x48,0xB8};
    const std::array<unsigned char, 84> suffix{
        0xFF,0xD0,
        0x48,0x8B,0x4C,0x24,0x28,
        0x48,0x8B,0x54,0x24,0x30,
        0x4C,0x8B,0x44,0x24,0x38,
        0x4C,0x8B,0x4C,0x24,0x40,
        0x0F,0x10,0x44,0x24,0x48,
        0x0F,0x10,0x4C,0x24,0x58,
        0x0F,0x10,0x54,0x24,0x68,
        0x0F,0x10,0x5C,0x24,0x78,
        0x0F,0x10,0x64,0x24,0x88,
        0x0F,0x10,0x6C,0x24,0x98,
        0x48,0x81,0xC4,0xC8,0x00,0x00,0x00,
        0x48,0xB8};
    const std::size_t total = prefix.size() + sizeof(std::uintptr_t) + suffix.size() + sizeof(std::uintptr_t) + 2;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!memory) return false;
    std::size_t at = 0;
    std::memcpy(memory + at, prefix.data(), prefix.size());
    at += prefix.size();
    const auto helper = reinterpret_cast<std::uintptr_t>(&drag_wrapper_helper);
    std::memcpy(memory + at, &helper, sizeof(helper));
    r.helper_offset = at;
    at += sizeof(helper);
    std::memcpy(memory + at, suffix.data(), suffix.size());
    at += suffix.size();
    const std::uintptr_t placeholder{};
    std::memcpy(memory + at, &placeholder, sizeof(placeholder));
    r.original_offset = at;
    at += sizeof(placeholder);
    memory[at++] = 0xFF;
    memory[at++] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), memory, total);
    r.code = reinterpret_cast<std::uintptr_t>(memory);
    return true;
}
} // namespace

bool drag_state_available() noexcept { return shared().ready.load(std::memory_order_acquire); }
bool drag_state_armed() noexcept { return shared().armed.load(std::memory_order_relaxed); }
bool drag_state_active() noexcept { return shared().active.load(std::memory_order_relaxed); }

// The hook is installed lazily, on the first arm, and disabled when the mode is
// turned off: unarmed gameplay never runs the relay.
bool ensure_hook() noexcept {
    auto& s = shared();
    if (s.hooked.load(std::memory_order_acquire)) return true;
    const auto base = s.base.load(std::memory_order_relaxed);
    if (!base || !s.ready.load(std::memory_order_relaxed)) return false;
    if (!build_relay()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Drag state: the motion relay could not be allocated.");
        return false;
    }
    auto* target = reinterpret_cast<void*>(base + ragdoll_motion_wrapper_rva);
    void* original{};
    if (hook_prepare(target, reinterpret_cast<void*>(relay().code), &original) != HookOk || !original) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Drag state: the motion wrapper could not be prepared.");
        return false;
    }
    const auto patched = reinterpret_cast<std::uintptr_t>(original);
    if (!write_bytes(relay().code + relay().original_offset, &patched, sizeof(patched)) ||
        hook_enable(target) != HookOk) {
        logging::write(logging::Level::warning, logging::Channel::skater,
            "Drag state: the motion wrapper could not be enabled.");
        return false;
    }
    s.hooked.store(true, std::memory_order_release);
    logging::write(logging::Level::info, logging::Channel::skater,
        "Drag state: the ragdoll motion wrapper is hooked while the mode is armed.");
    return true;
}

void set_drag_state(bool on) noexcept {
    auto& s = shared();
    if (on) {
        (void)ensure_hook();
        s.armed.store(true, std::memory_order_relaxed);
        return;
    }
    s.armed.store(false, std::memory_order_relaxed);
    drag_state_release();
    const auto base = s.base.load(std::memory_order_relaxed);
    if (base && s.hooked.exchange(false, std::memory_order_acq_rel))
        (void)hook_disable(reinterpret_cast<void*>(base + ragdoll_motion_wrapper_rva));
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
// Called from the physics-step hook: caches the local physics core and writes
// the velocity once more, in case the wrapper relay is not reached on a path.
void drag_state_apply(std::uintptr_t core) noexcept {
    auto& s = shared();
    if (core >= 0x10000) s.core.store(core, std::memory_order_relaxed);
    apply_velocity();
}
std::string drag_state_status() {
    auto& s = shared();
    if (!s.ready.load(std::memory_order_acquire)) return "Drag state: unavailable (state contracts did not match)";
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
        "Drag state: %s, %s, hook %s, %llu updates, state=0x%llx pos=(%.2f,%.2f,%.2f) goal=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f)",
        s.armed.load() ? "armed" : "off", s.active.load() ? "dragging" : "idle",
        s.hooked.load() ? "on" : "off",
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
        std::array<unsigned char, 16> prologue{};
        if (!memory::read(base + ragdoll_motion_wrapper_rva, prologue) ||
            std::memcmp(prologue.data(), ragdoll_motion_wrapper_prologue.data(), prologue.size()) != 0) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                "Drag state is unavailable: the ragdoll motion wrapper contract did not match.");
            return false;
        }
        if (!build_relay()) {
            logging::write(logging::Level::warning, logging::Channel::skater,
                "Drag state is unavailable: the motion relay could not be allocated.");
            return false;
        }
        s.base.store(base, std::memory_order_release);
        s.ready.store(true, std::memory_order_release);
        logging::write(logging::Level::info, logging::Channel::skater,
            "Drag state ready: the bail motion states (Falling, FollowRagdoll, FollowAnimatedRagdoll) "
            "can be driven from the tether ('dragstate on'; the wrapper is hooked only while armed).");
        return true;
    } catch (...) {}
    return false;
}
} // namespace dingosdk::player_skitch
