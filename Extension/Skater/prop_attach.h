#pragma once
#include <cstdint>
#include <string>

// Hand props, the game's own way.
//
// A vanilla gesture attaches its prop or effect to the skeleton, and because the
// attachment follows the bones it follows a custom animation too: confirmed in
// game, the selfie phone stays glued to the hand through custom playback, lasts
// as long as the gesture, and the same holds for the other prop gestures
// (boardboombox, chainsaw, sadtrombone, handpuppets, snakeflutecharmer).
//
// What that leaves is the trigger. The player pressing the emote wheel is the
// only path known today; nothing in this codebase starts a gesture, and the
// gesture item data (id at +0x20, layered +0x24, stationary +0x25) is
// permission state, not a request. Writing a guess at a request field could
// take the game down, so this module watches first:
//
//   prop list                    the loaded gesture items and their ids
//   prop watch selfie 60         watch for a minute; after a ten second idle
//                                phase it flips to phase 1 by itself, and the
//                                gesture is pressed then
//
// The watch samples the plausible state -- the gesture items, the local player,
// the skater entity and its components, the holder and the whole animation
// instance -- on every client frame and logs the first change of every address,
// tagged with the phase it happened in. The animation graph churns while idle,
// so the finding is the set difference: an address that changes in the marked
// phase and not in the idle one. Nothing is written until that is identified.
namespace dingosdk::skater {

void request_prop_report(std::string filter);
void request_prop_watch(std::string filter, unsigned seconds);
// Bump the watch phase: the log tags each changed address with the phase it
// first changed in, so an idle phase and a pressing phase can be diffed.
void request_prop_mark();
// Watch the animation layer cluster every frame and keep the whole timeline.
// Layer weights are floats; 1.0 means a layer is fully active, which is what a
// gesture (and its prop) looks like from the outside.
void request_prop_weight(unsigned seconds);
// Watch the output pose joint by joint. If the phone hangs off a socket joint,
// that joint moves when the phone appears, and this is the watch that shows it.
void request_prop_pose(unsigned seconds);
std::string prop_status();
// Per-frame service on the client thread, beside the other skater ticks.
void tick_prop_attach(std::uintptr_t base, std::uintptr_t client) noexcept;

} // namespace dingosdk::skater
