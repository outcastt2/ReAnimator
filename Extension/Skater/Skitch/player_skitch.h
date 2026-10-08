#pragma once
#include "tow_controller.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <string>
namespace dingosdk::player_skitch {
struct Request {
    std::uintptr_t base{}, client{}, entity{}, core{}, component{};
    std::uint64_t expires{};
    skateskitch::TowPlan plan;
};
void tick(std::uintptr_t base, std::uintptr_t client, const multiplayer::NativeFrame&,
    skateskitch::WorldKey, std::uint64_t local_id, std::uint64_t now,
    std::span<const skateskitch::TowCandidate>);
void suspend() noexcept;
// Reason must be a static string literal; stored for the next game-thread tick.
void fault(const char* reason="Skitch physics changed; release grab and try again") noexcept;
void note_physics_step() noexcept;
void note_turn_step() noexcept;
// Passive probe of the wipeout motion state (core+0x3b0 and its +0x48 active
// substate): dumps both on the first ragdoll frame and logs the first change of
// every watched word. Read-only; use it to find the field that moves a ragdoll.
void request_probe(unsigned seconds) noexcept;
std::optional<Request> request() noexcept;
void animation_evaluated(std::uintptr_t component) noexcept;
void render_pose(std::uintptr_t animation_interface) noexcept;
// Grip-holds-the-bail option. While on and the local skater has an active grip
// (a plan is being published), wipeout requests are suppressed: the rider
// stays on the board and the physical tow keeps working, instead of entering
// the animation-driven ragdoll. Off by default; `ragdoll nobail on|off`.
void set_grip_no_bail(bool on) noexcept;
bool grip_no_bail() noexcept;
bool grip_holding() noexcept;
void set_enabled(bool) noexcept;
bool enabled() noexcept;
std::string status();
} // namespace dingosdk::player_skitch
