#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace dingosdk::player_skitch {
// Wipeout drag state. The bail motion states are animation-driven: their
// position/velocity outputs are recomputed every update from the wipeout
// context at core+0x3c0 (+0x7a0..+0x8e0), which the ragdoll/animation
// evaluation rewrites each frame -- external writes to the state or to the
// context are overwritten in-frame (proven by the probe: the context write
// landed every frame and read back unchanged).
//
// The one input the engine consumes from outside is the motion target: every
// offboard state update calls skater_motion (0x4776a80) with the transform its
// native root drive pulls the skater toward, the ground and ragdoll states
// included. While the grip holds a ragdoll plan this module replaces that
// target with a bounded step toward the tether follow slot, so the engine's own
// drive carries the body -- and the camera -- instead of a placement write.
//
// Armed with `dragstate on`, logged with `dragstate probe <seconds>`.
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
// Called from the skater-motion hook (client_noclip) with the rig wrapper, the
// context and the transform the native root drive would use. Returns true with
// `target` holding the drag step while a ragdoll plan is held.
bool drag_state_motion(std::uintptr_t rig, std::uintptr_t context,
    const std::array<float, 16>* supplied, std::array<float, 16>& target) noexcept;
std::string drag_state_status();
} // namespace dingosdk::player_skitch
