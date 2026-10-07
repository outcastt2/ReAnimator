#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::player_skitch {
// Native ragdoll drag. The bail's motion states (Falling, FollowRagdoll,
// FollowAnimatedRagdoll) are animation-driven: every update recomputes the
// velocity from the offboard machine's context, integrates it into the state's
// position and publishes that to the machine -- which is why the whole-body
// drag had to live in the render pose. This takes the lever the engine itself
// uses (the same one the flight jump-scaling uses): from the physics-step hook,
// inside the state's own update and before its position integration, the tether
// velocity is written into the active ragdoll state. The engine's own
// integration and placement publish then carry the body, so the core, the
// render placement and the camera follow -- no render-only write.
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
// Called from the physics-step hook (client_noclip) with the local physics
// core, in the animation update order, before the state's position integration.
void drag_state_apply(std::uintptr_t core) noexcept;
std::string drag_state_status();
} // namespace dingosdk::player_skitch
