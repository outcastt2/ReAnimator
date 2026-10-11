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

// The same write, interpolating between two clip frames: a clip authored at
// 24 fps played back at 60 Hz snaps between frames otherwise, which reads as a
// stutter. Pass the last frame and the first frame as the pair and the loop
// closes without a jump.
void write_pose_interpolated(std::uintptr_t buffer, const float *frame_a, const float *frame_b, float alpha,
                             std::uint32_t first, std::uint32_t count, float keep) noexcept;

// Floor correction. A clip baked in Blender has no idea where the game's
// floor is, so its feet sink into the board or the ground. The game's own
// gesture IK already answered that question in the very pose this unit
// replaces: the engine's feet are planted on whatever the skater stands on.
// The two leg chains, sampled just before the clip is written, carry that
// answer -- per foot, so a stance with the feet on two different elevations
// (a ledge, a stair) comes out right, plus the way each knee bends and the
// pitch of each planted foot.
struct LegFloor {
    bool valid = false;         // the chain composed at all
    bool toe_valid = false;     // the ankle->toe vector was long enough to trust
    float foot_y = 0.0f;        // world Y of this foot's lowest point (ankle, toe)
    float ankle_offset = 0.0f;  // the game's ankle rides this far above its foot
    float knee[3]{};            // unit: the way the game's knee bends (world)
    float toe_dir[3]{};         // unit: the game's ankle->toe direction (the sole's pitch)
};
struct FloorSample {
    bool valid = false;
    float floor_y = 0.0f;       // lowest of both feet: what hands and the torso clear
    LegFloor legs[2]{};         // 0 = right chain, 1 = left chain
};
bool sample_floors(std::uintptr_t buffer, FloorSample &out) noexcept;

// After the clip write: correct what the clip drove against the sampled
// ground by an analytic two-bone solve per limb, so the legs and arms bend
// instead of stretching.
//   legs  per-leg floor: lift what clips through, and -- only where the
//         game's own two feet sit at different heights, i.e. the terrain is
//         uneven -- follow the leg's own foot down or up within a band, so a
//         dangling foot reaches its surface instead of floating. While the
//         foot is near its floor the sole is also pitched flat onto the
//         game's planted-foot direction and the ankle held at the height
//         that keeps the toe on the ground.
//   torso the spine+head chain is lifted clear of the floor plane (the
//         pelvis is the game's, so the hips are already safe)
//   arms  always clip-driven: lifted clear of the floor plane; a hand
//         resting on the floor is a handplant and stays
// `margin` is the tolerated sink (authored near-floor poses and joint noise
// are left alone); `strength` scales every correction, 0 disables. Legs
// respect the mask (scaled by 1 - keep: a leg the mask kept is the game's
// own planted foot and is never touched). Only rotations are written; every
// position, scale and spare float in the buffer stays exactly as the clip
// (or the game) left it.
void apply_floor(std::uintptr_t buffer, const FloorSample &floor, float margin, float strength,
                 float keep) noexcept;

} // namespace dingosdk::skater::layers
