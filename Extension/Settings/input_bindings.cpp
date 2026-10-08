#include "Extension/Profile/profile_internal.h"
#include <cmath>
#include <set>
#include <type_traits>

namespace dingosdk::profile {
using namespace detail;
std::uint32_t noclip_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("noclip");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Noclip binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= controller_button_mask && valid_controller_combo(static_cast<std::uint32_t>(mask)),
        "Unsupported controller buttons in Noclip binding");
    return static_cast<std::uint32_t>(mask);
}

std::uint32_t forward_velocity_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("forward_velocity");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Forward velocity binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= controller_button_mask && valid_controller_combo(static_cast<std::uint32_t>(mask)),
        "Unsupported controller buttons in Forward Boost binding");
    return static_cast<std::uint32_t>(mask);
}

std::uint32_t up_velocity_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 0;
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("up_velocity");
    if (value == bindings->end()) return 0;
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Up velocity binding must be a nonnegative integer");
    const auto mask = value->get<std::uint64_t>();
    require(mask <= controller_button_mask && valid_controller_combo(static_cast<std::uint32_t>(mask)),
        "Unsupported controller buttons in Up Boost binding");
    return static_cast<std::uint32_t>(mask);
}

std::uint32_t skitch_key_binding(const Snapshot& s) {
    const auto bindings = s.settings.find("bindings");
    if (bindings == s.settings.end()) return 'V';
    require(bindings->is_object(), "Bindings must be an object");
    const auto value = bindings->find("skitch_key");
    if (value == bindings->end()) return 'V';
    require(value->is_number_unsigned() || (value->is_number_integer() && value->get<std::int64_t>() >= 0),
        "Skitch key must be a nonnegative virtual-key code");
    const auto key = value->get<std::uint64_t>();
    require(key <= 255, "Unsupported Skitch virtual-key code");
    return static_cast<std::uint32_t>(key);
}

}
