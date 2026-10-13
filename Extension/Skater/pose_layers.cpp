#include "pose_layers.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dingosdk::skater::layers {
namespace {
constexpr std::size_t offset_scale = 0x00;
constexpr std::size_t offset_quat = 0x10;
constexpr std::size_t offset_pos = 0x20;

void joint_fields(std::uintptr_t buffer, std::uint32_t joint, float scale[3], float quat[4],
                  float pos[3]) noexcept {
    const auto *at = reinterpret_cast<const std::uint8_t *>(buffer) + static_cast<std::size_t>(joint) * pose_stride;
    std::memcpy(scale, at + offset_scale, 12);
    std::memcpy(quat, at + offset_quat, 16);
    std::memcpy(pos, at + offset_pos, 12);
}

// a * b, both as (x, y, z, w).
void multiply(const float a[4], const float b[4], float out[4]) noexcept {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// v rotated by the quaternion q (x, y, z, w).
void rotate(const float q[4], const float v[3], float out[3]) noexcept {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]);
    const float ty = 2.0f * (z * v[0] - x * v[2]);
    const float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Shortest-arc spherical blend from `from` (weight 0) to `to` (weight 1),
// renormalised so a long ramp cannot drift off the unit sphere.
void blend_quat(const float *from, const float *to, float weight, float *out) noexcept {
    float target[4] = {to[0], to[1], to[2], to[3]};
    float dot = from[0] * target[0] + from[1] * target[1] + from[2] * target[2] + from[3] * target[3];
    if (dot < 0.0f) {
        for (float &value : target) value = -value;
        dot = -dot;
    }
    const float t = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    if (dot > 0.9995f) {
        for (int i = 0; i < 4; ++i) out[i] = from[i] + (target[i] - from[i]) * t;
    } else {
        const float theta = std::acos(dot < -1.0f ? -1.0f : (dot > 1.0f ? 1.0f : dot));
        const float sin_theta = std::sin(theta);
        const float wa = std::sin((1.0f - t) * theta) / sin_theta;
        const float wb = std::sin(t * theta) / sin_theta;
        for (int i = 0; i < 4; ++i) out[i] = from[i] * wa + target[i] * wb;
    }
    const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (length > 1e-6f) for (int i = 0; i < 4; ++i) out[i] /= length;
}

void write_fields(std::uintptr_t destination, const float *frame) noexcept {
    auto *dst = reinterpret_cast<std::uint8_t *>(destination);
    std::memcpy(dst + offset_scale, frame + 0, 12);                 // scale.xyz, spare float left alone
    std::memcpy(dst + offset_quat, frame + 3, 16);                  // quat.xyzw
    std::memcpy(dst + offset_pos, frame + 7, 12);                   // pos.xyz, spare float left alone
}

void write_quat(std::uintptr_t buffer, std::uint32_t joint, const float quat[4]) noexcept {
    auto *dst = reinterpret_cast<std::uint8_t *>(buffer) + static_cast<std::size_t>(joint) * pose_stride +
                offset_quat;
    std::memcpy(dst, quat, 16); // the only field a rotation solve writes
}

// A joint's composed world frame. The floor solve works in world space (the
// floor is a world height) but writes parent-local rotations back, so it
// needs both halves of every joint on the chain.
struct Frame {
    float pos[3];
    float quat[4];
};
constexpr std::size_t max_chain = 12; // the longest limb chain here is 10

// Compose every joint on a chain, root first: the parent's world rotation
// carries the child's local translation into world space, the same walk
// world_position does, but keeping the rotations as well.
bool compose_chain(std::uintptr_t buffer, const std::uint32_t *chain, std::size_t count, Frame *out) noexcept {
    if (!buffer || !chain || count == 0 || count > max_chain) return false;
    float scale[3]{}, world_quat[4]{}, pos[3]{};
    joint_fields(buffer, chain[0], scale, world_quat, pos);
    out[0].pos[0] = pos[0];
    out[0].pos[1] = pos[1];
    out[0].pos[2] = pos[2];
    std::memcpy(out[0].quat, world_quat, sizeof(world_quat));
    float accumulated = (scale[0] + scale[1] + scale[2]) / 3.0f;
    for (std::size_t i = 1; i < count; ++i) {
        float local_scale[3]{}, local_quat[4]{}, local_pos[3]{};
        joint_fields(buffer, chain[i], local_scale, local_quat, local_pos);
        const float scaled[3] = {local_pos[0] * accumulated, local_pos[1] * accumulated,
                                 local_pos[2] * accumulated};
        float rotated[3]{};
        rotate(world_quat, scaled, rotated);
        out[i].pos[0] = out[i - 1].pos[0] + rotated[0];
        out[i].pos[1] = out[i - 1].pos[1] + rotated[1];
        out[i].pos[2] = out[i - 1].pos[2] + rotated[2];
        float next[4]{};
        multiply(world_quat, local_quat, next);
        std::memcpy(world_quat, next, sizeof(next));
        std::memcpy(out[i].quat, world_quat, sizeof(world_quat));
        accumulated *= (local_scale[0] + local_scale[1] + local_scale[2]) / 3.0f;
    }
    return true;
}

float length3(const float v[3]) noexcept {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void normalize3(float v[3]) noexcept {
    const float length = length3(v);
    if (length > 1e-8f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

// The rotation taking unit vector `a` onto unit vector `b`: the shortest arc,
// which is what keeps a knee pointing the way the clip pointed it.
void from_to(const float a[3], const float b[3], float out[4]) noexcept {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d > 0.999999f) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
    out[3] = 1.0f + d;
    if (d < -0.999999f) {
        // Opposite directions: the arc is 180 degrees and any axis perpendicular
        // to `a` will do, so take the one least aligned with it.
        const float ax = std::fabs(a[0]), ay = std::fabs(a[1]), az = std::fabs(a[2]);
        float axis[3] = {1.0f, 0.0f, 0.0f};
        if (ay <= ax && ay <= az) {
            axis[0] = 0.0f;
            axis[1] = 1.0f;
        } else if (az <= ax && az <= ay) {
            axis[0] = 0.0f;
            axis[1] = 0.0f;
            axis[2] = 1.0f;
        }
        out[0] = a[1] * axis[2] - a[2] * axis[1];
        out[1] = a[2] * axis[0] - a[0] * axis[2];
        out[2] = a[0] * axis[1] - a[1] * axis[0];
        out[3] = 0.0f;
    }
    const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (length > 1e-8f) {
        out[0] /= length;
        out[1] /= length;
        out[2] /= length;
        out[3] /= length;
    }
}

void conjugate(const float q[4], float out[4]) noexcept {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

void normalize_quat(float q[4]) noexcept {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length > 1e-8f) {
        q[0] /= length;
        q[1] /= length;
        q[2] /= length;
        q[3] /= length;
    }
}
} // namespace

bool joint_masked(std::uint32_t joint) noexcept {
    return joint < clip_first_joint || joint > clip_last_joint;
}

bool world_position(std::uintptr_t buffer, const std::uint32_t *chain, std::size_t count,
                    float out[3]) noexcept {
    Frame frames[max_chain];
    if (!compose_chain(buffer, chain, count, frames)) return false;
    out[0] = frames[count - 1].pos[0];
    out[1] = frames[count - 1].pos[1];
    out[2] = frames[count - 1].pos[2];
    return true;
}

float ramp_weight(float weight, float target, std::uint64_t elapsed_ms, std::uint64_t ramp_ms) noexcept {
    if (ramp_ms == 0) return target;
    const float step = static_cast<float>(elapsed_ms) / static_cast<float>(ramp_ms);
    if (weight < target) return weight + step < target ? weight + step : target;
    if (weight > target) return weight - step > target ? weight - step : target;
    return weight;
}

namespace {
// One joint: the clip's fields (or an interpolation of two clip frames) written
// over the pose, honouring the mask and the layer ramp.
void write_one(std::uintptr_t destination, const float *fields, bool masked, float keep) noexcept {
    if (!masked || keep <= 0.0f) {
        write_fields(destination, fields);
        return;
    }
    if (keep >= 1.0f) return; // the game keeps this joint outright
    // Mid-ramp: blend the buffer's own value -- the game's, written this frame --
    // toward the clip.
    auto *dst = reinterpret_cast<std::uint8_t *>(destination);
    float game_scale[3], game_quat[4], game_pos[3];
    std::memcpy(game_scale, dst + offset_scale, sizeof(game_scale));
    std::memcpy(game_quat, dst + offset_quat, sizeof(game_quat));
    std::memcpy(game_pos, dst + offset_pos, sizeof(game_pos));
    const float weight = 1.0f - keep;
    // Built in clip layout (scale, quat, pos) so it can go straight through the
    // same writer as an untouched joint.
    float value[clip_stride];
    for (int c = 0; c < 3; ++c) {
        value[c] = game_scale[c] * keep + fields[c] * weight;
        value[7 + c] = game_pos[c] * keep + fields[7 + c] * weight;
    }
    blend_quat(game_quat, fields + 3, weight, value + 3);
    write_fields(destination, value);
}
} // namespace

void write_pose(std::uintptr_t buffer, const float *frame, std::uint32_t first, std::uint32_t count,
                float keep) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto joint = first + i;
        write_one(buffer + static_cast<std::size_t>(i) * pose_stride,
                  frame + static_cast<std::size_t>(i) * clip_stride, joint_masked(joint), keep);
    }
}

