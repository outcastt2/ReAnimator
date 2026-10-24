#include "custom_animation.h"

#include "pose_layers.h"
#include "Extension/AnimationStudio/anim_core.h"
#include "Extension/Multiplayer/Remote/native_skater_internal.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/skater_animation.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Custom animation playback, on the game's own animation system (skater_animation): this unit owns
// the .rska file, the per-clip floor opt-in and the mask, and the game does the blending, the floor
// and the drawing. Before ReSkate 2.1.2 this wrote the output pose itself; that pose override is
// Animation Studio's now, and skater_animation owns the one listener it installs.
namespace dingosdk::skater {
namespace {
namespace nsd = multiplayer::native_skater_detail;
using nsd::ptr;
using nsd::readable;

using anim_studio::Clip;
using anim_studio::Joint;

constexpr std::size_t floats_per_joint = 10;      // scale.xyz, quat.xyzw, pos.xyz
constexpr std::uint32_t max_clip_frames = 100000;
constexpr float clip_blend = 0.15f;               // in and out of a clip, seconds
constexpr std::uint64_t mask_ramp_ms = 180;       // the automatic mask's ramp
constexpr float test_seconds = 1.5f;              // the built-in test recording's length
// The joints a clip leaves to the game under the mask. The spine at joint 42 owns 42..340: torso,
// neck, head and both arms. Everything outside that -- the world placement at 1, the helper
// clusters the engine hangs off it (2..6 and 375..394), the pelvis and both legs -- belongs to the
// game (the ranges pose_layers documents and its test guards).
constexpr std::uint32_t mask_first_joint = 42, mask_last_joint = 340;

// poseanim's own state. What is drawn is the game's now; this says what to play.
struct State {
    std::mutex mutex;                 // name, recorded, floor_by_clip
    std::string name;                 // what was asked for, for status
    std::shared_ptr<const Clip> recorded;                 // the last recording, for "play"
    std::unordered_map<std::string, bool> floor_by_clip;  // per clip: off unless asked for
    bool test_pending{};              // "test": play the recording when it lands
    std::atomic<bool> trace{};
    std::atomic<float> ik{1.0f};      // 0 turns the game's floor solve off, even for a clip that opted in
    std::atomic<int> mask{static_cast<int>(PoseMask::automatic)};
    // The automatic mask's ramp. The client thread's only (tick_pose_playback).
    float mask_weight{};
    std::uint64_t mask_at{};
    float mask_applied{-1.0f};
    int mask_applied_mode{-1};
    std::atomic<bool> dump_request{};
};
State& state() { static State s; return s; }

std::mutex& status_mutex() { static std::mutex m; return m; }
std::string& status_text() { static std::string s; return s; }
void set_status(std::string text) {
    std::lock_guard lock(status_mutex());
    status_text() = std::move(text);
}

// The game directory, from this process's own module path.
std::filesystem::path game_directory() {
    std::array<wchar_t, 32768> buffer{};
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(buffer.data()).parent_path();
}

// Clips live in <game>/CustomAnimations, so a bare name is enough. The console treats a backslash
// as an escape, so forward slashes are accepted and a name with no directory is looked up there.
// `.rska` is assumed when the name has no extension.
std::filesystem::path resolve_clip_path(std::string_view name, bool for_write) {
    std::wstring text;
    for (const char c : name) text.push_back(static_cast<unsigned char>(c) < 128 ? wchar_t(c) : L'?');
    for (auto& c : text) if (c == L'/') c = L'\\';
    std::filesystem::path path(text);
    if (!path.has_parent_path()) {
        const auto directory = game_directory() / L"CustomAnimations";
        if (for_write) {
            std::error_code error;
            std::filesystem::create_directories(directory, error);
        }
        path = directory / path;
    }
    if (!path.has_extension()) path += L".rska";
    return path;
}

// An .rska clip: 20-byte header ("RSKA", version 1, joints, frames, fps) then frames * joints * 10
// floats, scale.xyz quat.xyzw pos.xyz, parent-local. It is anim_studio::Clip's shape already, so the
// read is a repack: seconds from the frame rate, one Joint per joint per frame.
bool load_rska(const std::filesystem::path& path, Clip& clip, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path.string(); return false; }
    std::array<char, 20> header{};
    in.read(header.data(), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size()) || std::memcmp(header.data(), "RSKA", 4) != 0) {
        error = "not an .rska clip: " + path.string();
        return false;
    }
    std::uint32_t version{}, joints{}, frame_count{};
    float fps{};
    std::memcpy(&version, header.data() + 4, 4);
    std::memcpy(&joints, header.data() + 8, 4);
    std::memcpy(&frame_count, header.data() + 12, 4);
    std::memcpy(&fps, header.data() + 16, 4);
    if (version != 1 || frame_count == 0 || frame_count > max_clip_frames || !(fps > 0.0f && fps <= 240.0f)) {
        error = "unsupported clip header in " + path.string();
        return false;
    }
    if (joints != anim_studio::skeleton_joints) {
        error = "the clip has " + std::to_string(joints) + " joints; the skater's animation output has " +
                std::to_string(anim_studio::skeleton_joints);
        return false;
    }
    std::vector<float> data(static_cast<std::size_t>(frame_count) * joints * floats_per_joint);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
    if (in.gcount() != static_cast<std::streamsize>(data.size() * sizeof(float))) {
        error = "clip data is truncated in " + path.string();
        return false;
    }
    clip = {};
    clip.joints = joints;
    clip.times.resize(frame_count);
    clip.frames.resize(static_cast<std::size_t>(frame_count) * joints);
    for (std::uint32_t f = 0; f < frame_count; ++f) {
        clip.times[f] = static_cast<float>(f) / fps;
        const float* src = data.data() + static_cast<std::size_t>(f) * joints * floats_per_joint;
        Joint* dst = clip.frames.data() + static_cast<std::size_t>(f) * joints;
        for (std::uint32_t j = 0; j < joints; ++j, src += floats_per_joint) {
            Joint joint;
            joint.scale = {src[0], src[1], src[2]};
            joint.rotation = {src[3], src[4], src[5], src[6]};
            joint.position = {src[7], src[8], src[9]};
            if (!anim_studio::finite(joint)) {
                error = "the clip has a non-finite joint at frame " + std::to_string(f) + ", joint " + std::to_string(j);
                return false;
            }
            dst[j] = joint;
        }
    }
    return true;
}

