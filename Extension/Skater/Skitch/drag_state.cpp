#include "drag_state.h"
#include "../no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/offboard_drag.h"
#include "Engine/Game/Build/20260929/no_bail.h"
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
    std::atomic<std::uint64_t> pulls{};
    std::atomic<std::uint64_t> probe_until{};
    std::atomic<std::uintptr_t> last_state{};
};
Shared& shared() { static Shared value; return value; }

bool is_ragdoll_vtable(std::uintptr_t base, std::uintptr_t vtable) noexcept {
    return vtable == base + falling_vtable_rva || vtable == base + follow_ragdoll_vtable_rva ||
           vtable == base + follow_animated_ragdoll_vtable_rva;
}
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
    std::uintptr_t machine{}, machine_vtable{}, active{}, active_vtable{}, context{};
    memory::peek(core + core_machine_offset, machine);
    if (machine >= 0x10000) memory::peek(machine, machine_vtable);
    if (machine >= 0x10000) memory::peek(machine + machine_active_offset, active);
    if (active >= 0x10000) memory::peek(active, active_vtable);
    memory::peek(core + 0x3c0, context);
    if (context < 0x10000 && machine >= 0x10000) memory::peek(machine + machine_context_offset, context);
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
        "Drag state ctx[{}]: phys={} core=0x{:x} machine=0x{:x} mvt=0x{:x}{} active=0x{:x} avt=0x{:x}{} | "
        "v=({:.1f},{:.1f},{:.1f}) p=({:.1f},{:.1f},{:.1f}) pub=({:.1f},{:.1f},{:.1f}) |{}",
        tag, observed_physics_state(), core, machine, machine_vtable,
        machine_ok ? "" : " (expected 0x" + std::to_string(base + machine_vtable_rva) + ")",
        active, active_vtable, ragdoll ? " RAGDOLL" : "",
        state_velocity[0], state_velocity[1], state_velocity[2],
        state_position[0], state_position[1], state_position[2],
        published[0], published[1], published[2], body);
}
// The live ragdoll source: the context's 7a0 is the world position that gets
// published to machine+0xd30, with two copies at 7c0 and 830. Writing it from
// the animation point (after the ragdoll evaluation has written it, before the
// next step's state update reads it) is the one place a drag can land.
void apply_context_write() noexcept {
    auto& s = shared();
    // `active` is the tether plan's own ragdoll flag: drag_state_goal is only
    // published while the grip holds a ragdoll plan, so this is the bail
    // condition. (The selector's observed state is the caller's remapped
    // offboard value -- 504 for both walking and a wipeout -- so it cannot gate
    // this.)
    if (!s.armed.load(std::memory_order_relaxed) || !s.active.load(std::memory_order_relaxed)) return;
    const auto now = GetTickCount64();
    if (now > s.lease_until.load(std::memory_order_relaxed)) return;
    const auto base = s.base.load(std::memory_order_relaxed);
    const auto core = s.core.load(std::memory_order_relaxed);
    if (!base || core < 0x10000) return;
    std::uintptr_t context{};
    memory::peek(core + 0x3c0, context);
    if (context < 0x10000) return;
    std::array<float, 4> primary{}, copy_a{}, copy_b{};
    if (!memory::peek(context + 0x7a0, primary) || !memory::peek(context + 0x7c0, copy_a) ||
        !memory::peek(context + 0x830, copy_b)) return;
    for (const auto value : primary)
        if (!std::isfinite(value) || std::abs(value) > 100000.f) return;
    std::array<float, 3> goal{};
    for (std::size_t i = 0; i < 3; ++i) goal[i] = s.goal[i].load(std::memory_order_relaxed);
    constexpr float pull = 0.2f;
    std::array<float, 4> moved = primary;
    for (std::size_t i = 0; i < 3; ++i) moved[i] = primary[i] + (goal[i] - primary[i]) * pull;
    if (!write_bytes(context + 0x7a0, moved.data(), sizeof(float) * 3)) return;
    // Keep the two copies at their own offsets from the primary.
    std::array<float, 3> adjusted_a{}, adjusted_b{};
    for (std::size_t i = 0; i < 3; ++i) {
        adjusted_a[i] = moved[i] + (copy_a[i] - primary[i]);
        adjusted_b[i] = moved[i] + (copy_b[i] - primary[i]);
    }
    (void)write_bytes(context + 0x7c0, adjusted_a.data(), sizeof(float) * 3);
    (void)write_bytes(context + 0x830, adjusted_b.data(), sizeof(float) * 3);
    s.pulls.fetch_add(1, std::memory_order_relaxed);
    if (s.probe_until.load(std::memory_order_relaxed) > now) {
        static std::atomic<std::uint64_t> last_log{};
        auto previous = last_log.load(std::memory_order_relaxed);
        if (now - previous >= 250 && last_log.compare_exchange_strong(previous, now)) {
            logging::log(logging::Level::info, logging::Channel::runtime,
                "Drag state WRITE: context 7a0 ({:.1f},{:.1f},{:.1f}) -> ({:.1f},{:.1f},{:.1f}) goal ({:.1f},{:.1f},{:.1f})",
                primary[0], primary[1], primary[2], moved[0], moved[1], moved[2], goal[0], goal[1], goal[2]);
        }
    }
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
// Physics-step point: caches the local core and dumps the wipeout context.
void drag_state_apply(std::uintptr_t core) noexcept {
    auto& s = shared();
    if (core >= 0x10000) s.core.store(core, std::memory_order_relaxed);
    apply_context_write();
    probe_line("P");
}
// Animation point: the same dump from the animation callback, so the two
// timestamps per frame show where the evaluation writes the context. The drag
// write goes here too: this point runs after the ragdoll evaluation.
void drag_state_animation_probe() noexcept {
    apply_context_write();
    probe_line("A");
}
std::string drag_state_status() {
    auto& s = shared();
    if (!s.ready.load(std::memory_order_acquire)) return "Drag state: unavailable (state contracts did not match)";
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
        "Drag state: %s, %s, probe %s, %llu dumps, %llu pulls, state=0x%llx",
        s.armed.load() ? "armed" : "off", s.active.load() ? "plan" : "no plan",
        s.probe_until.load(std::memory_order_relaxed) > GetTickCount64() ? "on" : "off",
        static_cast<unsigned long long>(s.dumps.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(s.pulls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(s.last_state.load(std::memory_order_relaxed)));
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
