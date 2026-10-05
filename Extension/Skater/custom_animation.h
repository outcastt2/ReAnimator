#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Route B: play a baked custom animation by overwriting the local skater's
// output pose every evaluation. This is the same buffer the multiplayer code
// writes for remote skaters (Extension/Multiplayer/Remote/native_skater.cpp).
//
// A clip is a sequence of full 395-joint poses, each {scale.xyz, quat.xyzw,
// pos.xyz}. The pose is parent-local, exactly as the engine produces it.
namespace dingosdk::skater {

// Start playing `clip`. "test" runs a built-in procedural animation; anything
// else is a path to a .rska clip file.
void request_pose_playback(std::string clip);
void request_pose_playback_stop();
// Record the live pose as it is evaluated, then play it back with "play".
void request_pose_record();
void request_pose_record_playback();
// Write the recorded clip to an .rska file. Returns an error message, or empty
// on success.
std::string save_recorded_clip(std::string_view path);
// Queue a dump of the live skeleton resource beside the log (read-only).
void request_skeleton_dump();
std::string pose_playback_status();
// Per-frame housekeeping on the client thread (start/stop, local component).
void tick_pose_playback(std::uintptr_t base, std::uintptr_t client) noexcept;
// The animation-evaluation listener: overwrites the pose before it is rendered.
void on_pose_evaluated(std::uintptr_t component) noexcept;

} // namespace dingosdk::skater
