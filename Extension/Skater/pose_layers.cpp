#include "pose_layers.h"
#include <cmath>
#include <cstring>

namespace dingosdk::skater::layers {
namespace {
constexpr std::uint32_t hip_joint = 7;
constexpr std::uint32_t left_leg_first = 8, left_leg_last = 41;
constexpr std::uint32_t right_leg_first = 341, right_leg_last = 374;

constexpr std::size_t offset_scale = 0x00;
constexpr std::size_t offset_quat = 0x10;
constexpr std::size_t offset_pos = 0x20;

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
    return joint == hip_joint ||
           (joint >= left_leg_first && joint <= left_leg_last) ||
           (joint >= right_leg_first && joint <= right_leg_last);
}

float ramp_weight(float weight, float target, std::uint64_t elapsed_ms, std::uint64_t ramp_ms) noexcept {
    if (ramp_ms == 0) return target;
    const float step = static_cast<float>(elapsed_ms) / static_cast<float>(ramp_ms);
    if (weight < target) return weight + step < target ? weight + step : target;
    if (weight > target) return weight - step > target ? weight - step : target;
    return weight;
}

void write_pose(std::uintptr_t buffer, const float *frame, std::uint32_t first, std::uint32_t count,
                float keep) noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto joint = first + i;
        const auto *src = frame + static_cast<std::size_t>(i) * clip_stride;
        const auto destination = buffer + static_cast<std::size_t>(i) * pose_stride;
        if (!joint_masked(joint) || keep <= 0.0f) {
            write_fields(destination, src);
            continue;
        }
        if (keep >= 1.0f) continue; // the game keeps this joint outright
        // Mid-ramp: blend the buffer's own value -- the game's, written this
        // frame -- toward the clip.
        auto *dst = reinterpret_cast<std::uint8_t *>(destination);
        float game_scale[3], game_quat[4], game_pos[3];
        std::memcpy(game_scale, dst + offset_scale, sizeof(game_scale));
        std::memcpy(game_quat, dst + offset_quat, sizeof(game_quat));
        std::memcpy(game_pos, dst + offset_pos, sizeof(game_pos));
        const float weight = 1.0f - keep;
        // Built in clip layout (scale, quat, pos) so it can go straight through
        // the same writer as an untouched joint.
        float value[clip_stride];
        for (int c = 0; c < 3; ++c) {
            value[c] = game_scale[c] * keep + src[c] * weight;
            value[7 + c] = game_pos[c] * keep + src[7 + c] * weight;
        }
        blend_quat(game_quat, src + 3, weight, value + 3);
        write_fields(destination, value);
    }
}

} // namespace dingosdk::skater::layers
