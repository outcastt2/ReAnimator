#pragma once
#include <cstdint>
#include <string>
namespace dingosdk::board_gesture::routes {
void tick(std::uintptr_t base,bool requested,bool mounted=true) noexcept;
std::string status();
}
