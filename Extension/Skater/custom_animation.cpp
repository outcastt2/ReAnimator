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
constexpr std::size_t floats_per_joint = 10; // scale.xyz, quat.xyzw, pos.xyz

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
    std::uintptr_t base{};
    ULONGLONG started{};
    bool pending{}, stop{}, pending_test{};
    std::string pending_clip;
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

// The output pose buffer for a component, or 0.
std::uintptr_t pose_buffer(std::uintptr_t base, std::uintptr_t component) {
    std::uintptr_t holder{};
    if (!readable(component + 0xa0, &holder, 8) || !holder) return 0;
    const auto reader = [](std::uintptr_t address, void *out, std::size_t size) {
        return readable(address, out, size);
    };
    const auto pose = multiplayer::read_native_pose_layout(reader, base, holder, skater_joint_bound);
    return pose.buffer;
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

void on_pose_evaluated(std::uintptr_t component) noexcept {
    try {
        auto &p = playback();
        if (!p.playing.load(std::memory_order_acquire)) return;
        if (component != p.component.load(std::memory_order_acquire)) return;
        const auto buffer = pose_buffer(p.base, component);
        if (!buffer) return;
        const auto elapsed_ms = GetTickCount64() - p.started;
        if (p.test.load(std::memory_order_relaxed)) {
            // A procedural head bob: read the evaluated pose and move the head.
            std::array<float, 12> bone{};
            if (!readable(buffer + test_joint * 0x30ULL, bone.data(), sizeof(bone))) return;
            const float phase = static_cast<float>(elapsed_ms) * 0.004f;
            bone[8] += 0.0f;
            bone[9] += 0.35f * std::sin(phase);
            bone[10] += 0.20f * std::cos(phase * 0.7f);
            (void)nsd::write(buffer + test_joint * 0x30ULL, bone.data(), sizeof(bone));
            return;
        }
        std::lock_guard lock(p.mutex);
        if (p.clip.frames == 0 || p.clip.data.empty()) return;
        const auto frame = static_cast<std::uint32_t>(
            (static_cast<double>(elapsed_ms) * p.clip.fps / 1000.0)) % p.clip.frames;
        write_frame(buffer, p.clip.data.data() + static_cast<std::size_t>(frame) * p.clip.joints * floats_per_joint,
                    p.clip.joints);
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
            p.playing.store(false, std::memory_order_release);
            p.component.store(0, std::memory_order_release);
            set_status("Custom animation stopped.");
            logging::log(logging::Level::info, logging::Channel::skater, "Custom animation: stopped.");
            return;
        }
        if (!pending) return;
        // The local skater's animation component.
        std::uintptr_t component{};
        try {
            nsd::require(ptr(client) == base + addr::engine::client_vtable, "Local client is unavailable.");
            const auto context = ptr(client, 8);
            const auto offset = read<std::uint32_t>(base, addr::engine::context_player_manager_offset);
            nsd::require(offset <= 0x1000000, "Player manager offset changed.");
            const auto manager = ptr(context, offset);
            const auto begin = ptr(manager, 0x4c8);
            const auto player = ptr(begin);
            const auto entity = ptr(player, 0xb8);
            component = ptr(entity, 0x628);
        } catch (const std::exception &e) {
            set_status(std::string("Custom animation: ") + e.what());
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
        p.component.store(component, std::memory_order_release);
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
