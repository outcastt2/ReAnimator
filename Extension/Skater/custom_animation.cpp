#include "custom_animation.h"
#include "Extension/Multiplayer/Remote/native_skater_internal.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// Route B: overwrite the local skater's output pose with a baked clip.
namespace dingosdk::skater {
namespace {
namespace nsd = multiplayer::native_skater_detail;
using nsd::ptr;
using nsd::read;
using nsd::readable;

constexpr std::size_t skater_joint_bound = 512;
constexpr std::uint16_t test_joint = 103; // head
constexpr std::uint16_t test_spine_joint = 7; // a body joint in the head chain's ancestry
constexpr std::size_t floats_per_joint = 10; // scale.xyz, quat.xyzw, pos.xyz
constexpr std::uint32_t expected_skater_joints = 395;
constexpr std::uint32_t max_record_frames = 1800; // 30 s at 60 Hz

// A clip is frames * joints * 10 floats.
struct Clip {
    std::uint32_t joints{}, frames{};
    float fps{30.0f};
    std::vector<float> data;
};
struct Playback {
    std::mutex mutex;
    Clip clip;
    std::string clip_name, status;
    std::atomic<bool> playing{};
    std::atomic<bool> test{};
    std::atomic<std::uintptr_t> component{};
    std::atomic<std::uint64_t> writes{};
    std::uintptr_t base{};
    ULONGLONG started{};
    bool pending{}, stop{}, pending_test{};
    std::string pending_clip;
    // The pose as it was when playback began, written back on stop so a write
    // into an idle graph's output buffer does not outlive the animation.
    std::vector<std::byte> saved_pose;
    std::uint32_t saved_joints{};
    // Recording: the live pose is captured as it is evaluated.
    std::atomic<bool> recording{};
    std::vector<float> record_data;
    std::atomic<std::uint32_t> record_frames{};
    std::uint32_t record_capacity{};
    ULONGLONG record_started{};
    float record_fps{60.0f};
};
Playback &playback() { static Playback p; return p; }
std::mutex &status_mutex() { static std::mutex m; return m; }
void set_status(const std::string &text) {
    std::lock_guard lock(status_mutex());
    playback().status = text;
}

bool load_clip(const std::string &path, Clip &clip, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path; return false; }
    std::array<char, 20> header{};
    in.read(header.data(), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size()) || std::memcmp(header.data(), "RSKA", 4) != 0) {
        error = "not an .rska clip";
        return false;
    }
    std::uint32_t version{}, joints{}, frames{};
    float fps{};
    std::memcpy(&version, header.data() + 4, 4);
    std::memcpy(&joints, header.data() + 8, 4);
    std::memcpy(&frames, header.data() + 12, 4);
    std::memcpy(&fps, header.data() + 16, 4);
    if (version != 1 || joints == 0 || joints > skater_joint_bound || frames == 0 || frames > 100000 ||
        !(fps > 0.0f && fps <= 240.0f)) {
        error = "unsupported clip header";
        return false;
    }
    clip.joints = joints;
    clip.frames = frames;
    clip.fps = fps;
    clip.data.resize(static_cast<std::size_t>(frames) * joints * floats_per_joint);
    in.read(reinterpret_cast<char *>(clip.data.data()), static_cast<std::streamsize>(clip.data.size() * sizeof(float)));
    if (in.gcount() != static_cast<std::streamsize>(clip.data.size() * sizeof(float))) {
        error = "clip data is truncated";
        return false;
    }
    return true;
}

// The output pose buffer for a component, or {0, 0}.
struct PoseLocation {
    std::uintptr_t buffer{};
    std::uint32_t joints{};
};
PoseLocation pose_location(std::uintptr_t base, std::uintptr_t component) {
    std::uintptr_t holder{};
    if (!readable(component + 0xa0, &holder, 8) || !holder) return {};
    const auto reader = [](std::uintptr_t address, void *out, std::size_t size) {
        return readable(address, out, size);
    };
    const auto pose = multiplayer::read_native_pose_layout(reader, base, holder, skater_joint_bound);
    return {pose.buffer, pose.count};
}

// The local skater's animation component, or 0.
std::uintptr_t local_component(std::uintptr_t base, std::uintptr_t client) {
    try {
        nsd::require(ptr(client) == base + addr::engine::client_vtable, "Local client is unavailable.");
        const auto context = ptr(client, 8);
        const auto offset = read<std::uint32_t>(base, addr::engine::context_player_manager_offset);
        nsd::require(offset <= 0x1000000, "Player manager offset changed.");
        const auto manager = ptr(context, offset);
        const auto begin = ptr(manager, 0x4c8);
        const auto player = ptr(begin);
        const auto entity = ptr(player, 0xb8);
        return ptr(entity, 0x628);
    } catch (...) {
        return 0;
    }
}