void write_pose_interpolated(std::uintptr_t buffer, const float *frame_a, const float *frame_b, float alpha,
                             std::uint32_t first, std::uint32_t count, float keep) noexcept {
    const float t = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto joint = first + i;
        const auto *a = frame_a + static_cast<std::size_t>(i) * clip_stride;
        const auto *b = frame_b + static_cast<std::size_t>(i) * clip_stride;
        // A clip is authored at its own rate and the game runs at 60 Hz, so a
        // whole-frame snap reads as a stutter. Interpolate, and let the caller
        // pass the last frame and the first frame as the pair: that closes the
        // loop without a jump.
        float fields[clip_stride];
        for (int c = 0; c < 3; ++c) {
            fields[c] = a[c] + (b[c] - a[c]) * t;
            fields[7 + c] = a[7 + c] + (b[7 + c] - a[7 + c]) * t;
        }
        blend_quat(a + 3, b + 3, t, fields + 3);
        write_one(buffer + static_cast<std::size_t>(i) * pose_stride, fields, joint_masked(joint), keep);
    }
}

namespace {
// One two-bone limb, as indices into its composed chain: the root joint the
// solve rotates (hip, shoulder), the mid joint (knee, elbow) it also rotates,
// the end joint it drives (ankle, wrist), and for a leg the toe, whose height
// decides when the foot counts as sunk. Sides follow the skeleton's own
// geometry (README: right leg j008..j011, left leg j341..j344; right arm
// j046..j049, left arm j275..j278).
struct Limb {
    std::uint32_t chain[max_chain];
    std::size_t count;
    std::size_t root;
    std::size_t mid;
    std::size_t end;
    bool has_tip;
    std::size_t tip;
};

const Limb &right_leg() noexcept {
    static const Limb limb{{1, 7, 8, 9, 10, 11}, 6, 2, 3, 4, true, 5};
    return limb;
}
const Limb &left_leg() noexcept {
    static const Limb limb{{1, 7, 341, 342, 343, 344}, 6, 2, 3, 4, true, 5};
    return limb;
}
const Limb &right_arm() noexcept {
    static const Limb limb{{1, 7, 42, 43, 44, 45, 46, 47, 48, 49}, 10, 7, 8, 9, false, 0};
    return limb;
}
const Limb &left_arm() noexcept {
    static const Limb limb{{1, 7, 42, 43, 44, 45, 275, 276, 277, 278}, 10, 7, 8, 9, false, 0};
    return limb;
}
// The torso as one limb: the spine from joint 42 up through the neck to the
// head at 103. The pelvis below it is the game's, so hips cannot clip; a
// clip bent over or lying down can put the head or chest through the floor,
// and this is the chain that lifts them back. Root 42, mid 45 (upper chest),
// end 103 (head).
const Limb &torso() noexcept {
    static const Limb limb{{1, 7, 42, 43, 44, 45, 101, 102, 103}, 9, 2, 5, 8, false, 0};
    return limb;
}

// One analytic pass of the two-bone solve: lift `lift` meters (negative
// reaches down), aiming the end joint at the height it had plus the lift.
// Only the root's and mid's rotations are written -- bone lengths are
// rotation invariants, so the solve moves the end without stretching the
// limb, and every position, scale and spare float in the buffer stays as it
// was. `knee_hint`, when given, is the way the game's own knee bends (a unit
// world direction off the aim line, sampled before the clip wrote): a clip
// whose leg is dead straight carries no bend plane of its own -- the perp is
// float noise -- and a clip authored against a mirrored rig bends the knee
// backwards. In both cases the game's direction wins; a clip that clearly
// authored its own bend and agrees keeps it. Returns false when the geometry
// cannot move any further.
bool solve_limb_pass(std::uintptr_t buffer, const Limb &limb, float lift,
                     const float *knee_hint) noexcept {
    Frame frames[max_chain];
    if (!compose_chain(buffer, limb.chain, limb.count, frames)) return false;
    const float *root = frames[limb.root].pos;
    const float *mid = frames[limb.mid].pos;
    const float *end = frames[limb.end].pos;
    float bone1[3] = {mid[0] - root[0], mid[1] - root[1], mid[2] - root[2]};
    float bone2[3] = {end[0] - mid[0], end[1] - mid[1], end[2] - mid[2]};
    const float l1 = length3(bone1), l2 = length3(bone2);
    if (l1 < 1e-4f || l2 < 1e-4f) return false;
    float aim[3] = {end[0] - root[0], end[1] - root[1] + lift, end[2] - root[2]};
    const float reach = length3(aim);
    if (reach < 1e-5f) return false;
    // A target past full extension lands the end at full extension in the
    // target's direction: the closest the geometry gets.
    const float d_min = (l1 > l2 ? l1 - l2 : l2 - l1) + 1e-4f;
    const float d_max = l1 + l2 - 1e-4f;
    const float d = reach < d_min ? d_min : (reach > d_max ? d_max : reach);
    const float unit[3] = {aim[0] / reach, aim[1] / reach, aim[2] / reach};
    // The bend plane, taken from the current pose: how far the knee sits off
    // the aim line. A dead-straight limb has no plane of its own -- what
    // perp it has is float noise, which is how a straight-legged clip ends up
    // with the knee popping backwards. The game's own knee, sampled before
    // the clip wrote, is the fallback: it bends the way this body's knee
    // actually bends. A leg also borrows the direction its toe points (a knee
    // bends the way a foot points) and anything perpendicular will do for an
    // arm.
    const float along = bone1[0] * unit[0] + bone1[1] * unit[1] + bone1[2] * unit[2];
    float perp[3] = {bone1[0] - along * unit[0], bone1[1] - along * unit[1], bone1[2] - along * unit[2]};
    float perp_length = length3(perp);
    float hint[3]{}, hint_length = 0.0f;
    if (knee_hint) {
        const float hint_along = knee_hint[0] * unit[0] + knee_hint[1] * unit[1] + knee_hint[2] * unit[2];
        hint[0] = knee_hint[0] - hint_along * unit[0];
        hint[1] = knee_hint[1] - hint_along * unit[1];
        hint[2] = knee_hint[2] - hint_along * unit[2];
        hint_length = length3(hint);
    }
    // The clip's plane counts as authored when the knee sits clearly off the
    // aim line AND on the same side as the game's knee. Anything else -- a
    // straight leg, or a knee bent against the way the body's own knees bend
    // -- takes the game's direction instead. With no hint at all (arms, torso)
    // only degeneracy decides, the way it did before the knee hint existed.
    const bool has_hint = hint_length > 1e-3f;
    const bool authored = has_hint ? perp_length > 0.05f * l1 &&
                                          perp[0] * hint[0] + perp[1] * hint[1] + perp[2] * hint[2] > 0.0f
                                   : perp_length > 1e-3f;
    if (!authored && has_hint) {
        perp[0] = hint[0];
        perp[1] = hint[1];
        perp[2] = hint[2];
        perp_length = hint_length;
    }
    if (perp_length < 1e-3f && limb.has_tip) {
        const float *tip = frames[limb.tip].pos;
        const float toe[3] = {tip[0] - end[0], tip[1] - end[1], tip[2] - end[2]};
        const float toe_along = toe[0] * unit[0] + toe[1] * unit[1] + toe[2] * unit[2];
        perp[0] = toe[0] - toe_along * unit[0];
        perp[1] = toe[1] - toe_along * unit[1];
        perp[2] = toe[2] - toe_along * unit[2];
        perp_length = length3(perp);
    }
    if (perp_length < 1e-3f) {
        // Straight limb with no toe to borrow a plane from (an arm): cross the
        // aim line with whichever world axis is least aligned with it.
        const float ex[3] = {1.0f, 0.0f, 0.0f};
        const float ey[3] = {0.0f, 1.0f, 0.0f};
        const float *axis = std::fabs(unit[0]) < std::fabs(unit[1]) ? ex : ey;
        perp[0] = unit[1] * axis[2] - unit[2] * axis[1];
        perp[1] = unit[2] * axis[0] - unit[0] * axis[2];
        perp[2] = unit[0] * axis[1] - unit[1] * axis[0];
        perp_length = length3(perp);
    }
    if (perp_length < 1e-6f) return false;
    // Law of cosines onto the circle of radius l1 about the root and l2 about
    // the target: where the knee goes.
    const float a = (l1 * l1 - l2 * l2 + d * d) / (2.0f * d);
    float h_sq = l1 * l1 - a * a;
    if (h_sq < 0.0f) h_sq = 0.0f;
    const float h = std::sqrt(h_sq);
    perp[0] /= perp_length;
    perp[1] /= perp_length;
    perp[2] /= perp_length;
    const float knee[3] = {root[0] + unit[0] * a + perp[0] * h, root[1] + unit[1] * a + perp[1] * h,
                           root[2] + unit[2] * a + perp[2] * h};
    // Turn the root so its old bone direction aims at the new knee. Everything
    // below the root turned with it, which puts the mid joint exactly on the
    // new knee position and carries the end part of the way.
    normalize3(bone1);
    float knee_dir[3] = {knee[0] - root[0], knee[1] - root[1], knee[2] - root[2]};
    normalize3(knee_dir);
    float delta_root[4];
    from_to(bone1, knee_dir, delta_root);
    float root_world[4];
    multiply(delta_root, frames[limb.root].quat, root_world);
    float swung[3] = {end[0] - root[0], end[1] - root[1], end[2] - root[2]};
    float swung_rotated[3]{};
    rotate(delta_root, swung, swung_rotated);
    const float end_after[3] = {root[0] + swung_rotated[0], root[1] + swung_rotated[1],
                                root[2] + swung_rotated[2]};
    // Then the mid joint turns the shin or forearm onto the target. Both
    // directions span l2, so the rotation lands the end exactly on it.
    float bone2_after[3] = {end_after[0] - knee[0], end_after[1] - knee[1], end_after[2] - knee[2]};
    float target[3] = {root[0] + unit[0] * d, root[1] + unit[1] * d, root[2] + unit[2] * d};
    float target_dir[3] = {target[0] - knee[0], target[1] - knee[1], target[2] - knee[2]};
    normalize3(bone2_after);
    normalize3(target_dir);
    float delta_mid[4];
    from_to(bone2_after, target_dir, delta_mid);
    float mid_turned[4];
    multiply(delta_root, frames[limb.mid].quat, mid_turned);
    float mid_world[4];
    multiply(delta_mid, mid_turned, mid_world);
    // Back to parent-local, which is what the buffer holds. The root's parent
    // is the frame before it on the chain; the mid joint's parent is whatever
    // sits between -- the root itself for a leg or arm, but for the torso
    // chain (root 42, mid 45) the spine joints in between. Everything below
    // the root turned with it, so the mid's parent's new world is the root's
    // delta applied to its old one.
    float parent_inv[4];
    conjugate(frames[limb.root - 1].quat, parent_inv);
    float root_local[4];
    multiply(parent_inv, root_world, root_local);
    normalize_quat(root_local);
    float mid_parent[4];
    multiply(delta_root, frames[limb.mid - 1].quat, mid_parent);
    float mid_parent_inv[4];
    conjugate(mid_parent, mid_parent_inv);
    float mid_local[4];
    multiply(mid_parent_inv, mid_world, mid_local);
    normalize_quat(mid_local);
    write_quat(buffer, limb.chain[limb.root], root_local);
    write_quat(buffer, limb.chain[limb.mid], mid_local);
    return true;
}

// Drive a limb's lowest point to `goal` (a world Y), lifting or reaching
// down. One pass is exact for an arm (nothing hangs past the wrist), but a
// foot's toe rides with the shin: tilting the shin to move the ankle rotates
// the toe along with it, so a pass aimed at the ankle can leave the toe just
// short. Each pass moves what is still missing and stops when the gap
// closes or the geometry runs out of reach. `knee_hint` is the game's own
// knee direction or null; see solve_limb_pass.
void drive_to(std::uintptr_t buffer, const Limb &limb, float goal, const float *knee_hint) noexcept {
    Frame frames[max_chain];
    for (int pass = 0; pass < 3; ++pass) {
        if (!compose_chain(buffer, limb.chain, limb.count, frames)) return;
        float now = frames[limb.end].pos[1];
        if (limb.has_tip) now = std::min(now, frames[limb.tip].pos[1]);
        const float remaining = goal - now;
        if (std::fabs(remaining) <= 5e-4f) return; // close enough
        if (!solve_limb_pass(buffer, limb, remaining, knee_hint)) return; // out of reach
        Frame after[max_chain];
        if (!compose_chain(buffer, limb.chain, limb.count, after)) return;
        float moved = after[limb.end].pos[1];
        if (limb.has_tip) moved = std::min(moved, after[limb.tip].pos[1]);
        if (std::fabs(moved - now) < 1e-3f) return; // the solve is not gaining any more
    }
}

// Pitch the sole flat onto the game's planted-foot direction: rotate the
// ankle so the clip's ankle->toe vector takes the game's, blended by
// `strength`. Only the ankle's rotation is written, which swings the toe
// with it; the toe's height is then whatever it is, and the height passes in
// correct_leg put the foot back on the floor with the sole flat -- which is
// what holds the ankle at the height that keeps the toe on the ground.
void flatten_sole(std::uintptr_t buffer, const Limb &limb, const LegFloor &leg, float strength) noexcept {
    Frame frames[max_chain];
    if (!leg.toe_valid || strength <= 0.0f) return;
    if (!compose_chain(buffer, limb.chain, limb.count, frames)) return;
    const float *ankle = frames[limb.end].pos;
    const float *toe = frames[limb.tip].pos;
    float current[3] = {toe[0] - ankle[0], toe[1] - ankle[1], toe[2] - ankle[2]};
    if (length3(current) < 1e-4f) return;
    normalize3(current);
    // Blend the target direction by strength: full strength takes the game's
    // pitch outright, half leaves part of the authored tilt.
    float want[3];
    for (int c = 0; c < 3; ++c) want[c] = current[c] + (leg.toe_dir[c] - current[c]) * strength;
    if (length3(want) < 1e-4f) return;
    normalize3(want);
    float delta[4];
    from_to(current, want, delta);
    float ankle_world[4];
    multiply(delta, frames[limb.end].quat, ankle_world);
    float parent_inv[4];
    conjugate(frames[limb.end - 1].quat, parent_inv); // the shin: the ankle's parent
    float local[4];
    multiply(parent_inv, ankle_world, local);
    normalize_quat(local);
    write_quat(buffer, limb.chain[limb.end], local);
}

// One leg against its own sampled foot. `uneven` says the game's two feet sit
// at different heights -- the terrain itself is uneven (a ledge, a stair) --
// and only then may a floating foot reach DOWN to its surface: on flat
// ground a raised foot is the clip's authorship (a kick, a tuck) and is left
// alone. While the foot is near its floor the sole is also pitched flat onto
// the game's planted direction. Only rotations are written.
void correct_leg(std::uintptr_t buffer, const Limb &limb, const LegFloor &leg, float margin,
                 float strength, bool uneven) noexcept {
    if (strength <= 0.0f || !leg.valid) return;
    // How far above its floor a foot may still be followed down: past this it
    // is not floating over the ground under it, it is an authored air pose.
    constexpr float follow_band = 0.35f;
    Frame frames[max_chain];
    if (!compose_chain(buffer, limb.chain, limb.count, frames)) return;
    float lowest = frames[limb.end].pos[1];
    if (limb.has_tip) lowest = std::min(lowest, frames[limb.tip].pos[1]);
    // Where the foot's lowest point ends: the scaled share of the way to its
    // own floor (or down to it, on uneven ground). Decided once here, so
    // strength is applied a single time however many phases run -- the phases
    // only settle what the flatten moved around it.
    const float sink = leg.foot_y - lowest;  // >0: under its own floor
    const float above = lowest - leg.foot_y; // >0: floating above it
    bool drive = false;
    float target = 0.0f;
    if (sink > margin) {
        drive = true;
        target = lowest + sink * strength;
    } else if (uneven && above > margin && above < follow_band) {
        drive = true;
        target = lowest - above * strength;
    }
    for (int phase = 0; phase < 2; ++phase) {
        // Flatten the sole first while the foot is near its floor, then bring
        // the (new) lowest point to the height decided above: the ankle ends
        // at whatever keeps the toe on the ground.
        if (!compose_chain(buffer, limb.chain, limb.count, frames)) return;
        lowest = frames[limb.end].pos[1];
        if (limb.has_tip) lowest = std::min(lowest, frames[limb.tip].pos[1]);
        if (std::fabs(lowest - leg.foot_y) <= margin + 0.05f) flatten_sole(buffer, limb, leg, strength);
        if (!compose_chain(buffer, limb.chain, limb.count, frames)) return;
        lowest = frames[limb.end].pos[1];
        if (limb.has_tip) lowest = std::min(lowest, frames[limb.tip].pos[1]);
        if (drive) {
            drive_to(buffer, limb, target, leg.knee);
        } else if (lowest < leg.foot_y - margin) {
            // A planted foot whose flatten dipped the toe through the floor:
            // put it back on the surface. Nothing within the margin is chased.
            drive_to(buffer, limb, leg.foot_y, leg.knee);
        } else if (phase > 0) {
            return;
        }
    }
}

// Lift one limb until its lowest point clears the shared floor plane: the
// torso (spine and head -- the pelvis below is the game's, so hips are
// already safe) and the hands.
void correct_limb(std::uintptr_t buffer, const Limb &limb, float floor_y, float margin,
                  float strength) noexcept {
    if (strength <= 0.0f) return;
    Frame frames[max_chain];
    if (!compose_chain(buffer, limb.chain, limb.count, frames)) return;
    const float lowest = frames[limb.end].pos[1];
    const float sink = floor_y - lowest;
    if (sink <= margin) return; // within the tolerated sink: authored or noise
    drive_to(buffer, limb, lowest + sink * strength, nullptr);
}
} // namespace