// Is the local skater off the board? The selector's own motion object carries the off-board vtable
// while he walks, and the board's while he rides.
bool skater_off_board(std::uintptr_t base, std::uintptr_t component) noexcept {
    if (!base || !component) return false;
    try {
        const auto core = ptr(component, 0x70);
        if (!core) return false;
        const auto motion = ptr(core, 0x3b0);
        return motion != 0 && ptr(motion) == base + addr::offboard_flight::offboard_flight_vtable;
    } catch (...) {
        return false;
    }
}

// ---- the mask -------------------------------------------------------------------------------------
// Per joint, how much of the game's own pose to keep: 1 is the game's, 0 is the clip's.
std::shared_ptr<const std::vector<float>> build_mask(float keep) {
    if (!(keep > 0.0f)) return {};
    std::vector<float> joints(anim_studio::skeleton_joints, 0.0f);
    for (std::uint16_t j = 0; j < anim_studio::skeleton_joints; ++j)
        if (j < mask_first_joint || j > mask_last_joint) joints[j] = keep;
    return std::make_shared<const std::vector<float>>(std::move(joints));
}

// Push the mask the mode and the ramp call for, when it changed. The client thread's.
void apply_mask() {
    auto& s = state();
    const auto mode = static_cast<PoseMask>(s.mask.load(std::memory_order_relaxed));
    const float keep = mode == PoseMask::legs ? 1.0f : mode == PoseMask::automatic ? s.mask_weight : 0.0f;
    if (static_cast<int>(mode) == s.mask_applied_mode && std::fabs(keep - s.mask_applied) < 0.01f) return;
    s.mask_applied_mode = static_cast<int>(mode);
    s.mask_applied = keep;
    skater_animation::set_mask(build_mask(keep));
}

