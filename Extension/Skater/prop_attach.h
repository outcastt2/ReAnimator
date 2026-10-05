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
//   prop watch boombox 15        arm a field watch for that many seconds
//
// The watch samples the plausible state -- the gesture items, the local player,
// the skater entity and its animation component, the holder and the rig -- five
// times a second and logs every word that changes. Press the gesture during the
// window; the log then names the offset whose value becomes that gesture's id,
// which is the request path. Nothing is written until that is identified.
namespace dingosdk::skater {

void request_prop_report(std::string filter);
void request_prop_watch(std::string filter, unsigned seconds);
// Bump the watch phase: the log tags each changed address with the phase it
// first changed in, so an idle phase and a pressing phase can be diffed.
void request_prop_mark();
std::string prop_status();
// Per-frame service on the client thread, beside the other skater ticks.
void tick_prop_attach(std::uintptr_t base, std::uintptr_t client) noexcept;

} // namespace dingosdk::skater