bool sample_floors(std::uintptr_t buffer, FloorSample &out) noexcept {
    out = {};
    if (!buffer) return false;
    // The game's own feet, sampled before the clip overwrites the legs: per
    // foot, its lowest point (ankle or toe) is the surface that foot stands
    // on -- the board's top when riding, the ground when walking or standing,
    // the ledge under one foot and the drop under the other when the stance
    // straddles an edge. The engine's gesture IK already solved foot
    // placement into this pose; this only reads its answer, along with the
    // way each knee bends (the fallback bend plane for a clip whose leg is
    // straight or mirrored) and each sole's pitch.
    constexpr std::uint32_t chains[2][6] = {{1, 7, 8, 9, 10, 11}, {1, 7, 341, 342, 343, 344}};
    Frame frames[max_chain];
    bool floored = false;
    for (int side = 0; side < 2; ++side) {
        LegFloor &leg = out.legs[side];
        if (!compose_chain(buffer, chains[side], 6, frames)) continue;
        const float *hip = frames[2].pos;
        const float *knee = frames[3].pos;
        const float *ankle = frames[4].pos;
        const float *toe = frames[5].pos;
        leg.valid = true;
        leg.foot_y = std::min(ankle[1], toe[1]);
        leg.ankle_offset = ankle[1] - leg.foot_y;
        // How far the knee sits off the hip->ankle line: the way this body's
        // knee bends, in world space.
        float bone1[3] = {knee[0] - hip[0], knee[1] - hip[1], knee[2] - hip[2]};
        float aim[3] = {ankle[0] - hip[0], ankle[1] - hip[1], ankle[2] - hip[2]};
        if (length3(aim) > 1e-4f && length3(bone1) > 1e-4f) {
            normalize3(aim);
            const float along = bone1[0] * aim[0] + bone1[1] * aim[1] + bone1[2] * aim[2];
            float perp[3] = {bone1[0] - along * aim[0], bone1[1] - along * aim[1], bone1[2] - along * aim[2]};
            if (length3(perp) > 1e-4f) {
                normalize3(perp);
                leg.knee[0] = perp[0];
                leg.knee[1] = perp[1];
                leg.knee[2] = perp[2];
            }
        }
        float toe_dir[3] = {toe[0] - ankle[0], toe[1] - ankle[1], toe[2] - ankle[2]};
        if (length3(toe_dir) > 1e-4f) {
            normalize3(toe_dir);
            leg.toe_valid = true;
            leg.toe_dir[0] = toe_dir[0];
            leg.toe_dir[1] = toe_dir[1];
            leg.toe_dir[2] = toe_dir[2];
        }
        out.floor_y = floored ? std::min(out.floor_y, leg.foot_y) : leg.foot_y;
        floored = true;
    }
    out.valid = out.legs[0].valid && out.legs[1].valid;
    return out.valid;
}