// ---- playback -------------------------------------------------------------------------------------
bool floor_enabled(const std::string& name) {
    auto& s = state();
    if (!(s.ik.load(std::memory_order_relaxed) > 0.0f)) return false;
    std::lock_guard lock(s.mutex);
    const auto it = s.floor_by_clip.find(name);
    return it != s.floor_by_clip.end() && it->second;
}

void play_clip(std::shared_ptr<const Clip> clip, const std::string& name, float at, bool loop) {
    skater_animation::Play play;
    play.clip = std::move(clip);
    play.speed = 1.0f;
    play.blend = clip_blend;
    play.loop = loop;
    play.at = at;
    play.paused = false;
    play.floor = floor_enabled(name);
    skater_animation::play(std::move(play));
}

// The skeleton resource beside the log (read-only): the joint names and parents a clip is written
// against, for when a clip lands on the wrong joints.
std::string write_skeleton_dump(std::uintptr_t base, std::uintptr_t client) {
    const auto component = local_skater_component(base, client);
    if (!component) return "the local skater's animation component is unavailable.";
    std::uintptr_t holder{};
    if (!readable(component + 0xa0, &holder, 8) || !holder) return "the animation holder is unavailable.";
    std::uintptr_t rig{};
    if (!readable(holder + 0x78, &rig, 8) || !rig) return "the animation rig is unavailable.";
    std::uintptr_t definition{};
    if (!readable(rig + 0x18, &definition, 8) || !definition) return "the animation definition is unavailable.";
    std::uintptr_t resource{};
    if (!readable(definition + 0x1a0, &resource, 8) || !resource) return "the skeleton resource is unavailable.";
    std::uint32_t count{};
    if (!readable(resource + 0xc, &count, 4)) return "the skeleton bone count is unavailable.";
    constexpr std::size_t window = 0x80000;
    std::vector<std::byte> bytes(window);
    if (!readable(resource, bytes.data(), bytes.size())) return "the skeleton resource could not be read.";
    const auto path = logging::status().directory / L"skeleton_dump.bin";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot write the skeleton dump.";
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Skeleton dump: component={:#x} holder={:#x} rig={:#x} definition={:#x} resource={:#x} joints={}.",
                 component, holder, rig, definition, resource, count);
    return "Skeleton dumped: " + std::to_string(count) + " joints -> skeleton_dump.bin";
}

} // namespace

// ---- the console surface --------------------------------------------------------------------------
void request_pose_playback(std::string name) {
    auto& s = state();
    if (name == "test") {
        // No file: record a moment of the live pose and loop it. It exercises the whole path
        // (record -> clip -> play -> draw) on the machine it runs on.
        if (!skater_animation::record(test_seconds)) {
            set_status("Custom animation: a recording is already running.");
            return;
        }
        {
            std::lock_guard lock(s.mutex);
            s.test_pending = true;
            s.name = "test";
        }
        set_status("Custom animation: recording the test clip...");
        return;
    }
    Clip loaded;
    std::string error;
    const auto path = resolve_clip_path(name, false);
    if (!load_rska(path, loaded, error)) {
        set_status("Custom animation: " + error);
        logging::log(logging::Level::warning, logging::Channel::skater, "Custom animation: cannot play \"{}\": {}.",
                     name, error);
        return;
    }
    auto clip = std::make_shared<const Clip>(std::move(loaded));
    {
        std::lock_guard lock(s.mutex);
        s.name = name;
        s.test_pending = false;
    }
    const bool floor = floor_enabled(name);
    play_clip(clip, name, 0.0f, true);
    set_status(std::format("Custom animation: playing \"{}\" ({} frames, {:.2f} s{}{}).", name, clip->times.size(),
                           clip->duration(), floor ? ", on the floor" : "",
                           static_cast<PoseMask>(s.mask.load(std::memory_order_relaxed)) == PoseMask::full ? "" :
                               ", legs masked"));
}

