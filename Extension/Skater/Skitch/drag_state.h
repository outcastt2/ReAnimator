#pragma once
#include <cstdint>
#include <string>

namespace dingosdk::player_skitch {
// Wipeout source probe. The bail motion states (Falling, FollowRagdoll,
// FollowAnimatedRagdoll) are animation-driven: every update recomputes its
// outputs -- velocity at +0x10, position at +0x30, published placement at
// machine+0xd30 -- from the wipeout context at core+0x3c0, +0x7a0..+0x8e0,
// which is written upstream by the ragdoll/animation evaluation. Writing the
// state's fields is therefore discarded, and the earlier attempt to write the
// context was overwritten in-frame; the missing piece is *when* the evaluation
// writes it relative to the physics and animation points.
//
// This module (for now) does not move anything: it dumps those context fields
// from both frame points, tagged P (physics step) and A (animation callback),
// so the two timestamps per frame show where the live source is written and
// which field is the ragdoll's position/velocity. Armed with `dragstate on`,
// logged with `dragstate probe <seconds>`.
bool start_drag_state(std::uintptr_t base) noexcept;
bool drag_state_available() noexcept;
void set_drag_state(bool on) noexcept;
bool drag_state_armed() noexcept;
bool drag_state_active() noexcept;
// Published from the client tick while the grip holds a ragdoll plan.
void drag_state_goal(float x, float y, float z, float vx, float vy, float vz) noexcept;
void drag_state_release() noexcept;
void drag_state_probe(unsigned seconds) noexcept;
// Called from the physics-step hook (client_noclip) with the local core.
void drag_state_apply(std::uintptr_t core) noexcept;
// Called from the animation callback (player_skitch::animation_evaluated).
void drag_state_animation_probe() noexcept;
std::string drag_state_status();
} // namespace dingosdk::player_skitch
