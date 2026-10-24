#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Custom animation playback ("poseanim"): .rska clips on the local skater, through the game's own
// animation system (Extension/Skater/skater_animation.h, Animation Studio's pose override).
//
// A clip is a sequence of full 395-joint poses, each {scale.xyz, quat.xyzw, pos.xyz}, parent-local
// exactly as the engine produces it -- the same shape as anim_studio::Clip, which is what this
// hands to skater_animation::play. Nothing here writes the pose: the game blends the clip in and
// out and puts it on the floor (Play::floor), so what this unit owns is the file format, the
// per-clip floor opt-in and the mask.
namespace dingosdk::skater {

// Start playing `clip`. "test" runs a built-in procedural animation; anything
// else is a path to a .rska clip file.
void request_pose_playback(std::string clip);
void request_pose_playback_stop();
// Floor correction is opt-in per clip: off unless `poseanim <file> ik` was
// given, remembered per clip name, so re-running `poseanim <file>` after a
// quickswap re-export keeps the flag; `set_pose_clip_ik(name, false)` (or
// `poseanim <file> noik`) clears it. It is the game's own floor placement
// (skater_animation::Play::floor), which puts the clip's board under the feet
// before the takeoff and after the landing.
void set_pose_clip_ik(std::string_view clip, bool ik) noexcept;
bool pose_clip_ik(std::string_view clip) noexcept;
// Which joints a custom animation is allowed to drive. The game can keep the
// pelvis and both legs, so a clip layers on top of a board stance the way the
// game's own moving gestures do.
//   automatic -- hand the legs back while riding (the default)
//   full      -- the clip drives the whole body
//   legs      -- always keep the game's legs and pelvis
enum class PoseMask { automatic, full, legs };
void set_pose_mask(PoseMask mask) noexcept;
std::string pose_mask_name();
// Record the live pose as it is evaluated, then play it back with "play".
void request_pose_record(float seconds = 5.0f);
void request_pose_record_playback();
// Write the recorded clip to an .rska file. Returns an error message, or empty
// on success.
std::string save_recorded_clip(std::string_view path);
// Queue a dump of the live skeleton resource beside the log (read-only).
void request_skeleton_dump();
std::string pose_playback_status();
// What this tool's per-frame work actually costs: it has none of its own now.
std::string pose_playback_cost();
// The game's own clip probe in the log, every two seconds while a clip plays.
void set_pose_trace(bool enabled) noexcept;
bool pose_trace();
// Floor correction strength, 0..1 (0 off, the default 1). Kept for the console
// surface: the solve is the game's now, so this is an on/off gate for it.
void set_pose_ik(float strength) noexcept;
float pose_ik() noexcept;
// Per-frame housekeeping on the client thread (the automatic mask, recordings).
void tick_pose_playback(std::uintptr_t base, std::uintptr_t client) noexcept;

} // namespace dingosdk::skater
