#include "pose_layers.h"
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
} // namespace

bool joint_masked(std::uint32_t joint) noexcept {
    return joint < clip_first_joint || joint > clip_last_joint;
}

bool world_position(std::uintptr_t buffer, const std::uint32_t *chain, std::size_t count,
                    float out[3]) noexcept {
    if (!buffer || !chain || count == 0) return false;
    // The chain is walked root first, carrying the parent's world rotation and
    // scale down with it: a joint's local translation is expressed in its
    // parent's frame, so it is the parent's *world* frame that places it.
    float scale[3]{}, world_quat[4]{}, pos[3]{};
    joint_fields(buffer, chain[0], scale, world_quat, pos);
    out[0] = pos[0];
    out[1] = pos[1];
    out[2] = pos[2];
    float accumulated = (scale[0] + scale[1] + scale[2]) / 3.0f;
    for (std::size_t i = 1; i < count; ++i) {
        float local_scale[3]{}, local_quat[4]{}, local_pos[3]{};
        joint_fields(buffer, chain[i], local_scale, local_quat, local_pos);
        const float scaled[3] = {local_pos[0] * accumulated, local_pos[1] * accumulated,
                                 local_pos[2] * accumulated};
        float rotated[3]{};
        rotate(world_quat, scaled, rotated);
        out[0] += rotated[0];
        out[1] += rotated[1];
        out[2] += rotated[2];
        float next[4]{};
        multiply(world_quat, local_quat, next);
        std::memcpy(world_quat, next, sizeof(next));
        accumulated *= (local_scale[0] + local_scale[1] + local_scale[2]) / 3.0f;
    }
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

} // namespace dingosdk::skater::layers
