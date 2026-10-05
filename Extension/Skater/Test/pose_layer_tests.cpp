// Host exercise of the pose layering, against memory owned by this process.
// No game, no hooks: the point is that an off-by-one in the mapping between a
// clip's joints and the engine's pose buffer silently scrambles the skater, so
// the mapping is checked joint by joint, including the two reserved joints at
// the front and the spare floats that must never be touched.
#include "Extension/Skater/pose_layers.h"
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
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

// A joint's world position relative to joint 1, the same way the runtime
// diagnostic does it.
bool pose_offset_for_test(std::uintptr_t buffer, const std::uint32_t *chain, std::size_t count, float out[3]) {
    const std::uint32_t root[] = {1};
    float origin[3]{}, world[3]{};
    if (!world_position(buffer, root, 1, origin)) return false;
    if (!world_position(buffer, chain, count, world)) return false;
    for (int c = 0; c < 3; ++c) out[c] = world[c] - origin[c];
    return true;
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
    // The clip owns the body above the pelvis: the spine at joint 42 and
    // everything it parents, 42..340.
    check(!joint_masked(42) && !joint_masked(100) && !joint_masked(340), "the clip owns 42..340");
    // The engine keeps the world placement, its helper clusters, the pelvis and
    // both legs.
    check(joint_masked(1), "the world placement is the game's");
    check(joint_masked(2) && joint_masked(6), "the root helper cluster is the game's");
    check(joint_masked(7), "the pelvis is the game's");
    check(joint_masked(8) && joint_masked(41), "the whole left leg is the game's");
    check(joint_masked(341) && joint_masked(374), "the whole right leg is the game's");
    check(joint_masked(375) && joint_masked(394), "the tail helper cluster is the game's");
}

