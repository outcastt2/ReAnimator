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
// A standing pose with identity rotations everywhere, so composed world
// positions are the plain sum of the local ones. The right foot's sole (its
// toe) sits at y = -0.95 and both wrists hang at about -0.8.
void put(std::uintptr_t address, std::uint32_t joint, float x, float y, float z) {
    const float scale[3] = {1.0f, 1.0f, 1.0f};
    const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float pos[3] = {x, y, z};
    std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x00), scale, sizeof(scale));
    std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x10), quat, sizeof(quat));
    std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x20), pos, sizeof(pos));
}
std::vector<std::byte> standing_pose() {
    auto pose = sentinel_pose();
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto put = [&](std::uint32_t joint, float x, float y, float z) {
        const float scale[3] = {1.0f, 1.0f, 1.0f};
        const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float pos[3] = {x, y, z};
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x00), scale, sizeof(scale));
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x10), quat, sizeof(quat));
        std::memcpy(reinterpret_cast<void *>(address + joint * pose_stride + 0x20), pos, sizeof(pos));
    };
    for (std::uint32_t joint = 0; joint < pose_joints; ++joint) put(joint, 0.0f, 0.0f, 0.0f);
    put(7, 0.0f, -0.1f, 0.0f); // pelvis
    // Right leg j8..j11: hip, knee (bent forward), ankle, toe.
    put(8, 0.1f, 0.0f, 0.0f);
    put(9, 0.0f, -0.4f, 0.05f);
    put(10, 0.0f, -0.4f, -0.05f);
    put(11, 0.0f, -0.05f, 0.1f);
    // Left leg j341..j344, mirrored.
    put(341, -0.1f, 0.0f, 0.0f);
    put(342, 0.0f, -0.4f, 0.05f);
    put(343, 0.0f, -0.4f, -0.05f);
    put(344, 0.0f, -0.05f, 0.1f);
    // Right arm j46..j49: clavicle, shoulder, elbow, wrist.
    put(47, 0.2f, -0.1f, 0.0f);
    put(48, 0.0f, -0.3f, 0.0f);
    put(49, 0.0f, -0.3f, 0.05f);
    // Left arm j275..j278, mirrored.
    put(276, -0.2f, -0.1f, 0.0f);
    put(277, 0.0f, -0.3f, 0.0f);
    put(278, 0.0f, -0.3f, 0.05f);
    return pose;
}

// Sink a foot the way a clip authored against a different floor does: a
// deeper bend with the ankle below where the game planted it.
void sink_right_foot(std::uintptr_t address) {
    const float knee[3] = {0.0f, -0.45f, 0.02f};
    const float ankle[3] = {0.0f, -0.45f, -0.02f};
    std::memcpy(reinterpret_cast<void *>(address + 9 * pose_stride + 0x20), knee, sizeof(knee));
    std::memcpy(reinterpret_cast<void *>(address + 10 * pose_stride + 0x20), ankle, sizeof(ankle));
}

// The same standing leg the pose was built with, written back over a chain
// that a test moved: hip, knee, ankle, toe (local positions, identity
// rotations) -- i.e. "the clip authored this leg at standing height".
void put_leg(std::uintptr_t address, std::uint32_t hip, std::uint32_t knee, std::uint32_t ankle,
             std::uint32_t toe) {
    const float hip_pos[3] = {hip == 8 ? 0.1f : -0.1f, 0.0f, 0.0f};
    const float knee_pos[3] = {0.0f, -0.4f, 0.05f};
    const float ankle_pos[3] = {0.0f, -0.4f, -0.05f};
    const float toe_pos[3] = {0.0f, -0.05f, 0.1f};
    std::memcpy(reinterpret_cast<void *>(address + hip * pose_stride + 0x20), hip_pos, sizeof(hip_pos));
    std::memcpy(reinterpret_cast<void *>(address + knee * pose_stride + 0x20), knee_pos, sizeof(knee_pos));
    std::memcpy(reinterpret_cast<void *>(address + ankle * pose_stride + 0x20), ankle_pos, sizeof(ankle_pos));
    std::memcpy(reinterpret_cast<void *>(address + toe * pose_stride + 0x20), toe_pos, sizeof(toe_pos));
}

