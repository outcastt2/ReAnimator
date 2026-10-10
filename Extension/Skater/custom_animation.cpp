#include "custom_animation.h"
#include "pose_layers.h"
#include "Extension/Multiplayer/Remote/native_skater_internal.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Skater/no_bail.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
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

// ---------------------------------------------------------------------------
// Masking: which joints the game keeps when a custom animation layers on top of
// locomotion. The ranges and the write itself live in pose_layers.cpp, which is
// checked on the host by Extension/Skater/Test/pose_layer_tests.cpp.
//
// A layer switch is ramped rather than cut, so starting to walk mid-gesture does
// not snap the legs.
constexpr ULONGLONG mask_ramp_ms = 200;
// Above this the automatic mask decides the skater is walking or riding.
constexpr float walking_speed = 0.6f;

// The skater is on foot exactly when their motion state (core+0x3b0) is the
// offboard flight state, which owns walking, sliding and falling. While riding,
// that slot holds a different object with a different vtable -- the same check
// the noclip code makes. A read that fails counts as "on the board": riding is
// the common case, and masking is the safe way to be wrong there.
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
    std::uint32_t record_joints{};
    ULONGLONG record_started{};
    float record_fps{60.0f};
    bool dump_request{};
    // Masking. `mask` is set from the console; the rest is only touched by the
    // write path, which runs on the animation thread.
    std::atomic<int> mask{static_cast<int>(PoseMask::automatic)};
    float mask_weight{};
    ULONGLONG mask_updated{};
    std::array<float, 3> world_sample{};
    ULONGLONG world_sampled{};
    bool world_valid{};
    float speed{};
    bool moving{};
    bool on_board{};
    bool mask_engaged{};
    std::uint32_t physics_state{UINT32_MAX};
    bool offboard_motion{};
    ULONGLONG summary_at{};
    // Diagnostics. The geometry summary is off unless `poseanim trace 1` asks for
    // it, and the handler cost is always measured: this tool runs inside the
    // engine's frame, so it should be able to say what it costs.
    std::atomic<bool> trace{};
    std::atomic<std::uint64_t> handler_ticks{};
    std::atomic<std::uint64_t> handler_peak{};
    std::atomic<std::uint32_t> handler_frames{};
};
Playback &playback() { static Playback p; return p; }
std::mutex &status_mutex() { static std::mutex m; return m; }
// The performance counter's frequency, read once. Every frame this tool spends
// inside the engine is measured against it.
std::uint64_t performance_frequency() noexcept {
    static const std::uint64_t value = [] {
        LARGE_INTEGER frequency{};
        return static_cast<std::uint64_t>(QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0
                                              ? frequency.QuadPart
                                              : 1);
    }();
    return value;
}
// Times the per-frame handler and folds the result into the playback record.
struct HandlerTimer {
    Playback &owner;
    LARGE_INTEGER begin{};
    explicit HandlerTimer(Playback &value) noexcept : owner(value) { QueryPerformanceCounter(&begin); }
    ~HandlerTimer() noexcept {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        const auto elapsed = static_cast<std::uint64_t>(end.QuadPart - begin.QuadPart);
        owner.handler_ticks.fetch_add(elapsed, std::memory_order_relaxed);
        owner.handler_frames.fetch_add(1, std::memory_order_relaxed);
        auto peak = owner.handler_peak.load(std::memory_order_relaxed);
        while (elapsed > peak &&
               !owner.handler_peak.compare_exchange_weak(peak, elapsed, std::memory_order_relaxed)) {
        }
    }
};
void set_status(const std::string &text) {
    std::lock_guard lock(status_mutex());
    playback().status = text;
}