void request_pose_playback_stop() {
    auto& s = state();
    skater_animation::play(std::nullopt);
    {
        std::lock_guard lock(s.mutex);
        s.name.clear();
        s.test_pending = false;
    }
    set_status("Custom animation: stopped.");
}

void set_pose_clip_ik(std::string_view clip, bool ik) noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.floor_by_clip[std::string(clip)] = ik;
    } catch (...) {}
}

bool pose_clip_ik(std::string_view clip) noexcept {
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        const auto it = s.floor_by_clip.find(std::string(clip));
        return it != s.floor_by_clip.end() && it->second;
    } catch (...) {
        return false;
    }
}

void set_pose_mask(PoseMask mask) noexcept {
    state().mask.store(static_cast<int>(mask), std::memory_order_relaxed);
}

std::string pose_mask_name() {
    switch (static_cast<PoseMask>(state().mask.load(std::memory_order_relaxed))) {
    case PoseMask::full: return "full";
    case PoseMask::legs: return "legs";
    default: return "auto";
    }
}

void request_pose_record(float seconds) {
    if (!skater_animation::record(seconds)) {
        set_status("Custom animation: a recording is already running.");
        return;
    }
    set_status(std::format("Custom animation: recording for {:.1f} s...", seconds));
}

void request_pose_record_playback() {
    auto& s = state();
    std::shared_ptr<const Clip> clip;
    {
        std::lock_guard lock(s.mutex);
        clip = s.recorded;
        if (clip) s.name = "recording";
    }
    if (!clip) {
        set_status("Custom animation: nothing has been recorded yet (poseanim record).");
        return;
    }
    play_clip(clip, "recording", 0.0f, true);
    set_status(std::format("Custom animation: playing the recording ({} frames, {:.2f} s).", clip->times.size(),
                           clip->duration()));
}

std::string save_recorded_clip(std::string_view path) {
    std::shared_ptr<const Clip> clip;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        clip = s.recorded;
    }
    if (!clip || clip->times.size() < 2 || clip->joints == 0) return "nothing has been recorded yet";
    const auto resolved = resolve_clip_path(path, true);
    std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot write " + resolved.string();
    const std::uint32_t version = 1;
    const std::uint32_t joints = clip->joints;
    const std::uint32_t frame_count = static_cast<std::uint32_t>(clip->times.size());
    float fps = 60.0f;
    if (clip->duration() > 0.0f) fps = static_cast<float>(frame_count - 1) / clip->duration();
    if (!(fps >= 1.0f && fps <= 240.0f)) fps = 60.0f;
    std::array<char, 20> header{};
    std::memcpy(header.data(), "RSKA", 4);
    std::memcpy(header.data() + 4, &version, 4);
    std::memcpy(header.data() + 8, &joints, 4);
    std::memcpy(header.data() + 12, &frame_count, 4);
    std::memcpy(header.data() + 16, &fps, 4);
    out.write(header.data(), header.size());
    std::vector<float> row(static_cast<std::size_t>(joints) * floats_per_joint);
    for (std::uint32_t f = 0; f < frame_count; ++f) {
        const Joint* frame = clip->frames.data() + static_cast<std::size_t>(f) * joints;
        for (std::uint32_t j = 0; j < joints; ++j) {
            float* dst = row.data() + static_cast<std::size_t>(j) * floats_per_joint;
            std::memcpy(dst, frame[j].scale.data(), 12);
            std::memcpy(dst + 3, frame[j].rotation.data(), 16);
            std::memcpy(dst + 7, frame[j].position.data(), 12);
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(float)));
    }
    if (!out) return "cannot write " + resolved.string();
    logging::log(logging::Level::info, logging::Channel::skater, "Custom animation: saved {} frames to {}.",
                 frame_count, resolved.string());
    return {};
}