void test_world_position() {
    // A tiny pose: joint 1 at the origin, joint 7 two units down, joint 8 one
    // unit further along the pelvis's own frame, which is rotated 90 degrees
    // about x so its local +y points along world -z.
    auto pose = sentinel_pose();
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto put = [&](std::uint32_t joint, float x, float y, float z, float qx, float qy, float qz,
                         float qw, float scale) {
        const float position[3] = {x, y, z};
        const float quat[4] = {qx, qy, qz, qw};
        const float scales[3] = {scale, scale, scale};
        auto *at = reinterpret_cast<void *>(address + joint * pose_stride);
        std::memcpy(at, scales, sizeof(scales));
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x10), quat, sizeof(quat));
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x20), position, sizeof(position));
    };
    // 90 degrees about x: (x, y, z) -> (x, -z, y).
    const float half = 0.70710678f;
    put(1, 5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    put(7, 0.0f, -2.0f, 0.0f, half, 0.0f, 0.0f, half, 1.0f);
    put(8, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    const std::uint32_t chain[] = {1, 7, 8};
    float out[3]{};
    check(world_position(address, chain, 3, out), "the chain composes");
    check(std::abs(out[0] - 5.0f) < 1e-3f, "composed x follows the root");
    // joint 7 sits 2 below the root; joint 8's own -1 along y is rotated by the
    // pelvis frame to world -z.
    check(std::abs(out[1] - (-2.0f)) < 1e-3f, "composed y is the pelvis offset");
    check(std::abs(out[2] - (-1.0f)) < 1e-3f, "composed z is the rotated chain");
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
void test_against_a_real_clip() {
    // The composition has to agree with the add-on's verified composer, or the
    // live diagnostic lies. stance.rska is the game's own standing pose on the
    // board: its left ankle sits about 0.86 m below the world placement joint.
    constexpr const char *path = R"(D:\Games\reSkate\Skate\CustomAnimations\stance.rska)";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cout << "     (no stance.rska; skipped)\n";
        return;
    }
    std::array<char, 20> header{};
    in.read(header.data(), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size()) || std::memcmp(header.data(), "RSKA", 4) != 0)
        throw std::runtime_error("stance.rska header");
    std::uint32_t joints = 0, frames = 0;
    std::memcpy(&joints, header.data() + 8, 4);
    std::memcpy(&frames, header.data() + 12, 4);
    check(joints == pose_joints && frames > 0, "stance.rska shape");
    std::vector<float> values(static_cast<std::size_t>(frames) * joints * clip_stride);
    in.read(reinterpret_cast<char *>(values.data()),
            static_cast<std::streamsize>(values.size() * sizeof(float)));
    if (in.gcount() != static_cast<std::streamsize>(values.size() * sizeof(float)))
        throw std::runtime_error("stance.rska body");

    // Re-pack the clip into a pose buffer: the layout is already the pose's.
    std::vector<std::byte> pose(pose_bytes);
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    for (std::uint32_t joint = 0; joint < joints; ++joint) {
        const float *src = values.data() + static_cast<std::size_t>(joint) * clip_stride;
        auto *dst = reinterpret_cast<void *>(address + joint * pose_stride);
        std::memcpy(dst, src + 0, 12);
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x10), src + 3, 16);
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x20), src + 7, 12);
    }
    const std::uint32_t pelvis_chain[] = {1, 7};
    const std::uint32_t left_chain[] = {1, 7, 8, 9, 10};
    float pelvis[3]{}, left[3]{};
    check(pose_offset_for_test(address, pelvis_chain, 2, pelvis), "pelvis composes");
    check(pose_offset_for_test(address, left_chain, 5, left), "ankle composes");
    std::cout << "     pose: pelvis_y=" << pelvis[1] << " L_ankle=(" << left[0] << ", " << left[1] << ", "
              << left[2] << ")   reference: pelvis_y=-0.031 L_ankle=(-0.076, -0.860, -0.147)\n";
    check(std::abs(pelvis[1] - (-0.031f)) < 0.02f, "pelvis height matches the reference");
    check(std::abs(left[1] - (-0.860f)) < 0.02f, "ankle height matches the reference");
    check(std::abs(left[2] - (-0.147f)) < 0.02f, "ankle side matches the reference");
}
void test_interpolation() {
    // Two clip frames and a quarter of the way between them: the pose must be
    // the blend, not either frame, and a rotation must travel the short arc.
    const float quarter = 0.25f;
    std::vector<float> a(static_cast<std::size_t>(pose_joints) * clip_stride, 0.0f);
    std::vector<float> b(static_cast<std::size_t>(pose_joints) * clip_stride, 0.0f);
    for (std::uint32_t joint = 0; joint < pose_joints; ++joint) {
        float *x = a.data() + static_cast<std::size_t>(joint) * clip_stride;
        float *y = b.data() + static_cast<std::size_t>(joint) * clip_stride;
        x[0] = x[1] = x[2] = 2.0f;
        x[6] = 1.0f; // identity rotation
        y[0] = y[1] = y[2] = 4.0f;
        y[5] = 0.70710678f; // 90 degrees about z
        y[6] = 0.70710678f;
        y[7] = y[8] = y[9] = 10.0f;
    }
    auto pose = sentinel_pose();
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    write_pose_interpolated(address + 2 * pose_stride, a.data() + 2 * clip_stride, b.data() + 2 * clip_stride,
                            quarter, 2, pose_joints - 2, 0.0f);
    check(std::abs(field(address, 42, 0x00) - 2.5f) < 1e-3f, "interpolated scale");
    check(std::abs(field(address, 42, 0x20) - 2.5f) < 1e-3f, "interpolated position");
    // A quarter of 90 degrees about z is 22.5: z = sin(11.25), w = cos(11.25).
    check(std::abs(field(address, 42, 0x18) - 0.19509f) < 1e-3f, "interpolated rotation");
    check(std::abs(field(address, 42, 0x1c) - 0.98079f) < 1e-3f, "interpolated rotation w");
    // The mask still applies: at full keep the game's pose survives untouched.
    auto kept = game_pose();
    const auto kept_address = reinterpret_cast<std::uintptr_t>(kept.data());
    write_pose_interpolated(kept_address + 2 * pose_stride, a.data() + 2 * clip_stride, b.data() + 2 * clip_stride,
                            quarter, 2, pose_joints - 2, 1.0f);
    check(std::abs(field(kept_address, 7, 0x00) - 4.0f) < 1e-3f, "a masked joint keeps the game's pose");
    check(std::abs(field(kept_address, 42, 0x00) - 2.5f) < 1e-3f, "an unmasked joint still interpolates");
}
} // namespace

int main() {
    try {
        test_masked_ranges();
        test_world_position();
        test_against_a_real_clip();
        test_whole_body();
        test_masked_body();
        test_blend();
        test_interpolation();
        std::cout << "pose layer tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << "pose layer tests FAILED: " << error.what() << "\n";
        return 1;
    }
}
