#pragma once
#include <cstddef>
#include <cstdint>

// Layering a custom animation over the game's own pose.
//
// The engine's output pose is an array of joint bodies, 0x30 bytes each:
// scale.xyz, a spare float, quat.xyzw, pos.xyz, a spare float. A clip carries
// the same fields as 10 floats per joint. This unit is the whole of the
// mapping, kept free of game code so it can be exercised on the host:
// Extension/Skater/Test/pose_layer_tests.cpp does exactly that, because an
// off-by-one here silently scrambles every joint of the skater.
namespace dingosdk::skater::layers {

inline constexpr std::size_t pose_stride = 0x30;  // bytes per joint in the pose
inline constexpr std::size_t clip_stride = 10;    // floats per joint in a clip

// The clip drives the body above the pelvis, and the engine keeps everything
// else.
//
// The spine at joint 42 owns joints 42..340: torso, neck, head and both arms.
// Everything outside that -- the world placement at 1, the helper clusters the
// engine hangs off it (2..6 and 375..394), the pelvis and both legs -- belongs
// to the game. Writing the clip over a helper cluster is what floated the
// skater above the board: those joints track the board and the physics, and the
// clip holds them wherever they were when it was authored.
inline constexpr std::uint32_t clip_first_joint = 42;
inline constexpr std::uint32_t clip_last_joint = 340;
bool joint_masked(std::uint32_t joint) noexcept;

// Compose a joint's world position from the pose, walking a chain of ancestors
// root first -- {1, 7, 8, 9, 10} reaches the left ankle. The first joint's own
// translation is taken as already world; each step then applies its parent's
// rotation and average scale, exactly as the engine composes a pose. For
// diagnostics: the buffer is parent-local, so a raw offset between two joints is
// meaningless without this.
bool world_position(std::uintptr_t buffer, const std::uint32_t *chain, std::size_t count,
                    float out[3]) noexcept;

// Ramp a 0..1 weight toward `target`, moving at most 1/ramp_ms per elapsed ms.
float ramp_weight(float weight, float target, std::uint64_t elapsed_ms, std::uint64_t ramp_ms) noexcept;

// Write `count` clip joints starting at joint `first` into the pose buffer,
// which must already point at that joint's body. `frame` must point at the
// clip's fields for the same joint.
//
//   keep <= 0  the clip wins outright
//   keep >= 1  the game keeps every masked joint; the rest still take the clip
//   in between the masked joints blend from the buffer's current value (the
//              game's, written this frame) toward the clip
void write_pose(std::uintptr_t buffer, const float *frame, std::uint32_t first, std::uint32_t count,
                float keep) noexcept;

} // namespace dingosdk::skater::layers