FloorSample smooth_floor(FloorSample state, const FloorSample &fresh, float alpha) noexcept {
    // Nothing to smooth against yet: the first sample is taken outright, and a
    // clip that has just started gets the raw answer rather than a slow fade
    // in from nothing.
    if (!state.valid) return fresh;
    // The fresh read failed to compose (the skater is mid-bail, the graph is
    // resetting): hold the last good sample rather than dropping the floor out
    // from under the correction for a frame.
    if (!fresh.valid) return state;
    if (!(alpha > 0.0f)) alpha = 0.0f;
    else if (alpha > 1.0f) alpha = 1.0f;
    const auto blend = [alpha](float old_value, float new_value) {
        return old_value + (new_value - old_value) * alpha;
    };
    FloorSample out = fresh;  // the flags come from the fresh read...
    out.floor_y = blend(state.floor_y, fresh.floor_y);  // ...but the numbers are smoothed
    for (int side = 0; side < 2; ++side) {
        const LegFloor &old_leg = state.legs[side];
        LegFloor &out_leg = out.legs[side];
        // A leg that did not compose this frame keeps the smoothed one; a leg
        // that never had a prior value keeps the fresh one (handled by the
        // `state.valid` guard above).
        if (!out_leg.valid || !old_leg.valid) continue;
        out_leg.foot_y = blend(old_leg.foot_y, out_leg.foot_y);
        out_leg.ankle_offset = blend(old_leg.ankle_offset, out_leg.ankle_offset);
        for (int axis = 0; axis < 3; ++axis) {
            out_leg.knee[axis] = blend(old_leg.knee[axis], out_leg.knee[axis]);
            out_leg.toe_dir[axis] = blend(old_leg.toe_dir[axis], out_leg.toe_dir[axis]);
        }
        // A lerp of two unit vectors is not unit; renormalise, and if the two
        // samples opposed so completely that the sum collapsed (a hint flipping
        // 180 degrees, which a healthy knee never does), keep the fresh one.
        if (length3(out_leg.knee) > 1e-3f) normalize3(out_leg.knee);
        else { out_leg.knee[0] = fresh.legs[side].knee[0]; out_leg.knee[1] = fresh.legs[side].knee[1]; out_leg.knee[2] = fresh.legs[side].knee[2]; }
        if (length3(out_leg.toe_dir) > 1e-3f) normalize3(out_leg.toe_dir);
        else { out_leg.toe_dir[0] = fresh.legs[side].toe_dir[0]; out_leg.toe_dir[1] = fresh.legs[side].toe_dir[1]; out_leg.toe_dir[2] = fresh.legs[side].toe_dir[2]; }
    }
    return out;
}

