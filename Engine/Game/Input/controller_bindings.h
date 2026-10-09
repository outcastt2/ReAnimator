#pragma once
#include <bit>
#include <cstdint>
#include <optional>
#include <string>

namespace dingosdk {
// XInput button bits, plus two synthetic trigger bits (above the dead zone).
inline constexpr std::uint32_t controller_button_mask = 0x3f3ff;
inline constexpr bool valid_controller_combo(std::uint32_t value) noexcept {
    return (value & ~controller_button_mask) == 0;
}
// Skitch's controller default is R1 (RB on Xbox pads): the value a profile
// that never saved a binding reports. An explicit 0 in the profile clears it.
inline constexpr std::uint32_t default_skitch_combo = 0x200;
// Button names shown to the player. Bindings are always stored as XInput bits.
enum class ControllerStyle : std::uint8_t { xbox, dualshock4, dualsense };
struct ControllerInput {
    bool available{};
    std::uint32_t buttons{};
    // Identifies the connected source set; zero means unavailable. Any change
    // (a pad connecting, disconnecting or switching source) resets latches.
    unsigned device{};
    ControllerStyle style{};
};
struct ControllerBindingsModel {
    bool available{};
    std::uint32_t noclip_combo{};
    std::uint32_t forward_velocity_combo{};
    std::uint32_t up_velocity_combo{};
    std::uint32_t skitch_combo{default_skitch_combo}; // held while towing; zero disables controller Skitch
    std::uint32_t skitch_key{'V'}; // virtual-key code; zero disables keyboard Skitch
    std::string status;
};
inline std::string controller_combo_label(std::uint32_t value, ControllerStyle style = ControllerStyle::xbox) {
    if (!value) return "Not bound";
    const bool playstation = style != ControllerStyle::xbox;
    struct Button { std::uint32_t bit; const char* xbox; const char* playstation; };
    constexpr Button buttons[]{
        {0x100,"LB","L1"}, {0x200,"RB","R1"}, {0x10000,"LT","L2"}, {0x20000,"RT","R2"},
        {0x40,"L3","L3"}, {0x80,"R3","R3"},
        {0x1000,"A","Cross"}, {0x2000,"B","Circle"}, {0x4000,"X","Square"}, {0x8000,"Y","Triangle"},
        {1,"D-pad Up","D-pad Up"}, {2,"D-pad Down","D-pad Down"}, {4,"D-pad Left","D-pad Left"}, {8,"D-pad Right","D-pad Right"},
        {0x10,"Start","Options"}, {0x20,"Back",nullptr}};
    std::string label;
    for (const auto& button : buttons) if (value & button.bit) {
        if (!label.empty()) label += " + ";
        label += !playstation ? button.xbox : button.playstation ? button.playstation :
            style == ControllerStyle::dualsense ? "Create" : "Share";
    }
    return label;
}
// A held combo never repeats. Returning from menus, reconnecting, changing a
// binding, and changing level all require releasing every bound button first.
struct ControllerComboLatch {
    std::uint32_t combo{};
    unsigned device{};
    bool armed{};
    bool update(std::uint32_t binding, ControllerInput input, bool inhibited) noexcept {
        if (binding != combo || input.device != device) {
            combo = binding; device = input.device; armed = false;
        }
        if (inhibited || !input.available || !combo || !valid_controller_combo(combo)) {
            armed = false; return false;
        }
        const auto held = input.buttons & combo;
        if (!held) armed = true;
        if (!armed || held != combo) return false;
        armed = false;
        return true;
    }
};
// Record the largest simultaneous chord, not a union of sequential presses.
// Begin neutral so the input that opened Record cannot become the binding.
struct ControllerComboCapture {
    std::uint32_t chord{};
    unsigned device{};
    bool ready{};
    std::optional<std::uint32_t> update(ControllerInput input) noexcept {
        if (!input.available || input.device != device) {
            chord = 0; ready = false; device = input.device;
        }
        if (!input.available) return {};
        const auto buttons = input.buttons & controller_button_mask;
        if (!ready) { ready = buttons == 0; return {}; }
        if (std::popcount(buttons) > std::popcount(chord)) chord = buttons;
        if (!buttons && chord) {
            const auto result = chord; chord = 0; ready = false;
            return result;
        }
        return {};
    }
};
}
