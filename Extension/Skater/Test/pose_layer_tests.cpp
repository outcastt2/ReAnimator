// Host exercise of the pose layering, against memory owned by this process.
// No game, no hooks: the point is that an off-by-one in the mapping between a
// clip's joints and the engine's pose buffer silently scrambles the skater, so
// the mapping is checked joint by joint, including the two reserved joints at
// the front and the spare floats that must never be touched.
#include "Extension/Skater/pose_layers.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dingosdk::skater::layers;

namespace {
constexpr std::uint32_t pose_joints = 395;
constexpr std::size_t pose_bytes = pose_joints * pose_stride;

void check(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}

float field(std::uintptr_t buffer, std::uint32_t joint, std::size_t offset) {
    float value{};
    std::memcpy(&value, reinterpret_cast<const void *>(buffer + joint * pose_stride + offset), sizeof(value));
    return value;
}

// A single byte, for the places that must not be written at all: sentinel bytes
// are not a number a float comparison can be trusted with.
bool untouched(std::uintptr_t buffer, std::uint32_t joint, std::size_t offset) {
    unsigned char value{};
    std::memcpy(&value, reinterpret_cast<const void *>(buffer + joint * pose_stride + offset), 1);
    return value == 0xff;
}

std::vector<float> make_frame(std::uint32_t joints) {
    std::vector<float> frame(static_cast<std::size_t>(joints) * clip_stride, 0.0f);
    for (std::uint32_t joint = 0; joint < joints; ++joint) {
        float *at = frame.data() + static_cast<std::size_t>(joint) * clip_stride;
        at[0] = 10.0f * joint; at[1] = 20.0f * joint; at[2] = 30.0f * joint;
        at[3] = 0.0f; at[4] = 0.0f; at[5] = 0.0f; at[6] = 1.0f;   // identity
        at[7] = 100.0f * joint; at[8] = 200.0f * joint; at[9] = 300.0f * joint;
    }
    return frame;
}

// Every byte of the pose buffer, so untouched joints and spare floats stand out.
std::vector<std::byte> sentinel_pose() {
    return std::vector<std::byte>(pose_bytes, std::byte{0xff});
}

void check_clip_written(const std::vector<std::byte> &pose, std::uint32_t joint, const std::string &what) {
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    check(std::abs(field(address, joint, 0x00) - 10.0f * joint) < 1e-3f, what + ": scale x at joint " + std::to_string(joint));
    check(std::abs(field(address, joint, 0x08) - 30.0f * joint) < 1e-3f, what + ": scale z at joint " + std::to_string(joint));
    check(std::abs(field(address, joint, 0x20) - 100.0f * joint) < 1e-3f, what + ": pos x at joint " + std::to_string(joint));
    check(std::abs(field(address, joint, 0x28) - 300.0f * joint) < 1e-3f, what + ": pos z at joint " + std::to_string(joint));
    check(std::abs(field(address, joint, 0x14) - 0.0f) < 1e-3f && std::abs(field(address, joint, 0x1c) - 1.0f) < 1e-3f,
          what + ": quaternion at joint " + std::to_string(joint));
    // The spare float after each vector is not part of a pose.
    check(untouched(reinterpret_cast<std::uintptr_t>(pose.data()), joint, 0x0c), what + ": scale spare float at joint " + std::to_string(joint));
    check(untouched(reinterpret_cast<std::uintptr_t>(pose.data()), joint, 0x2c), what + ": pos spare float at joint " + std::to_string(joint));
}

void test_masked_ranges() {
    check(!joint_masked(0) && !joint_masked(1) && !joint_masked(2), "joints 0..2 are not masked");
    check(joint_masked(7), "the pelvis is masked");
    check(joint_masked(8) && joint_masked(41), "the whole left leg is masked");
    check(!joint_masked(42) && !joint_masked(340), "neither the spine nor the last torso helper is masked");
    check(joint_masked(341) && joint_masked(374), "the whole right leg is masked");
    check(!joint_masked(375) && !joint_masked(394), "the helper tail is not masked");
}

void test_whole_body() {
    // keep = 0: the clip drives everything from joint 2 up.
    auto pose = sentinel_pose();
    const auto frame = make_frame(pose_joints);
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    write_pose(address + 2 * pose_stride, frame.data() + 2 * clip_stride, 2, pose_joints - 2, 0.0f);
    for (std::uint32_t joint = 2; joint < pose_joints; ++joint)
        check_clip_written(pose, joint, "whole body");
    // Joints 0 and 1 carry world placement and are never written.
    check(untouched(address, 0, 0x00) && untouched(address, 1, 0x20), "joints 0 and 1 are left alone");
}

void test_masked_body() {
    // keep = 1: the game keeps legs and pelvis, the clip still has the rest.
    auto pose = sentinel_pose();
    const auto frame = make_frame(pose_joints);
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    write_pose(address + 2 * pose_stride, frame.data() + 2 * clip_stride, 2, pose_joints - 2, 1.0f);
    for (std::uint32_t joint = 2; joint < pose_joints; ++joint) {
        if (joint_masked(joint)) {
            check(untouched(address, joint, 0x00) && untouched(address, joint, 0x20),
                  "masked joint " + std::to_string(joint) + " is untouched");
        } else {
            check_clip_written(pose, joint, "masked body");
        }
    }
}

// Every joint holding a known game pose, so a blend has something to blend from.
std::vector<std::byte> game_pose() {
    auto pose = sentinel_pose();
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    for (std::uint32_t joint = 0; joint < pose_joints; ++joint) {
        const float scale = 4.0f, position = 8.0f;
        const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x00), &scale, sizeof(scale));
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x10), quat, sizeof(quat));
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x20), &position, sizeof(position));
    }
    return pose;
}

void test_blend() {
    // A half blend between a known game pose and the clip.
    auto pose = game_pose();
    const auto frame = make_frame(pose_joints);
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    write_pose(address + 2 * pose_stride, frame.data() + 2 * clip_stride, 2, pose_joints - 2, 0.5f);
    // Unmasked joints still take the clip outright.
    check_clip_written(pose, 42, "blend");
    // A masked joint sits halfway between the game's value and the clip's.
    const std::uint32_t masked = 20;
    const float expected_scale = 0.5f * 4.0f + 0.5f * 10.0f * masked;
    const float expected_pos = 0.5f * 8.0f + 0.5f * 100.0f * masked;
    check(std::abs(field(address, masked, 0x00) - expected_scale) < 1e-3f, "blended scale");
    check(std::abs(field(address, masked, 0x20) - expected_pos) < 1e-3f, "blended position");
    const float length = std::sqrt(field(address, masked, 0x10) * field(address, masked, 0x10) +
                                   field(address, masked, 0x1c) * field(address, masked, 0x1c));
    check(std::abs(length - 1.0f) < 1e-3f, "blended quaternion stays unit length");
    // The spare floats are still nobody's business.
    check(untouched(address, masked, 0x0c) && untouched(address, masked, 0x2c), "blend leaves spare floats");
    // The ramp moves toward its target and clamps at both ends.
    check(ramp_weight(0.0f, 1.0f, 100, 200) == 0.5f, "ramp half way");
    check(ramp_weight(0.9f, 1.0f, 100, 200) == 1.0f, "ramp clamps at one");
    check(ramp_weight(0.1f, 0.0f, 500, 200) == 0.0f, "ramp clamps at zero");
}
} // namespace

int main() {
    try {
        test_masked_ranges();
        test_whole_body();
        test_masked_body();
        test_blend();
        std::cout << "pose layer tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << "pose layer tests FAILED: " << error.what() << "\n";
        return 1;
    }
}
