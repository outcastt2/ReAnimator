#pragma once
#include "developer_board_material.h"

namespace dingosdk {
// Colours the board a listed player rides: `styles` is how they have each marked cosmetic, the
// board's deck, grip tape, trucks and wheels after the skater's.
void update_developer_board(std::uintptr_t base, std::uintptr_t board, std::uint64_t steam_id,
                            std::uint64_t generation, DeveloperBoardState &state,
                            const multiplayer::MarkStyles &styles) noexcept;
void tick_local_developer_board(std::uintptr_t base, std::uintptr_t client, bool ready) noexcept;
} // namespace dingosdk
