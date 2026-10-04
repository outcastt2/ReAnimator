#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Attach a native EffectBlueprint to a joint of the local skater, for testing
// whether a runtime-spawned effect is possible (see SkateAnimationStudio/README.md
// section 10.7). The blueprint must already be loaded: the engine's asset lookup
// (find_asset) only finds loaded assets, so wear a costume that uses the effect
// first (Burt Blaze loads eb_chr_fire_head, the Grim Reaper loads ebp_chr_gr_*).
namespace dingosdk::skater {

// Queue an attach. `blueprint` is the full asset path (case-insensitive).
// `offset` is a world-space vertical offset above the joint, in metres.
void request_effect_attach(std::string blueprint, float offset);
// Queue a detach and destruction of the spawned entity.
void request_effect_attach_off();
// Human-readable state for the console command and the log.
std::string effect_attach_status();
// Report whether a named asset is loaded, without spawning anything.
std::string asset_loaded_report(std::string_view name);
// Per-frame work, called on the client thread from the runtime tick.
void tick_effect_attach(std::uintptr_t base, std::uintptr_t client) noexcept;

} // namespace dingosdk::skater