void write_frame(std::uintptr_t buffer, const float *frame, std::uint32_t joints) noexcept {
    for (std::uint32_t j = 0; j < joints; ++j) {
        const auto *src = frame + static_cast<std::size_t>(j) * floats_per_joint;
        auto *dst = reinterpret_cast<std::uint8_t *>(buffer) + static_cast<std::size_t>(j) * 0x30;
        std::memcpy(dst + 0x00, src + 0, 12);  // scale.xyz (w left alone)
        std::memcpy(dst + 0x10, src + 3, 16);  // quat.xyzw
        std::memcpy(dst + 0x20, src + 7, 12);  // pos.xyz (w left alone)
    }
}
} // namespace

// Packs one evaluated pose into the clip format: scale.xyz, quat.xyzw, pos.xyz.
namespace {
void pack_frame(std::uintptr_t buffer, std::uint32_t joints, float *out) noexcept {
    std::array<std::byte, expected_skater_joints * 0x30> raw{};
    const auto bytes = static_cast<std::size_t>(joints) * 0x30;
    if (bytes > raw.size() || !readable(buffer, raw.data(), bytes)) return;
    for (std::uint32_t j = 0; j < joints; ++j) {
        const auto *bone = reinterpret_cast<const float *>(raw.data() + static_cast<std::size_t>(j) * 0x30);
        float *dst = out + static_cast<std::size_t>(j) * floats_per_joint;
        dst[0] = bone[0]; dst[1] = bone[1]; dst[2] = bone[2];
        dst[3] = bone[4]; dst[4] = bone[5]; dst[5] = bone[6]; dst[6] = bone[7];
        dst[7] = bone[8]; dst[8] = bone[9]; dst[9] = bone[10];
    }
}
} // namespace

void on_pose_evaluated(std::uintptr_t component) noexcept {
    try {
        auto &p = playback();
        if (component != p.component.load(std::memory_order_acquire)) return;
        if (p.recording.load(std::memory_order_acquire)) {
            const auto rec = pose_location(p.base, component);
            if (!rec.buffer || !rec.joints) return;
            const auto frame = p.record_frames.load(std::memory_order_relaxed);
            if (frame >= p.record_capacity) return;
            auto *dst = p.record_data.data() + static_cast<std::size_t>(frame) * rec.joints * floats_per_joint;
            pack_frame(rec.buffer, rec.joints, dst);
            p.record_frames.store(frame + 1, std::memory_order_release);
            if (frame == 0)
                logging::log(logging::Level::info, logging::Channel::skater, "Custom animation: recording.");
            return; // Never overwrite the pose while recording it.
        }
        if (!p.playing.load(std::memory_order_acquire)) return;
        const auto pose = pose_location(p.base, component);
        const auto buffer = pose.buffer;
        if (!buffer) return;
        // Confirm once per playback that the write is actually happening.
        const auto note_write = [&p] {
            const auto count = p.writes.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count == 1 || count == 10 || count == 100 || count == 1000 || count == 10000)
                logging::log(logging::Level::info, logging::Channel::skater,
                             "Custom animation: pose written ({} times).", count);
        };
        const auto elapsed_ms = GetTickCount64() - p.started;
        if (p.test.load(std::memory_order_relaxed)) {
            const float phase = static_cast<float>(elapsed_ms) * 0.003f;
            // 1) Head scale: the operation first person uses to hide the head,
            //    already proven to reach the renderer.
            std::array<float, 12> head{};
            if (readable(buffer + test_joint * 0x30ULL, head.data(), sizeof(head))) {
                const float scale = 0.5f + 0.45f * std::sin(phase);
                head[0] = head[1] = head[2] = scale;
                (void)nsd::write(buffer + test_joint * 0x30ULL, head.data(), sizeof(head));
            }
            // 2) A rotation on a body joint. If the body twists, rotations land
            //    for everything but the head; if it does not, the engine's
            //    post-physics skeleton response is rewriting the whole pose's
            //    rotations after this write.
            std::array<float, 12> spine{};
            if (readable(buffer + test_spine_joint * 0x30ULL, spine.data(), sizeof(spine))) {
                const float angle = 1.0f * std::sin(phase);
                const float half = angle * 0.5f, s = std::sin(half), c = std::cos(half);
                const float qx = spine[4], qy = spine[5], qz = spine[6], qw = spine[7];
                spine[4] = qw * s + qx * c;
                spine[5] = qy * c + qz * s;
                spine[6] = qz * c - qy * s;
                spine[7] = qw * c - qx * s;
                (void)nsd::write(buffer + test_spine_joint * 0x30ULL, spine.data(), sizeof(spine));
            }
            note_write();
            return;
        }
        std::lock_guard lock(p.mutex);
        if (p.clip.frames == 0 || p.clip.data.empty() || p.clip.joints <= 2) return;
        const auto frame = static_cast<std::uint32_t>(
            (static_cast<double>(elapsed_ms) * p.clip.fps / 1000.0)) % p.clip.frames;
        // Joints 0 and 1 are not pose: joint 1 carries the skater's world
        // placement, and the game drives it from the board and physics. Leaving
        // it alone is what keeps the skater on the board, the way a gesture
        // animates in place while the player keeps moving.
        write_frame(buffer + 2 * 0x30ULL,
                    p.clip.data.data() + static_cast<std::size_t>(frame) * p.clip.joints * floats_per_joint +
                        2 * floats_per_joint,
                    p.clip.joints - 2);
        note_write();
    } catch (...) {
    }
}

