#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::player_skitch {
// Native ragdoll drag. The bail's motion states (Falling, FollowRagdoll,
// FollowAnimatedRagdoll) are animation-driven and recompute their position from
// the offboard machine's context every update, so no external write to their
// fields survives -- the reason the whole-body drag had to live in the render
// pose. This hooks the three state updates: the original runs first, keeping
// all of its bookkeeping, transitions and its own published placement intact,
// and then, while a drag goal is published, the state's velocity and position
// and the machine's published placement are replaced with the tether motion.
// The core, the render placement and the camera then follow the engine's own
// placement channel instead of a render-only write.
bool start_drag_state(std::uintptr_t base) noexcept;
bool drag_state_available() noexcept;
// Console arm switch: while armed, a published goal takes over the bail motion.
void set_drag_state(bool on) noexcept;
bool drag_state_armed() noexcept;
bool drag_state_active() noexcept;
// Published from the client tick while the grip holds a ragdoll plan. The goal
// is the tether follow slot in world coordinates; the lease expires if the
// ticks stop, so a lost target restores the native motion instead of freezing
// the ragdoll.
void drag_state_goal(float x, float y, float z, float vx, float vy, float vz) noexcept;
void drag_state_release() noexcept;
void drag_state_probe(unsigned seconds) noexcept;
std::string drag_state_status();
} // namespace dingosdk::player_skitch
