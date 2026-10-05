// The upstream mesh suite runs session.cpp without a game process and already
// stubs native actors. Stub only its calls to the native towing driver; the
// actual hip provider and the separate towing suites remain real code.
#include "player_skitch.h"
namespace dingosdk::player_skitch {
void tick(std::uintptr_t,std::uintptr_t,const multiplayer::NativeFrame&,
    skateskitch::WorldKey,std::uint64_t,std::uint64_t,std::span<const skateskitch::TowCandidate>) {}
void suspend() noexcept {}
}