void request_pose_playback(std::string clip) {
    std::lock_guard lock(playback().mutex);
    playback().pending = true;
    playback().stop = false;
    playback().pending_test = clip == "test";
    playback().pending_clip = std::move(clip);
}
void request_pose_playback_stop() {
    std::lock_guard lock(playback().mutex);
    playback().stop = true;
    playback().pending = false;
}
void request_pose_record() { request_pose_playback("record"); }
void request_pose_record_playback() { request_pose_playback("play"); }
std::string pose_playback_status() {
    std::lock_guard lock(status_mutex());
    return playback().status;
}

void tick_pose_playback(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        auto &p = playback();
        std::string pending_clip;
        bool pending{}, stop{}, pending_test{};
        {
            std::lock_guard lock(p.mutex);
            pending = p.pending;
            stop = p.stop;
            pending_test = p.pending_test;
            pending_clip = p.pending_clip;
            p.pending = p.stop = false;
        }
        if (stop) {
            if (p.recording.load(std::memory_order_acquire)) {
                const auto frames = p.record_frames.load(std::memory_order_acquire);
                const auto elapsed = GetTickCount64() - p.record_started;
                p.record_fps = elapsed > 0
                    ? static_cast<float>(static_cast<double>(frames) * 1000.0 / static_cast<double>(elapsed)) : 60.0f;
                if (p.record_fps < 1.0f || p.record_fps > 240.0f) p.record_fps = 60.0f;
                p.recording.store(false, std::memory_order_release);
                p.component.store(0, std::memory_order_release);
                set_status("Recording stopped (" + std::to_string(frames) + " frames). Run poseanim play.");
                logging::log(logging::Level::info, logging::Channel::skater,
                             "Custom animation: recording stopped after {} frames over {} ms ({:.1f} fps).",
                             frames, elapsed, p.record_fps);
                return;
            }
            p.playing.store(false, std::memory_order_release);
            // Put the pose captured at start back over the output buffer. Joint
            // 1 carries world placement, so it is left alone: restoring it would
            // teleport the skater back to where they stood when playback began.
            const auto component = p.component.load(std::memory_order_acquire);
            if (component && p.saved_joints > 2 && !p.saved_pose.empty()) {
                const auto pose = pose_location(base, component);
                if (pose.buffer && pose.joints > 2) {
                    const auto joints = p.saved_joints < pose.joints ? p.saved_joints : pose.joints;
                    (void)nsd::write(pose.buffer + 2 * 0x30ULL, p.saved_pose.data() + 2 * 0x30ULL,
                                     static_cast<std::size_t>(joints - 2) * 0x30);
                }
            }
            p.saved_pose.clear();
            p.saved_joints = 0;
            p.component.store(0, std::memory_order_release);
            set_status("Custom animation stopped.");
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Custom animation: stopped after {} pose writes.", p.writes.load());
            return;
        }
        if (!pending) {
            // A level change or respawn frees the pose buffer: stop rather than
            // keep writing to a component that is no longer the local skater.
            // A zero here is a transient read failure, not a different skater.
            if (const auto current = local_component(base, client);
                p.playing.load(std::memory_order_acquire) && current &&
                p.component.load(std::memory_order_acquire) != current) {
                p.playing.store(false, std::memory_order_release);
                p.saved_pose.clear();
                p.saved_joints = 0;
                p.component.store(0, std::memory_order_release);
                set_status("Custom animation stopped (the skater changed).");
            }
            return;
        }
        const auto component = local_component(base, client);
        if (!component) {
            set_status("Custom animation: the local skater's animation component is unavailable.");
            return;
        }
        if (pending_clip == "record") {
            std::string detail;
            if (!multiplayer::install_entity_hooks(base, detail)) {
                set_status("Custom animation: the animation hook is unavailable: " + detail);
                return;
            }
            multiplayer::set_pose_playback_listener(&on_pose_evaluated);
            p.base = base;
            p.component.store(component, std::memory_order_release);
            p.playing.store(false, std::memory_order_release);
            p.recording.store(false, std::memory_order_release);
            {
                std::lock_guard lock(p.mutex);
                p.clip = {};
            }
            p.record_data.assign(
                static_cast<std::size_t>(max_record_frames) * expected_skater_joints * floats_per_joint, 0.0f);
            p.record_capacity = max_record_frames;
            p.record_frames.store(0, std::memory_order_release);
            p.record_started = GetTickCount64();
            p.saved_pose.clear();
            p.saved_joints = 0;
            p.recording.store(true, std::memory_order_release);
            set_status("Recording the pose; run poseanim off to stop.");
            logging::log(logging::Level::info, logging::Channel::skater, "Custom animation: recording started.");
            return;
        }
        if (pending_clip == "play") {
            const auto frames = p.record_frames.load(std::memory_order_acquire);
            if (frames < 2) {
                set_status("Custom animation: nothing has been recorded yet.");
                return;
            }
            std::string detail;
            if (!multiplayer::install_entity_hooks(base, detail)) {
                set_status("Custom animation: the animation hook is unavailable: " + detail);
                return;
            }
            multiplayer::set_pose_playback_listener(&on_pose_evaluated);
            p.base = base;
            p.component.store(component, std::memory_order_release);
            p.recording.store(false, std::memory_order_release);
            Clip clip;
            clip.joints = expected_skater_joints;
            clip.frames = frames;
            clip.fps = p.record_fps;
            clip.data.assign(p.record_data.begin(),
                             p.record_data.begin() + static_cast<std::size_t>(frames) * clip.joints * floats_per_joint);
            const auto fps = clip.fps;
            {
                std::lock_guard lock(p.mutex);
                p.clip = std::move(clip);
            }
            p.test.store(false, std::memory_order_relaxed);
            p.clip_name = "recorded";
            p.saved_pose.clear();
            p.saved_joints = 0;
            if (const auto pose = pose_location(base, component); pose.buffer && pose.joints > 2) {
                p.saved_pose.resize(static_cast<std::size_t>(pose.joints) * 0x30);
                if (readable(pose.buffer, p.saved_pose.data(), p.saved_pose.size())) p.saved_joints = pose.joints;
                else p.saved_pose.clear();
            }
            p.writes.store(0, std::memory_order_release);
            p.started = GetTickCount64();
            p.playing.store(true, std::memory_order_release);
            set_status("Playing the recorded pose (" + std::to_string(frames) + " frames).");
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Custom animation: playing the recorded pose ({} frames, {:.1f} fps).", frames, fps);
            return;
        }
        if (pending_test) {
            p.clip = {};
            p.test.store(true, std::memory_order_relaxed);
            p.clip_name = "test";
        } else {
            Clip clip;
            std::string error;
            if (!load_clip(pending_clip, clip, error)) {
                set_status("Custom animation: " + error + ".");
                logging::log(logging::Level::warning, logging::Channel::skater, "Custom animation: {}", error);
                return;
            }
            {
                std::lock_guard lock(p.mutex);
                p.clip = std::move(clip);
            }
            p.test.store(false, std::memory_order_relaxed);
            p.clip_name = pending_clip;
        }
        std::string detail;
        if (!multiplayer::install_entity_hooks(base, detail)) {
            set_status("Custom animation: the animation hook is unavailable: " + detail);
            return;
        }
        multiplayer::set_pose_playback_listener(&on_pose_evaluated);
        p.base = base;
        // Capture the current pose so stop can put it back.
        p.saved_pose.clear();
        p.saved_joints = 0;
        if (const auto pose = pose_location(base, component); pose.buffer && pose.joints > 2) {
            p.saved_pose.resize(static_cast<std::size_t>(pose.joints) * 0x30);
            if (readable(pose.buffer, p.saved_pose.data(), p.saved_pose.size())) p.saved_joints = pose.joints;
            else p.saved_pose.clear();
        }
        p.component.store(component, std::memory_order_release);
        p.writes.store(0, std::memory_order_release);
        p.started = GetTickCount64();
        p.playing.store(true, std::memory_order_release);
        set_status("Custom animation playing: " + p.clip_name + ".");
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Custom animation: playing {} on component {:#x}.", p.clip_name, component);
    } catch (const std::exception &e) {
        set_status(std::string("Custom animation: ") + e.what());
    } catch (...) {
        set_status("Custom animation: unknown failure.");
    }
}

} // namespace dingosdk::skater
