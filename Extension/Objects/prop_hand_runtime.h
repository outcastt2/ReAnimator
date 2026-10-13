#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace dingosdk::profile_runtime {
// Hand-prop follow: keep one *placed* object at a target transform every park
// tick, using the same native move path the park editor uses while dragging an
// object. The skater side supplies the target (the composed wrist joint, so a
// custom animation drives the prop); this side owns everything about the
// placement: choosing the object, suppressing its layout saves while it is
// held, moving it, and putting it back on release.
//
// Requests are plain structs behind a mutex, written from the client tick. The
// native reads and the move itself happen only in service_prop_hand(), called
// from update_placement_poses() on the park tick under native_mutex -- the
// client and render threads must not change authoritative world components.
void request_prop_hand(std::string substring);
void request_prop_hand_release();
// The skater tick's per-frame contribution: the wrist world transform.
void set_prop_hand_target(const std::array<float, 3> &position, const std::array<float, 4> &rotation);
// Park tick, under native_mutex. A cheap no-op when nothing is requested.
void service_prop_hand();
// "" while idle, otherwise a one-line summary for the console status.
std::string prop_hand_status();
bool prop_hand_following();
} // namespace dingosdk::profile_runtime
