#pragma once
#include <cstdint>

namespace dingosdk {
bool start_no_bail(std::uintptr_t image_base) noexcept;
bool no_bail_available() noexcept;
// Publish from the validated local client tick. Returns owner availability even
// when both controls are off. Manual protection expires if ticks stop arriving.
bool update_no_bail(std::uintptr_t client, std::uintptr_t entity, bool manual,
    bool flying, std::uint64_t flight_expires) noexcept;
void clear_no_bail() noexcept;
// S.K.A.T.E.: while `locked`, the local skater cannot get back on the board once it is off
// (its mount request is dropped). Publish from the client tick; it expires if ticks stop.
// Releasing it also leaves the skater's teleport option on the board again, since a turn's
// teleport may have set it off (skater component +0xc0) and the SDK's own teleports keep it.
void update_board_lock(std::uintptr_t client, std::uintptr_t entity, bool locked) noexcept;
// The physics state the local skater's selector last chose, for the trainer (air time, bail
// markers). Publish the skater to watch from the client tick; it expires if ticks stop.
struct PhysicsStateWatch {
    bool valid{};
    std::uint32_t state{};
    std::uint32_t previous{}; // the state before this one
    float previous_seconds{}; // how long that one lasted
    std::uint64_t changes{}, wipeouts{}; // counted since the process started
};
void watch_physics_state(std::uintptr_t client, std::uintptr_t entity) noexcept;
PhysicsStateWatch watched_physics_state() noexcept;
// Stopping flight must not discard the independent manual preference.
void clear_no_bail_flight() noexcept;
// The engine's physics state, as last selected by the physics selector hook.
// 504 is walking on foot, 300 is a ground wipeout, and the engine remaps
// between them; anything else means the skater is on the board. Published by
// the hook No Bail installs, so this is free to read.
std::uint32_t observed_physics_state() noexcept;

// Called after the native post-physics skeleton response has run for a rig.
// The response applies the engine's constraints to the pose, so a pose written
// in the animation callback is rewritten; this is the point after which a write
// survives to the renderer. Runs on the physics thread, in the animation update
// order. Set once; a null listener removes it.
using SkeletonResponded = void (*)(std::uintptr_t rig) noexcept;
void set_skeleton_responded_listener(SkeletonResponded listener) noexcept;
// The local skater as No Bail follows it (update_no_bail publishes it), for a feature that reads it
// (Hall of Meat): any thread, its ownership chain resolved again now.
struct NoBailSkater {
    std::uintptr_t base{}, entity{}, core{}, context{}, rig{};
};
bool no_bail_skater(NoBailSkater& skater) noexcept;
// Every physics step of that skater, whether or not No Bail protects it: No Bail's hook on the
// skeleton response runs once per step, right after the step's body contacts, and hands it on after
// its own filtering, so a wipeout it filtered never happened. `seconds` is the step's simulated time.
// One observer, nullptr for none; it runs on the physics thread.
using NoBailStepObserver = void (*)(const NoBailSkater& skater, float seconds, bool wipeout) noexcept;
void set_no_bail_step_observer(NoBailStepObserver observer) noexcept;
} // namespace dingosdk
