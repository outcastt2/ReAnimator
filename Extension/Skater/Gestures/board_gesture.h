#pragma once
#include <cstdint>
#include <string>
namespace dingosdk::board_gesture {
bool mounted(std::uintptr_t base,std::uintptr_t client) noexcept;
void tick(std::uintptr_t base,bool on_board=true) noexcept;
void set_enabled(bool value) noexcept;
bool enabled() noexcept;
std::string status();
} // namespace dingosdk::board_gesture