float lowest_foot(std::uintptr_t address) {
    const std::uint32_t right[] = {1, 7, 8, 9, 10, 11};
    float out[3]{};
    check(world_position(address, right, 6, out), "the foot composes");
    return out[1]; // the toe is the lowest point of this chain
}

float distance_between(std::uintptr_t address, const std::uint32_t *a, std::size_t ac,
                       const std::uint32_t *b, std::size_t bc) {
    float pa[3]{}, pb[3]{};
    check(world_position(address, a, ac, pa), "first joint composes");
    check(world_position(address, b, bc, pb), "second joint composes");
    const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void test_floor_sample() {
    // The floor is the game's own feet, sampled before the clip write: per
    // foot its lowest point (ankle or toe), plus the way that knee bends and
    // that sole pitches. On flat ground both feet agree and the shared floor
    // is the lowest of them.
    auto pose = standing_pose();
    const auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    FloorSample floor{};
    check(sample_floors(address, floor), "the standing pose has floors");
    check(floor.valid, "both leg chains composed");
    check(floor.legs[0].valid && floor.legs[1].valid, "both feet are valid");
    check(std::abs(floor.legs[0].foot_y - (-0.95f)) < 1e-3f, "the right foot's floor is its toe height");
    check(std::abs(floor.legs[1].foot_y - (-0.95f)) < 1e-3f, "the left foot's floor is its toe height");
    check(std::abs(floor.floor_y - (-0.95f)) < 1e-3f, "the shared floor is the lowest foot");
    check(std::abs(floor.legs[0].ankle_offset - 0.05f) < 1e-3f, "the ankle rides 5 cm above the sole");
    // The knee in the standing pose bends forward (+z off the hip-ankle line)
    // and the ankle->toe direction pitches forward and slightly down.
    check(floor.legs[0].knee[2] > 0.5f, "the sampled knee bends forward");
    check(floor.legs[0].toe_valid && floor.legs[0].toe_dir[2] > 0.5f,
          "the sampled sole pitches forward");
}

// A sampled floor from the standing pose, for the tests that correct a clip
// written over it.
FloorSample standing_floor(std::uintptr_t address) {
    FloorSample floor{};
    check(sample_floors(address, floor), "the floor samples");
    return floor;
}

void test_floor_lift() {
    // A clip sank the right foot about 0.1 below the game's floor; the solve
    // lifts it back without stretching the leg or flipping the knee.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto floor = standing_floor(address);
    sink_right_foot(address);
    const std::uint32_t hip_chain[] = {1, 7, 8};
    const std::uint32_t knee_chain[] = {1, 7, 8, 9};
    const std::uint32_t ankle_chain[] = {1, 7, 8, 9, 10};
    const float thigh_before = distance_between(address, knee_chain, 4, hip_chain, 3);
    const float shin_before = distance_between(address, ankle_chain, 5, knee_chain, 4);
    check(lowest_foot(address) < floor.floor_y - 0.05f, "the clip's foot is under the floor");
    apply_floor(address, floor, 0.01f, 1.0f, 0.0f);
    check(lowest_foot(address) > floor.floor_y - 0.02f, "the foot is lifted back to the floor");
    check(std::abs(distance_between(address, knee_chain, 4, hip_chain, 3) - thigh_before) < 1e-4f,
          "the thigh is not stretched");
    check(std::abs(distance_between(address, ankle_chain, 5, knee_chain, 4) - shin_before) < 1e-4f,
          "the shin is not stretched");
    // The knee stays on its authored side: forward of the hip-ankle line.
    float hip[3]{}, knee[3]{}, ankle[3]{};
    check(world_position(address, hip_chain, 3, hip), "hip composes");
    check(world_position(address, knee_chain, 4, knee), "knee composes");
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes");
    const float line_z = 0.5f * (hip[2] + ankle[2]);
    check(knee[2] > line_z, "the knee still bends forward");
    // Only the two rotations moved: local positions, scales and spares stay
    // as the clip wrote them, and the foot's own joints keep their rotation.
    check(std::abs(field(address, 10, 0x20)) < 1e-4f, "the ankle's local position is untouched");
    check(std::abs(field(address, 9, 0x20)) < 1e-4f, "the knee's local position is untouched");
    check(untouched(address, 8, 0x0c) && untouched(address, 9, 0x2c), "the solve leaves spare floats");
    const float hip_w = field(address, 8, 0x1c), knee_w = field(address, 9, 0x1c);
    check(std::abs(hip_w - 1.0f) > 1e-4f || std::abs(field(address, 8, 0x10)) > 1e-4f, "the hip rotation changed");
    check(std::abs(knee_w - 1.0f) > 1e-4f || std::abs(field(address, 9, 0x10)) > 1e-4f,
          "the knee rotation changed");
    check(untouched(address, 10, 0x2c), "the ankle's spare float is untouched");
    // The lift tilts the foot with the shin; the flatten then turns the ankle's
    // own rotation so the sole ends pitched like the game's planted one. That
    // is the whole point of v2 -- flat feet -- so the ankle rotation is
    // expected to move; the pitch itself is checked in test_floor_sole_pitch.
    {
        const std::uint32_t toe_chain[] = {1, 7, 8, 9, 10, 11};
        float toe[3]{};
        check(world_position(address, toe_chain, 6, toe), "toe composes");
        const float sole[3] = {toe[0] - ankle[0], toe[1] - ankle[1], toe[2] - ankle[2]};
        const float sole_len = std::sqrt(sole[0] * sole[0] + sole[1] * sole[1] + sole[2] * sole[2]);
        check(sole_len > 1e-3f &&
                  (sole[0] * floor.legs[0].toe_dir[0] + sole[1] * floor.legs[0].toe_dir[1] +
                   sole[2] * floor.legs[0].toe_dir[2]) /
                          sole_len > 0.99f,
              "the lifted foot's sole is pitched like the game's planted one");
    }
    // The left leg was never sunk, so the solve never touched it.
    check(std::abs(field(address, 342, 0x1c) - 1.0f) < 1e-4f, "the untouched leg keeps its rotation");
    // Strength scales the lift: half strength leaves the foot half sunk.
    auto half = standing_pose();
    auto half_address = reinterpret_cast<std::uintptr_t>(half.data());
    sink_right_foot(half_address);
    const float sunk = lowest_foot(half_address);
    apply_floor(half_address, floor, 0.01f, 0.5f, 0.0f);
    const float lifted = lowest_foot(half_address);
    check(std::abs((lifted - sunk) - 0.5f * (floor.floor_y - sunk)) < 0.02f, "half strength lifts half the sink");
    // Strength 0 is the old system: nothing moves at all.
    auto off = standing_pose();
    auto off_address = reinterpret_cast<std::uintptr_t>(off.data());
    sink_right_foot(off_address);
    const float before[3] = {field(off_address, 8, 0x10), field(off_address, 8, 0x14), field(off_address, 8, 0x1c)};
    apply_floor(off_address, floor, 0.01f, 0.0f, 0.0f);
    check(std::abs(field(off_address, 8, 0x10) - before[0]) < 1e-6f &&
              std::abs(field(off_address, 8, 0x14) - before[1]) < 1e-6f &&
              std::abs(field(off_address, 8, 0x1c) - before[2]) < 1e-6f,
          "strength 0 writes nothing (the noik system)");
    // A foot above the floor on flat ground is left exactly as authored: the
    // game's two feet agree, so the terrain is not uneven and a raised foot
    // is the clip's authorship.
    auto kick = standing_pose();
    auto kick_address = reinterpret_cast<std::uintptr_t>(kick.data());
    const float knee_high[3] = {0.0f, 0.4f, 0.05f};
    const float ankle_high[3] = {0.0f, 0.35f, 0.0f}; // a raised kick, clear of the floor
    std::memcpy(reinterpret_cast<void *>(kick_address + 9 * pose_stride + 0x20), knee_high, sizeof(knee_high));
    std::memcpy(reinterpret_cast<void *>(kick_address + 10 * pose_stride + 0x20), ankle_high,
                sizeof(ankle_high));
    apply_floor(kick_address, floor, 0.01f, 1.0f, 0.0f);
    check(std::abs(field(kick_address, 8, 0x1c) - 1.0f) < 1e-4f, "a foot above the floor is not moved");
}

void test_floor_knee_hint() {
    // The game's knee bends forward; the clip bent the same knee BACKWARD
    // (the mirrored-rig case from the in-game report: legs bending the wrong
    // way). The game's sampled knee direction must win over the clip's.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto floor = standing_floor(address);
    check(floor.legs[0].knee[2] > 0.5f, "the game's knee hint bends forward");
    // The clip: knee swung behind the hip-ankle line, ankle sunk below the
    // floor so the solve has to run at all.
    const float knee_back[3] = {0.0f, -0.45f, -0.08f};
    const float ankle_back[3] = {0.0f, -0.45f, -0.02f};
    std::memcpy(reinterpret_cast<void *>(address + 9 * pose_stride + 0x20), knee_back, sizeof(knee_back));
    std::memcpy(reinterpret_cast<void *>(address + 10 * pose_stride + 0x20), ankle_back, sizeof(ankle_back));
    const std::uint32_t hip_chain[] = {1, 7, 8};
    const std::uint32_t knee_chain[] = {1, 7, 8, 9};
    const std::uint32_t ankle_chain[] = {1, 7, 8, 9, 10};
    check(lowest_foot(address) < floor.floor_y - 0.05f, "the clip's backward-bent leg is under the floor");
    apply_floor(address, floor, 0.01f, 1.0f, 0.0f);
    check(lowest_foot(address) > floor.floor_y - 0.02f, "the foot is lifted back to the floor");
    float hip[3]{}, knee[3]{}, ankle[3]{};
    check(world_position(address, hip_chain, 3, hip), "hip composes");
    check(world_position(address, knee_chain, 4, knee), "knee composes");
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes");
    const float line_z = 0.5f * (hip[2] + ankle[2]);
    check(knee[2] > line_z, "the knee is bent the game's way, not the clip's");
}

void test_floor_uneven() {
    // A stance straddling a ledge: the game's right foot on the ledge top
    // (-0.65), its left foot on the ground (-0.95). The clip authored both
    // feet at standing height. Each foot must come right for ITS surface --
    // the right one lifted onto the ledge, not left floating below it.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    // Raise the game's right foot onto the ledge: ankle 0.3 higher, toe with
    // it (the game's own foot placement on the higher surface).
    const float knee_up[3] = {0.0f, -0.25f, 0.05f};
    const float ankle_up[3] = {0.0f, -0.25f, -0.05f};
    std::memcpy(reinterpret_cast<void *>(address + 9 * pose_stride + 0x20), knee_up, sizeof(knee_up));
    std::memcpy(reinterpret_cast<void *>(address + 10 * pose_stride + 0x20), ankle_up, sizeof(ankle_up));
    const auto floor = standing_floor(address);
    check(std::abs(floor.legs[0].foot_y - (-0.65f)) < 1e-3f, "the right foot's floor is the ledge top");
    check(std::abs(floor.legs[1].foot_y - (-0.95f)) < 1e-3f, "the left foot's floor is the ground");
    check(floor.floor_y < -0.9f, "the shared floor is the lower surface");
    // The clip then writes both legs back at standing height: its right foot
    // now floats 0.3 below the ledge it should stand on.
    put_leg(address, 8, 9, 10, 11);
    const std::uint32_t ankle_chain[] = {1, 7, 8, 9, 10};
    float ankle[3]{};
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes");
    check(ankle[1] < floor.legs[0].foot_y - 0.2f, "the clip's right foot hangs below the ledge");
    apply_floor(address, floor, 0.01f, 1.0f, 0.0f);
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes after");
    // At full strength the foot stands on its own ledge; a few passes of
    // convergence, so allow a small residual.
    check(ankle[1] > floor.legs[0].foot_y - 0.03f, "the right foot is lifted onto its own ledge");
    check(lowest_foot(address) > floor.legs[0].foot_y - 0.05f, "the right foot's toe clears the ledge");
    // The left leg was already at its floor and stays there.
    const std::uint32_t left_ankle_chain[] = {1, 7, 341, 342, 343};
    float left[3]{};
    check(world_position(address, left_ankle_chain, 5, left), "left ankle composes");
    check(std::abs(left[1] - (-0.9f)) < 0.02f, "the left foot stays at the ground");
}

void test_floor_reach_down() {
    // The other half of the ledge: the clip's foot floats ABOVE the surface
    // under it (authored as if the ground continued), and the game's two feet
    // disagree in height, so the terrain is uneven. The foot must reach down
    // to its own surface instead of hanging in the air.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    // The game's right foot dangles just below standing -- the edge of the
    // ledge: ankle a little deeper, toe with it.
    const float knee_low[3] = {0.0f, -0.45f, 0.05f};
    const float ankle_low[3] = {0.0f, -0.45f, -0.02f};
    std::memcpy(reinterpret_cast<void *>(address + 9 * pose_stride + 0x20), knee_low, sizeof(knee_low));
    std::memcpy(reinterpret_cast<void *>(address + 10 * pose_stride + 0x20), ankle_low, sizeof(ankle_low));
    const auto floor = standing_floor(address);
    check(floor.legs[0].foot_y < -1.0f, "the right foot's floor is the lower surface");
    // The clip writes the leg raised instead: a deeply bent knee floats the
    // foot a follow-band's worth above that surface, and extending the leg is
    // what reaches it.
    const float knee_raise[3] = {0.0f, -0.28f, 0.3f};  // knee forward, leg folded
    const float ankle_raise[3] = {0.0f, -0.4f, -0.35f}; // ankle well above full extension
    std::memcpy(reinterpret_cast<void *>(address + 9 * pose_stride + 0x20), knee_raise, sizeof(knee_raise));
    std::memcpy(reinterpret_cast<void *>(address + 10 * pose_stride + 0x20), ankle_raise, sizeof(ankle_raise));
    const std::uint32_t ankle_chain[] = {1, 7, 8, 9, 10};
    float ankle[3]{};
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes");
    const float before = ankle[1];
    check(before > floor.legs[0].foot_y + 0.2f, "the clip's foot floats above its surface");
    apply_floor(address, floor, 0.01f, 1.0f, 0.0f);
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes after");
    check(ankle[1] < before - 0.1f, "the floating foot reaches down toward its surface");
    check(lowest_foot(address) > floor.legs[0].foot_y - 0.03f, "the foot lands on its surface, not through it");
    // On flat ground the same raised foot is authored and stays: the reach
    // down only runs where the game's own feet disagree.
    auto flat = standing_pose();
    auto flat_address = reinterpret_cast<std::uintptr_t>(flat.data());
    const auto flat_floor = standing_floor(flat_address);
    std::memcpy(reinterpret_cast<void *>(flat_address + 9 * pose_stride + 0x20), knee_raise, sizeof(knee_raise));
    std::memcpy(reinterpret_cast<void *>(flat_address + 10 * pose_stride + 0x20), ankle_raise, sizeof(ankle_raise));
    float raised[3]{};
    check(world_position(flat_address, ankle_chain, 5, raised), "raised ankle composes");
    apply_floor(flat_address, flat_floor, 0.01f, 1.0f, 0.0f);
    float after[3]{};
    check(world_position(flat_address, ankle_chain, 5, after), "raised ankle composes after");
    check(std::abs(after[1] - raised[1]) < 1e-3f, "on flat ground a raised foot is left as authored");
    check(std::abs(field(flat_address, 10, 0x1c) - 1.0f) < 1e-4f && std::abs(field(flat_address, 10, 0x10)) < 1e-4f,
          "its ankle rotation is untouched too");
}

void test_floor_sole_pitch() {
    // The clip authored the right foot with the toe pitched UP (heel down,
    // toe in the air) while standing on its floor; the game's planted sole
    // pitches forward-down. Near the floor the sole must be flattened onto
    // the game's pitch, without moving the foot through the ground.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto floor = standing_floor(address);
    // The clip: same ankle height, toe raised well above it.
    const float toe_up[3] = {0.0f, 0.06f, 0.05f}; // toe local: up and forward
    std::memcpy(reinterpret_cast<void *>(address + 11 * pose_stride + 0x20), toe_up, sizeof(toe_up));
    const std::uint32_t ankle_chain[] = {1, 7, 8, 9, 10};
    const std::uint32_t toe_chain[] = {1, 7, 8, 9, 10, 11};
    float ankle[3]{}, toe[3]{};
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes");
    check(world_position(address, toe_chain, 6, toe), "toe composes");
    float before[3] = {toe[0] - ankle[0], toe[1] - ankle[1], toe[2] - ankle[2]};
    check(before[1] > 0.01f, "the clip's toe is pitched above the ankle");
    apply_floor(address, floor, 0.01f, 1.0f, 0.0f);
    check(world_position(address, ankle_chain, 5, ankle), "ankle composes after");
    check(world_position(address, toe_chain, 6, toe), "toe composes after");
    const float after_v[3] = {toe[0] - ankle[0], toe[1] - ankle[1], toe[2] - ankle[2]};
    const float before_len = std::sqrt(before[0] * before[0] + before[1] * before[1] + before[2] * before[2]);
    const float after_len = std::sqrt(after_v[0] * after_v[0] + after_v[1] * after_v[1] + after_v[2] * after_v[2]);
    check(after_v[1] < 0.0f, "the sole is flattened onto the game's pitch (toe below the ankle)");
    // The game's ankle->toe direction, for comparison.
    const auto &game_toe = floor.legs[0].toe_dir;
    check(floor.legs[0].toe_valid, "the game's sole pitch sampled");
    const float dot = (after_v[0] * game_toe[0] + after_v[1] * game_toe[1] + after_v[2] * game_toe[2]) / after_len;
    check(dot > 0.98f, "the sole matches the game's planted pitch");
    check(std::abs(after_len - before_len) < 1e-3f, "the ankle->toe bone length is unchanged");
    check(lowest_foot(address) > floor.floor_y - 0.02f, "flattening did not push the toe through the floor");
}

void test_floor_torso() {
    // A clip with the spine bent far forward: the head ends up below the
    // floor. The pelvis is the game's (hips already safe), but the spine and
    // head chain must be lifted clear -- rotations only, bones unpulled,
    // pelvis untouched.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto floor = standing_floor(address);
    // Bend the torso down: spine joints stacked forward-down so the head
    // composes well below the floor.
    for (std::uint32_t joint = 43; joint <= 45; ++joint)
        put(address, joint, 0.0f, -0.28f, 0.04f);
    for (std::uint32_t joint = 101; joint <= 103; ++joint)
        put(address, joint, 0.0f, -0.15f, 0.02f);
    const std::uint32_t head_chain[] = {1, 7, 42, 43, 44, 45, 101, 102, 103};
    const std::uint32_t hip_chain[] = {1, 7, 8};
    const std::uint32_t chest_chain[] = {1, 7, 42, 43, 44, 45};
    float head[3]{};
    check(world_position(address, head_chain, 9, head), "head composes");
    check(head[1] < floor.floor_y - 0.1f, "the clip's head is through the floor");
    // Bone lengths: the spine from 42 to 45 measures the same before and
    // after -- only rotations are ever written, so the chain cannot stretch.
    const float spine_before = distance_between(address, chest_chain, 6, hip_chain, 3);
    apply_floor(address, floor, 0.01f, 1.0f, 0.0f);
    check(world_position(address, head_chain, 9, head), "head composes after");
    check(head[1] > floor.floor_y - 0.02f, "the head is lifted clear of the floor");
    // The pelvis is the game's and stays exactly as it was.
    check(std::abs(field(address, 7, 0x10)) < 1e-6f && std::abs(field(address, 7, 0x1c) - 1.0f) < 1e-6f,
          "the pelvis rotation is untouched");
    // Only the root and mid of the chain turned: spine 42 and chest 45.
    check(std::abs(field(address, 42, 0x1c) - 1.0f) > 1e-4f || std::abs(field(address, 42, 0x10)) > 1e-4f,
          "the spine root rotation changed");
    check(std::abs(field(address, 45, 0x1c) - 1.0f) > 1e-4f || std::abs(field(address, 45, 0x10)) > 1e-4f,
          "the chest rotation changed");
    // The in-between spine joints keep their local rotation: the solve only
    // writes root and mid.
    check(std::abs(field(address, 43, 0x1c) - 1.0f) < 1e-4f && std::abs(field(address, 44, 0x1c) - 1.0f) < 1e-4f,
          "the spine between root and mid is untouched");
    const float spine_after = distance_between(address, chest_chain, 6, hip_chain, 3);
    check(std::abs(spine_after - spine_before) < 1e-4f, "the spine length is unchanged");
    // Positions are never written.
    check(std::abs(field(address, 43, 0x20)) < 1e-6f && std::abs(field(address, 103, 0x28) - 0.02f) < 1e-6f,
          "the torso solve writes no positions");
}

void test_floor_hands() {
    // A wrist below the game's floor is lifted even while the mask keeps the
    // legs: hands are always the clip's. A hand above the floor -- a
    // handplant reaching for it -- stays where the clip put it.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto floor = standing_floor(address);
    const float wrist[3] = {0.0f, -0.65f, 0.05f}; // below the elbow, past the floor
    std::memcpy(reinterpret_cast<void *>(address + 49 * pose_stride + 0x20), wrist, sizeof(wrist));
    apply_floor(address, floor, 0.01f, 1.0f, 1.0f); // keep = 1: the game owns the legs
    float composed[3]{};
    const std::uint32_t wrist_chain[] = {1, 7, 42, 43, 44, 45, 46, 47, 48, 49};
    check(world_position(address, wrist_chain, 10, composed), "the wrist composes");
    check(composed[1] > floor.floor_y - 0.03f, "the sunk wrist is lifted above the floor");
    check(std::abs(field(address, 48, 0x20)) < 1e-4f, "the elbow's local position is untouched");
    check(std::abs(field(address, 49, 0x28) - 0.05f) < 1e-4f, "the wrist's local z is untouched");
    // A hand above the floor is intentional and left alone.
    auto plant = standing_pose();
    auto plant_address = reinterpret_cast<std::uintptr_t>(plant.data());
    const float planted[3] = {0.0f, -0.35f, 0.05f}; // wrist well above the floor
    std::memcpy(reinterpret_cast<void *>(plant_address + 49 * pose_stride + 0x20), planted, sizeof(planted));
    apply_floor(plant_address, floor, 0.01f, 1.0f, 1.0f);
    check(std::abs(field(plant_address, 47, 0x1c) - 1.0f) < 1e-4f, "a hand above the floor is not moved");
}

void test_floor_masked_legs() {
    // keep = 1: the game owns the legs, and the legs it planted ARE the floor.
    // The correction must not touch them; the arms still get corrected.
    auto pose = standing_pose();
    auto address = reinterpret_cast<std::uintptr_t>(pose.data());
    const auto floor = standing_floor(address);
    sink_right_foot(address);
    const float wrist[3] = {0.0f, -0.65f, 0.05f};
    std::memcpy(reinterpret_cast<void *>(address + 49 * pose_stride + 0x20), wrist, sizeof(wrist));
    apply_floor(address, floor, 0.01f, 1.0f, 1.0f);
    check(std::abs(field(address, 8, 0x1c) - 1.0f) < 1e-4f && std::abs(field(address, 9, 0x1c) - 1.0f) < 1e-4f,
          "a leg the mask kept is not corrected");
    check(lowest_foot(address) < floor.floor_y - 0.05f, "the game's own sunk leg stays as the game wrote it");
    float composed[3]{};
    const std::uint32_t wrist_chain[] = {1, 7, 42, 43, 44, 45, 46, 47, 48, 49};
    check(world_position(address, wrist_chain, 10, composed), "the wrist composes");
    check(composed[1] > floor.floor_y - 0.03f, "the arm is still corrected while the legs are kept");
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
        test_floor_sample();
        test_floor_lift();
        test_floor_knee_hint();
        test_floor_uneven();
        test_floor_reach_down();
        test_floor_sole_pitch();
        test_floor_torso();
        test_floor_hands();
        test_floor_masked_legs();
        std::cout << "pose layer tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout << "pose layer tests FAILED: " << error.what() << "\n";
        return 1;
    }
}