void apply_floor(std::uintptr_t buffer, const FloorSample &floor, float margin, float strength,
                 float keep) noexcept {
    if (!buffer || !floor.valid || strength <= 0.0f) return;
    // Legs: only the part the clip owns. A leg the mask kept is the game's own
    // planted foot -- the very thing the floor was sampled from -- so the
    // correction fades out with the clip's share of the leg.
    const float leg_strength = keep >= 1.0f ? 0.0f : strength * (1.0f - keep);
    if (leg_strength > 0.0f) {
        // The game's own two feet at different heights is the one signal that
        // the terrain itself is uneven: only then may a floating foot reach
        // down to the surface under it.
        const bool uneven = floor.legs[0].valid && floor.legs[1].valid &&
                            std::fabs(floor.legs[0].foot_y - floor.legs[1].foot_y) > margin;
        correct_leg(buffer, right_leg(), floor.legs[0], margin, leg_strength, uneven);
        correct_leg(buffer, left_leg(), floor.legs[1], margin, leg_strength, uneven);
    }
    // Torso: a clip bent over or lying down can put the spine, chest or head
    // through the floor. The pelvis is the game's, so the hips are already
    // safe; this lifts the spine+head chain clear of the floor plane.
    correct_limb(buffer, torso(), floor.floor_y, margin, strength);
    // Hands: always the clip's. A hand resting on the floor is a handplant and
    // stays where the clip put it; only one below the floor is a bug, so the
    // clearance here is a hair, not the legs' margin.
    constexpr float hand_clearance = 0.005f;
    correct_limb(buffer, right_arm(), floor.floor_y, hand_clearance, strength);
    correct_limb(buffer, left_arm(), floor.floor_y, hand_clearance, strength);
}

} // namespace dingosdk::skater::layers