// The game directory, from this process's own module path.
std::filesystem::path game_directory() {
    std::array<wchar_t, 32768> buffer{};
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) return {};
    return std::filesystem::path(buffer.data()).parent_path();
}
// Clips live in <game>/CustomAnimations, so a bare name is enough. The console
// treats a backslash as an escape, so forward slashes are accepted and a name
// with no directory is looked up in that folder. `.rska` is assumed when the
// name has no extension.
std::filesystem::path resolve_clip_path(std::string_view name, bool for_write) {
    std::wstring text;
    for (const char c : name) text.push_back(static_cast<unsigned char>(c) < 128 ? wchar_t(c) : L'?');
    for (auto &c : text) if (c == L'/') c = L'\\';
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

bool load_clip(const std::filesystem::path &path, Clip &clip, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path.string(); return false; }
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

// The rig handed to the post-physics response is the physics rig, not the
// animation rig: no_bail's own resolution reads core = *(component + 0x70),
// rig = *(core + 0x438), and verifies *(rig + 0x4630) == core.
bool rig_is_local(std::uintptr_t rig, std::uintptr_t component) {
    if (!rig || !component) return false;
    const auto core = ptr(component, 0x70);
    return core && ptr(rig, 0x4630) == core;
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

// How much of the game's own pose to keep for the masked joints, ramped so a
// layer switch is a blend rather than a cut.
float mask_weight(Playback &p, ULONGLONG now) noexcept {
    const auto mode = static_cast<PoseMask>(p.mask.load(std::memory_order_relaxed));
    bool engaged = false;
    if (mode == PoseMask::legs) engaged = true;
    else if (mode == PoseMask::automatic) engaged = p.on_board || p.moving;
    if (engaged != p.mask_engaged) {
        p.mask_engaged = engaged;
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Custom animation: mask {} (on board {}, state {}, moving {}, {:.2f} m/s).",
                     engaged ? "engaged" : "released", p.on_board, p.physics_state, p.moving, p.speed);
    }
    const auto elapsed = now > p.mask_updated ? now - p.mask_updated : 0;
    p.mask_updated = now;
    p.mask_weight = layers::ramp_weight(p.mask_weight, engaged ? 1.0f : 0.0f, elapsed, mask_ramp_ms);
    return p.mask_weight;
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

// Writes the current frame of whatever is playing. Shared by the animation
// callback (which the engine's own constraints later rewrite) and the
// post-physics skeleton response, after which the write survives.
namespace {
// A joint's world position relative to joint 1, composed the way the engine
// composes a pose. A normal ride holds the ankles about 0.86 m below joint 1, so
// one log line says whether the pose being kept is a real stance.
bool pose_offset(std::uintptr_t buffer, const std::uint32_t *chain, std::size_t count, float out[3]) noexcept {
    float origin[3]{}, world[3]{};
    const std::uint32_t root[] = {1};
    if (!layers::world_position(buffer, root, 1, origin)) return false;
    if (!layers::world_position(buffer, chain, count, world)) return false;
    for (int c = 0; c < 3; ++c) out[c] = world[c] - origin[c];
    return true;
}

// How the game is moving the skater, sampled once per frame from the pose the
// engine just produced. Joint 1 carries world placement and the game writes it
// every frame, so its speed is the skater's speed; the motion state says
// whether they are on foot.
void sample_motion(Playback &p, const PoseLocation &pose, std::uintptr_t component) noexcept {
    const auto now = GetTickCount64();
    if (pose.buffer && pose.joints > 1) {
        std::array<float, 3> world{};
        if (readable(pose.buffer + 0x30ULL + 0x20ULL, world.data(), sizeof(world))) {
            if (p.world_valid && now > p.world_sampled) {
                const float dt = static_cast<float>(now - p.world_sampled) / 1000.0f;
                if (dt > 1e-3f) {
                    const float dx = world[0] - p.world_sample[0];
                    const float dy = world[1] - p.world_sample[1];
                    const float dz = world[2] - p.world_sample[2];
                    const float instant = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
                    p.speed = 0.7f * p.speed + 0.3f * std::min(instant, 50.0f);
                }
            }
            p.world_sample = world;
            p.world_sampled = now;
            p.world_valid = true;
        }
    }
    // Hysteresis: once walking, keep believing it until the speed really drops,
    // so a skater hovering around the threshold does not flicker between layers.
    p.moving = p.speed > (p.moving ? walking_speed * 0.5f : walking_speed);
    // The physics state machine is the truth for "on foot": 504 is walking and
    // 300 is a ground wipeout (the engine remaps between them), so anything else
    // means riding. The motion object is sampled too -- it is the other way to
    // ask the same question, and the log says when the two disagree.
    const auto state = observed_physics_state();
    p.physics_state = state;
    p.offboard_motion = skater_off_board(p.base, component);
    // Until the selector has chosen once, fall back to the motion object.
    p.on_board = state == UINT32_MAX
        ? !p.offboard_motion
        : !(state == addr::no_bail::offboard_physics_state || state == addr::no_bail::wipeout_physics_state);
    // A geometry summary, only when tracing: one line says what the layer
    // decided and where the legs it kept actually are. Off by default, because
    // a clip that plays for minutes does not need to narrate itself.
    if (p.trace.load(std::memory_order_relaxed) && now >= p.summary_at + 2000 && pose.buffer) {
        p.summary_at = now;
        const std::uint32_t pelvis_chain[] = {1, 7};
        const std::uint32_t left_chain[] = {1, 7, 8, 9, 10};
        const std::uint32_t right_chain[] = {1, 7, 341, 342, 343};
        float pelvis[3]{}, left[3]{}, right[3]{};
        const bool has_pelvis = pose_offset(pose.buffer, pelvis_chain, 2, pelvis);
        const bool has_left = pose_offset(pose.buffer, left_chain, 5, left);
        const bool has_right = pose_offset(pose.buffer, right_chain, 5, right);
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Custom animation: layer: state={} on_board={} moving={} speed={:.2f} m/s mask={:.2f} "
                     "pelvis=({:.3f},{:.3f},{:.3f}) L_ankle=({:.3f},{:.3f},{:.3f}) R_ankle=({:.3f},{:.3f},{:.3f})",
                     p.physics_state, p.on_board, p.moving, p.speed, p.mask_weight,
                     has_pelvis ? pelvis[0] : 0.0f, has_pelvis ? pelvis[1] : 0.0f, has_pelvis ? pelvis[2] : 0.0f,
                     has_left ? left[0] : 0.0f, has_left ? left[1] : 0.0f, has_left ? left[2] : 0.0f,
                     has_right ? right[0] : 0.0f, has_right ? right[1] : 0.0f, has_right ? right[2] : 0.0f);
    }
}

void write_current(Playback &p, std::uintptr_t buffer, std::uint32_t available_joints) noexcept {
    if (!p.playing.load(std::memory_order_acquire) || !buffer) return;
    const auto note_write = [&p] {
        const auto count = p.writes.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count == 10 || count == 100 || count == 1000 || count == 10000)
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Custom animation: pose written ({} times).", count);
    };
    const auto elapsed_ms = GetTickCount64() - p.started;
    if (p.test.load(std::memory_order_relaxed)) {
        const float phase = static_cast<float>(elapsed_ms) * 0.003f;
        // Head scale: the operation first person uses to hide the head.
        std::array<float, 12> head{};
        if (readable(buffer + test_joint * 0x30ULL, head.data(), sizeof(head))) {
            const float scale = 0.5f + 0.45f * std::sin(phase);
            head[0] = head[1] = head[2] = scale;
            (void)nsd::write(buffer + test_joint * 0x30ULL, head.data(), sizeof(head));
        }
        // A rotation on a body joint, to show whether rotations survive.
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
    // Never write past the pose the engine handed us: a joint body is 0x30 bytes
    // and the buffer holds `available_joints` of them. A clip authored against a
    // longer skeleton must not run off the end of a shorter one.
    auto clip_joints = p.clip.joints;
    if (available_joints && clip_joints > available_joints) clip_joints = available_joints;
    if (clip_joints <= 2) return;
    const auto frame = static_cast<std::uint32_t>(
        (static_cast<double>(elapsed_ms) * p.clip.fps / 1000.0)) % p.clip.frames;
    // Joints 0 and 1 are not pose: joint 1 carries the skater's world
    // placement, and the game drives it from the board and physics. Leaving it
    // alone is what keeps the skater on the board, the way a gesture animates
    // in place while the player keeps moving.
    //
    // The legs and pelvis are the same story whenever the mask is engaged: the
    // engine's own pose for them stays, so the feet keep the board and the walk
    // keeps walking.
    const auto keep = mask_weight(p, GetTickCount64());
    layers::write_pose(buffer + 2 * layers::pose_stride,
                       p.clip.data.data() + static_cast<std::size_t>(frame) * p.clip.joints * floats_per_joint +
                           2 * layers::clip_stride,
                       2, clip_joints - 2, keep);
    note_write();
}
} // namespace

// Deliberately does nothing.
//
// This runs after the animation graph and *before* the engine's physics
// response, and on this game that response is the ragdoll: it re-solves the
// rendered pose from the physics, driven by the pose that is in the buffer.
// Writing the clip here -- which the playback used to do -- feeds the clip's
// upper body into that solve, and the solve answers with a different lower
// body: a crouch with the feet off the board, which is exactly what survived
// when the mask was keeping the game's legs.
//
// The write that matters is the one after the response, so the physics only
// ever sees the game's own animation and the clip is applied on top of a body
// the engine is happy with.
void on_pose_evaluated(std::uintptr_t component) noexcept {
    (void)component;
}

// Runs after the engine's own post-physics constraints, on the same update
// order. This is the write that survives to the renderer.
void on_skeleton_responded(std::uintptr_t rig) noexcept {
    HandlerTimer measured(playback());
    try {
        auto &p = playback();
        if (!rig) return;
        const auto component = p.component.load(std::memory_order_acquire);
        if (!rig_is_local(rig, component)) return;
        const auto pose = pose_location(p.base, component);
        if (!pose.buffer) return;
        sample_motion(p, pose, component);
        if (p.recording.load(std::memory_order_acquire)) {
            // Record AFTER the engine's constraints, so a replay reproduces the
            // pose that was actually rendered (feet on the board and all).
            const auto frame = p.record_frames.load(std::memory_order_relaxed);
            if (frame < p.record_capacity && pose.joints) {
                auto *dst = p.record_data.data() + static_cast<std::size_t>(frame) * pose.joints * floats_per_joint;
                pack_frame(pose.buffer, pose.joints, dst);
                p.record_frames.store(frame + 1, std::memory_order_release);
                if (frame == 0) {
                    p.record_joints = pose.joints;
                    logging::log(logging::Level::info, logging::Channel::skater,
                                 "Custom animation: recording {} joints.", pose.joints);
                }
            }
            return;
        }
        write_current(p, pose.buffer, pose.joints);
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

std::string save_recorded_clip(std::string_view path) {
    try {
        auto &p = playback();
        const auto frames = p.record_frames.load(std::memory_order_acquire);
        const auto joints = p.record_joints;
        if (frames < 2 || joints <= 2) return "nothing has been recorded yet";
        const auto resolved = resolve_clip_path(path, true);
        std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot write " + resolved.string();
        std::array<char, 20> header{};
        const std::uint32_t version = 1;
        const float fps = p.record_fps;
        std::memcpy(header.data(), "RSKA", 4);
        std::memcpy(header.data() + 4, &version, 4);
        std::memcpy(header.data() + 8, &joints, 4);
        std::memcpy(header.data() + 12, &frames, 4);
        std::memcpy(header.data() + 16, &fps, 4);
        out.write(header.data(), header.size());
        out.write(reinterpret_cast<const char *>(p.record_data.data()),
                  static_cast<std::streamsize>(static_cast<std::size_t>(frames) * joints * floats_per_joint *
                                               sizeof(float)));
        out.close();
        logging::log(logging::Level::info, logging::Channel::skater,
                     "Custom animation: saved {} frames x {} joints to {}.", frames, joints, resolved.string());
        return {};
    } catch (const std::exception &e) {
        return std::string("save failed: ") + e.what();
    }
}

void request_skeleton_dump() {
    std::lock_guard lock(playback().mutex);
    playback().dump_request = true;
}

namespace {
// The live skeleton resource, beside the log. Read-only: this is the only place
// the joint hierarchy and the bind pose can come from, and neither is in the
// pose buffer.
std::string write_skeleton_dump(std::uintptr_t base, std::uintptr_t client) {
    const auto component = local_component(base, client);
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
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    logging::log(logging::Level::info, logging::Channel::skater,
                 "Skeleton dump: component={:#x} holder={:#x} rig={:#x} definition={:#x} resource={:#x} joints={}.",
                 component, holder, rig, definition, resource, count);
    return "Skeleton dumped: " + std::to_string(count) + " joints at resource 0x" + std::to_string(resource) +
           " -> skeleton_dump.bin";
}
} // namespace
std::string pose_playback_status() {
    std::lock_guard lock(status_mutex());
    return playback().status;
}

std::string pose_playback_cost() {
    auto &p = playback();
    const auto frames = p.handler_frames.load(std::memory_order_relaxed);
    if (!frames) return "Animation handler: not measured yet.";
    const auto frequency = static_cast<double>(performance_frequency());
    const auto average_us = 1e6 * static_cast<double>(p.handler_ticks.load(std::memory_order_relaxed)) / frequency /
                            static_cast<double>(frames);
    const auto peak_us = 1e6 * static_cast<double>(p.handler_peak.load(std::memory_order_relaxed)) / frequency;
    // A 60 Hz frame is 16.67 ms; the share of one is the number that matters.
    const auto share = average_us / 16666.7 * 100.0;
    return std::format(
        "Animation handler: {:.1f} us average, {:.1f} us worst, over {} frames ({:.3f}% of a 60 Hz frame).",
        average_us, peak_us, frames, share);
}

void set_pose_trace(bool enabled) noexcept { playback().trace.store(enabled, std::memory_order_relaxed); }
bool pose_trace() { return playback().trace.load(std::memory_order_relaxed); }

void set_pose_mask(PoseMask mask) noexcept {
    auto &p = playback();
    p.mask.store(static_cast<int>(mask), std::memory_order_relaxed);
    // Ramp on from wherever the last layer left the weight, so changing modes
    // while a clip plays blends instead of jumping.
    p.mask_updated = GetTickCount64();
    logging::log(logging::Level::info, logging::Channel::skater, "Custom animation: masking set to {}.",
                 pose_mask_name());
}

std::string pose_mask_name() {
    switch (static_cast<PoseMask>(playback().mask.load(std::memory_order_relaxed))) {
    case PoseMask::full: return "full";
    case PoseMask::legs: return "legs";
    default: return "auto";
    }
}

void tick_pose_playback(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        auto &p = playback();
        // The post-physics skeleton response is where a pose write survives;
        // No Bail installs that hook at startup on the supported build.
        static std::atomic<bool> armed{};
        if (!armed.exchange(true, std::memory_order_acq_rel))
            dingosdk::set_skeleton_responded_listener(&on_skeleton_responded);
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
        if (p.dump_request) {
            p.dump_request = false;
            set_status(write_skeleton_dump(base, client));
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
        // A new layer starts from a clean motion sample, so the automatic mask
        // decides on this skater's current state rather than the last clip's.
        p.world_valid = false;
        p.moving = false;
        p.speed = 0.0f;
        p.on_board = false;
        p.mask_engaged = false;
        p.physics_state = UINT32_MAX;
        p.mask_updated = GetTickCount64();
        p.summary_at = p.mask_updated;
        p.handler_ticks.store(0, std::memory_order_relaxed);
        p.handler_peak.store(0, std::memory_order_relaxed);
        p.handler_frames.store(0, std::memory_order_relaxed);
        // The write that survives is the one after the engine's post-physics
        // response. That hook belongs to No Bail, which installs it at startup;
        // if it is missing, say so rather than playing invisibly.
        if (!dingosdk::no_bail_available())
            logging::log(logging::Level::warning, logging::Channel::skater,
                         "Custom animation: the post-physics skeleton hook is not installed, so the engine "
                         "will overwrite the pose. The clipped animation will not be visible.");
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
            const auto path = resolve_clip_path(pending_clip, false);
            if (!load_clip(path, clip, error)) {
                set_status("Custom animation: " + error + ".");
                logging::log(logging::Level::warning, logging::Channel::skater, "Custom animation: {}", error);
                return;
            }
            {
                std::lock_guard lock(p.mutex);
                p.clip = std::move(clip);
            }
            p.test.store(false, std::memory_order_relaxed);
            p.clip_name = path.string();
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
                     "Custom animation: playing {} on component {:#x} (mask {}).", p.clip_name, component,
                     pose_mask_name());
    } catch (const std::exception &e) {
        set_status(std::string("Custom animation: ") + e.what());
    } catch (...) {
        set_status("Custom animation: unknown failure.");
    }
}

} // namespace dingosdk::skater
