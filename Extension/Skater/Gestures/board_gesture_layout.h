#pragma once
#include <cstdint>
// Native ABI profile verified for Skate.exe SHA256
// fbce74d5e28ef525dbba2cb4adbebc13405bdbd88f31bc940bca45e4ae88b8f9
// Gesture identities and counts are discovered from loaded assets, not this profile.
namespace dingosdk::board_gesture::layout {
inline constexpr std::uintptr_t named_asset_owners=0x7726d10;
inline constexpr std::uintptr_t item_vtable=0x61047b0;
inline constexpr std::uintptr_t item_type=0x721d2d8;
inline constexpr std::uintptr_t cdb_asset_vtable=0x61a7708;
inline constexpr std::uintptr_t cdb_asset_type=0x729beb0;
inline constexpr std::uintptr_t cdb_vtable=0x61a7390;
inline constexpr std::uintptr_t cdb_type=0x729bb48;
inline constexpr std::uintptr_t sequence_vtable=0x61bab98;
inline constexpr std::uintptr_t sequence_type=0x72b8490;
inline constexpr std::uintptr_t actor_vtable=0x61ba838;
inline constexpr std::uintptr_t actor_type=0x72b87d8;
inline constexpr std::uintptr_t expression_vtable=0x61b1390;
inline constexpr std::uintptr_t expression_type=0x72b3738;
}
