// The upstream mesh suite runs session.cpp without a game process and already
// stubs native actors. Stub only its calls to the native towing driver; the
// actual hip provider and the separate towing suites remain real code.
//
// ReSkate 2.1.2 also made session.cpp reach the throwdown kit, the plugin
// runtime, the skater push and the local profile card. None of those exist
// without a game process either, so they are stubbed here in the same way.
#include "player_skitch.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "Extension/Plugins/plugin_runtime.h"
#include "Extension/Skater/skater_push.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Profile/local_profile_runtime.h"

namespace dingosdk::player_skitch {
void tick(std::uintptr_t,std::uintptr_t,const multiplayer::NativeFrame&,
    skateskitch::WorldKey,std::uint64_t,std::uint64_t,std::span<const skateskitch::TowCandidate>) {}
void suspend() noexcept {}
std::uint64_t press_serial() noexcept { return 0; }
}

namespace dingosdk::throwdowns::kit {
Match match() noexcept { return {}; }
bool hidden(std::uint64_t) noexcept { return false; }
}

namespace dingosdk::plugins {
void receive_message(std::uint64_t, const std::vector<std::uint8_t>&) noexcept {}
std::vector<std::vector<std::uint8_t>> take_outgoing() noexcept { return {}; }
}

namespace dingosdk::skater_push {
bool holding() noexcept { return false; }
}

namespace dingosdk {
bool prepare_on_board_teleport(std::uintptr_t, std::uintptr_t) noexcept { return false; }
PlayerCardModel local_profile_player_card() { return {}; }
}