void request_skeleton_dump() { state().dump_request.store(true, std::memory_order_release); }

std::string pose_playback_status() {
    std::string name;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        name = s.name;
    }
    std::string text = name.empty() ? "Custom animation: idle" : "Custom animation: " + name;
    if (const auto at = skater_animation::play_time()) text += std::format(" at {:.2f} s", *at);
    text += name.empty() ? "" : skater_animation::playing() ? ", playing" : ", stopped";
    return text + ". " + skater_animation::status();
}

std::string pose_playback_cost() {
    return "Per-frame work: the game's own animation update - poseanim adds no pass of its own.";
}

void set_pose_trace(bool enabled) noexcept { state().trace.store(enabled, std::memory_order_relaxed); }
bool pose_trace() { return state().trace.load(std::memory_order_relaxed); }

void set_pose_ik(float strength) noexcept {
    state().ik.store(std::clamp(strength, 0.0f, 1.0f), std::memory_order_relaxed);
}
float pose_ik() noexcept { return state().ik.load(std::memory_order_relaxed); }

void tick_pose_playback(std::uintptr_t base, std::uintptr_t client) noexcept {
    auto& s = state();
    try {
        if (s.dump_request.exchange(false, std::memory_order_acq_rel)) {
            const auto text = write_skeleton_dump(base, client);
            set_status(text);
            logging::log(logging::Level::info, logging::Channel::skater, "{}", text);
        }
        // A finished recording is ours to keep: take_recording hands it over once.
        if (!skater_animation::recording()) {
            if (auto done = skater_animation::take_recording()) {
                bool test = false;
                {
                    std::lock_guard lock(s.mutex);
                    s.recorded = done;
                    test = s.test_pending;
                    if (test) {
                        s.test_pending = false;
                        s.name = "test";
                    }
                }
                if (test) {
                    play_clip(done, "test", 0.0f, true);
                    set_status(std::format("Custom animation: test clip recorded ({} frames, {:.2f} s); playing.",
                                           done->times.size(), done->duration()));
                } else {
                    set_status(std::format("Custom animation: recorded {} frames, {:.2f} s (poseanim play / save).",
                                           done->times.size(), done->duration()));
                }
            }
        }
        // The automatic mask: the game keeps the legs while the skater rides. The physics state
        // machine is the truth for "on foot" (504 walking, 300 a ground wipeout, anything else
        // riding); until the selector has chosen once, the motion object answers instead.
        const auto component = local_skater_component(base, client);
        const auto physics = observed_physics_state();
        const bool riding = component && (physics == UINT32_MAX
            ? !skater_off_board(base, component)
            : !(physics == addr::no_bail::offboard_physics_state || physics == addr::no_bail::wipeout_physics_state));
        const auto now = GetTickCount64();
        const auto elapsed = now > s.mask_at ? now - s.mask_at : 0;
        s.mask_at = now;
        const auto mode = static_cast<PoseMask>(s.mask.load(std::memory_order_relaxed));
        const bool engaged = mode == PoseMask::legs || (mode == PoseMask::automatic && riding);
        s.mask_weight = layers::ramp_weight(s.mask_weight, engaged ? 1.0f : 0.0f, elapsed, mask_ramp_ms);
        apply_mask();
        // Trace: one status line every two seconds while a clip plays.
        if (s.trace.load(std::memory_order_relaxed) && skater_animation::playing()) {
            static std::uint64_t next_trace = 0;
            if (now >= next_trace) {
                next_trace = now + 2000;
                logging::log(logging::Level::info, logging::Channel::skater, "Custom animation: {}.",
                             pose_playback_status());
            }
        }
    } catch (...) {}
}

} // namespace dingosdk::skater
