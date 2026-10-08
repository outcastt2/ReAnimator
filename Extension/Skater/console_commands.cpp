#include "Gestures/board_gesture.h"
#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "ai_skaters.h"
#include "Skitch/player_skitch.h"
#include "Skitch/drag_state.h"
#include "effect_attach.h"
#include "custom_animation.h"
#include "prop_attach.h"
#include <format>
#include <string>
namespace dingosdk::console {
void register_movement_commands(Commands &registry) {
    auto skitch = variable("skitch", "Player hip towing: hold V or LB+RB nearby", Group::movement, argument("0|1", Type::boolean));
    skitch.inspect = [](const Model&) { return boolean_state(true,player_skitch::enabled(),{},player_skitch::status()); };
    skitch.run = [](const Model&,const Values& args,const Output&) { player_skitch::set_enabled(std::get<bool>(args[0])); };
    skitch.reset = [](const Model&,const Output&) { player_skitch::set_enabled(false); };
    auto skitchprobe = action("skitchprobe",
        "Player skitch: probe the wipeout motion state during a bail (read-only, logs to the log)",
        Group::movement, {argument("seconds", Type::number, true)});
    skitchprobe.execution = Execution::local;
    skitchprobe.inspect = [](const Model&) {
        return State{true, {}, {}, "Probe the wipeout motion object; bail while holding the grab.", false};
    };
    skitchprobe.run = [](const Model&, const Values& args, const Output& out) {
        unsigned seconds = 60;
        if (!args.empty()) {
            try {
                if (std::holds_alternative<double>(args[0]))
                    seconds = static_cast<unsigned>(std::get<double>(args[0]));
                else if (std::holds_alternative<std::int64_t>(args[0]))
                    seconds = static_cast<unsigned>(std::get<std::int64_t>(args[0]));
                else if (std::holds_alternative<std::uint64_t>(args[0]))
                    seconds = static_cast<unsigned>(std::get<std::uint64_t>(args[0]));
            } catch (...) {
            }
        }
        player_skitch::request_probe(seconds);
        out("Player skitch: probing the wipeout motion state for " + std::to_string(seconds) +
            "s (read-only). Bail while holding the grab, then check the log.");
    };
    registry.add(std::move(skitchprobe));
    registry.add(std::move(skitch));
    auto gesture = action("boardgesture", "Show automatic board gesture status; 0 disables, 1 enables", Group::movement, {argument("0|1", Type::boolean,true)});
    gesture.inspect = [](const Model&) { return boolean_state(true,board_gesture::enabled(),{},board_gesture::status()); };
    gesture.run = [](const Model&,const Values& args,const Output& out) {
        if(!args.empty()) board_gesture::set_enabled(std::get<bool>(args[0]));
        out(std::string("Board gesture ")+(board_gesture::enabled() ? "enabled: " : "disabled: ")+board_gesture::status());
    };
    gesture.reset = [](const Model&,const Output&) { board_gesture::set_enabled(false); };
    registry.add(std::move(gesture));
    using Debug = overlay::DebugAction;
    struct Setting {
        const char *name;
        const char *alias;
        const char *description;
        Debug action;
        bool overlay::DebugModel::*value;
        bool overlay::DebugModel::*available;
    };
    const Setting settings[]{
        {"noclip", "debug.noclip", "Fly the local skater through the world", Debug::set_noclip,
         &overlay::DebugModel::noclip, &overlay::DebugModel::noclip_available},
        {"nobail", "debug.no_bail", "Prevent wipeouts; also activated automatically during noclip", Debug::set_no_bail,
         &overlay::DebugModel::no_bail, &overlay::DebugModel::no_bail_available},
        {"freecam", "debug.freecam", "Move the camera independently of the skater", Debug::set_free_camera,
         &overlay::DebugModel::free_camera, &overlay::DebugModel::camera_available},
        {"hideui", "debug.game_ui_hidden", "Hide the game's interface", Debug::set_game_ui_hidden,
         &overlay::DebugModel::game_ui_hidden, &overlay::DebugModel::ui_available}};
    for (const auto &setting : settings) {
        auto entry = variable(setting.name, setting.description, Group::movement, argument("0|1", Type::boolean));
        entry.aliases = {setting.alias};
        entry.inspect = [setting](const Model &m) {
            auto result =
                boolean_state(m.debug.*setting.available, m.debug.*setting.value, "Waiting for local player controls.");
            if (setting.action == Debug::set_noclip && !result.available)
                result.reason = m.debug.noclip_unavailable;
            if (setting.action == Debug::set_free_camera && !result.available)
                result.reason = m.debug.camera_unavailable;
            if (setting.action == Debug::set_no_bail) {
                result.value = m.debug.no_bail ? "1" : "0";
                result.detail = m.debug.no_bail_active ? "Protection active" : "Protection inactive";
                if (m.debug.noclip && m.debug.no_bail_active)
                    result.detail += "; automatic during noclip";
            }
            return result;
        };
        entry.run = [setting](const Model &, const Values &args, const Output &) {
            request_debug(setting.action, std::get<bool>(args[0]));
        };
        entry.reset = [setting](const Model &, const Output &out) {
            if (setting.action == Debug::set_no_bail || setting.action == Debug::set_noclip)
                request_debug(setting.action, false);
            else {
                out("Restoring the shared movement overrides.");
                request_debug(Debug::restore_debug);
            }
        };
        registry.add(std::move(entry));
    }
    auto speed = argument("speed", Type::number);
    speed.choices = {"0.6", "3", "5", "15", "60", "300", "1500"};
    auto flight = variable("flyspeed", "Free-flight speed in world units per second", Group::movement, speed);
    flight.aliases = {"debug.camera_speed"};
    flight.inspect = [](const Model &m) {
        return State{m.debug.available,
                     m.debug.available
                         ? std::optional<std::string>(value_text(static_cast<double>(m.debug.camera_speed)))
                         : std::nullopt,
                     "Waiting for local player controls.",
                     {},
                     false};
    };
    flight.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_camera_speed, false, static_cast<float>(std::get<double>(args[0])));
    };
    flight.reset = [](const Model &, const Output &) { request_debug(Debug::set_camera_speed, false, 15.0f); };
    registry.add(std::move(flight));
    auto fov_argument = argument("degrees", Type::number);
    fov_argument.minimum = 0;
    fov_argument.maximum = 120;
    auto free_fov = variable("freecamfov", "Freecam field of view in degrees (40-120; 0 = the game's own)",
        Group::movement, fov_argument);
    free_fov.inspect = [](const Model &m) {
        return State{m.debug.available, value_text(static_cast<double>(m.debug.free_camera_fov)),
                     "Waiting for local player controls.", {}, false};
    };
    free_fov.run = [](const Model &, const Values &args, const Output &out) {
        const auto degrees = std::get<double>(args[0]);
        if (degrees != 0 && degrees < 40) {
            out("error: Use 0 or 40-120.");
            return;
        }
        request_debug(Debug::set_free_camera_fov, false, static_cast<float>(degrees));
    };
    free_fov.reset = [](const Model &, const Output &) { request_debug(Debug::set_free_camera_fov, false, 0.0f); };
    registry.add(std::move(free_fov));
    auto boost_speed_argument = argument("speed", Type::number);
    boost_speed_argument.minimum = 1;
    boost_speed_argument.maximum = 300;
    auto boost_speed = variable("forwardvelocity", "Velocity added along the skater's forward direction", Group::movement, boost_speed_argument);
    boost_speed.aliases = {"debug.forward_velocity_speed"};
    boost_speed.inspect = [](const Model &m) {
        return State{m.debug.available,
                     m.debug.available
                         ? std::optional<std::string>(value_text(static_cast<double>(m.debug.forward_velocity_speed)))
                         : std::nullopt,
                     "Waiting for local player controls.", {}, false};
    };
    boost_speed.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_forward_velocity_speed, false, static_cast<float>(std::get<double>(args[0])));
    };
    boost_speed.reset = [](const Model &, const Output &) {
        request_debug(Debug::set_forward_velocity_speed, false, 20.0f);
    };
    registry.add(std::move(boost_speed));
    auto up_speed_argument = boost_speed_argument;
    up_speed_argument.maximum = 25;
    auto up_speed = variable("upvelocity", "Velocity added along world up", Group::movement, up_speed_argument);
    up_speed.aliases = {"debug.up_velocity_speed"};
    up_speed.inspect = [](const Model &m) {
        return State{m.debug.available,
                     m.debug.available
                         ? std::optional<std::string>(value_text(static_cast<double>(m.debug.up_velocity_speed)))
                         : std::nullopt,
                     "Waiting for local player controls.", {}, false};
    };
    up_speed.run = [](const Model &, const Values &args, const Output &) {
        request_debug(Debug::set_up_velocity_speed, false, static_cast<float>(std::get<double>(args[0])));
    };
    up_speed.reset = [](const Model &, const Output &) {
        request_debug(Debug::set_up_velocity_speed, false, 20.0f);
    };
    registry.add(std::move(up_speed));
    auto boost = action("forwardboost", "Add the configured velocity along the skater's forward direction", Group::movement);
    boost.inspect = [](const Model &m) {
        return State{m.debug.forward_velocity_available, {}, m.debug.forward_velocity_unavailable, {}, false};
    };
    boost.run = [](const Model &, const Values &, const Output &) {
        request_debug(Debug::add_forward_velocity);
    };
    registry.add(std::move(boost));
    auto mask = argument("controller_mask", Type::unsigned_integer);
    mask.minimum = 0;
    mask.maximum = UINT32_MAX;
    auto bind =
        action("bind noclip", "Bind controller buttons to noclip; 0 clears the binding", Group::movement, {mask});
    bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_noclip_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(bind));
    auto boost_bind =
        action("bind forwardvelocity", "Bind controller buttons to Forward Boost; 0 clears the binding", Group::movement, {mask});
    boost_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    boost_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_forward_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(boost_bind));
    auto up_bind =
        action("bind upvelocity", "Bind controller buttons to Up Boost; 0 clears the binding", Group::movement, {mask});
    up_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    up_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_up_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(up_bind));
    auto skitch_key_argument = argument("virtual_key", Type::unsigned_integer);
    skitch_key_argument.minimum = 0;
    skitch_key_argument.maximum = 255;
    auto skitch_key_bind = action("bind skitchkey",
        "Bind the keyboard key for player skitching; 0 disables the keyboard key (controller LB+RB remains)",
        Group::movement, {skitch_key_argument});
    skitch_key_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    skitch_key_bind.run = [](const Model &, const Values &args, const Output &out) {
        const auto key = static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0]));
        const bool saved = set_local_skitch_key_binding(key);
        out(saved ? local_profile_controller_bindings().status : "error: Invalid key or save failed.");
    };
    registry.add(std::move(skitch_key_bind));

    // Prototype: spawn a native effect blueprint and keep it on the skater's head.
    // The blueprint must already be loaded (wear a costume that uses it).
    auto headfx = action("headfx",
        "Attach a loaded native effect blueprint to the skater's head; 'headfx off' detaches",
        Group::movement, {argument("blueprint", Type::text, true), argument("offset", Type::number, true)});
    headfx.execution = Execution::local;
    headfx.inspect = [](const Model &) {
        return State{true, {}, {}, skater::effect_attach_status(), false};
    };
    headfx.run = [](const Model &, const Values &args, const Output &out) {
        if (args.empty()) { out(skater::effect_attach_status()); return; }
        const auto name = std::get<std::string>(args[0]);
        if (lower(name) == "off") {
            skater::request_effect_attach_off();
            out("Detaching effect...");
            return;
        }
        const float offset = args.size() > 1 ? static_cast<float>(std::get<double>(args[1])) : 0.0f;
        skater::request_effect_attach(name, offset);
        out("Attaching " + name + "... (the result appears in the log)");
    };
    registry.add(std::move(headfx));

    auto findasset = action("findasset", "Report whether a named asset is loaded (find_asset is find-only)",
        Group::movement, {argument("name")});
    findasset.execution = Execution::local;
    findasset.run = [](const Model &, const Values &args, const Output &out) {
        out(skater::asset_loaded_report(std::get<std::string>(args[0])));
    };
    registry.add(std::move(findasset));

    // Hand props: the game's own gesture props follow a custom animation, so the
    // trigger is what has to be found before anything is written.
    auto prop = action("prop",
        "Hand props: prop | prop list [name] | prop watch [name] [seconds] | prop trace [seconds] - gesture props and their creation",
        Group::movement, {argument("mode", Type::text, true), argument("name", Type::text, true),
                          argument("seconds|side", Type::text, true)});
    prop.execution = Execution::local;
    prop.inspect = [](const Model &) { return State{true, {}, {}, skater::prop_status(), false}; };
    prop.run = [](const Model &, const Values &args, const Output &out) {
        // Arguments arrive as a variant: "prop weight 45" puts the number in the
        // text slot, so read both rather than assuming a lane.
        const auto text_at = [&](std::size_t index) -> std::string {
            if (index >= args.size() || !std::holds_alternative<std::string>(args[index])) return {};
            return lower(std::get<std::string>(args[index]));
        };
        const auto number_at = [&](std::size_t index, unsigned fallback) -> unsigned {
            if (index >= args.size()) return fallback;
            try {
                if (std::holds_alternative<double>(args[index])) return static_cast<unsigned>(std::get<double>(args[index]));
                if (std::holds_alternative<std::int64_t>(args[index])) return static_cast<unsigned>(std::get<std::int64_t>(args[index]));
                if (std::holds_alternative<std::uint64_t>(args[index])) return static_cast<unsigned>(std::get<std::uint64_t>(args[index]));
                if (const auto text = text_at(index); !text.empty()) return static_cast<unsigned>(std::stoul(text));
            } catch (...) {
            }
            return fallback;
        };
        const auto mode = text_at(0);
        const auto name = text_at(1);
        if (mode.empty() || mode == "list") {
            skater::request_prop_report(name);
            out("Hand props: listing the loaded gesture items in the log...");
            return;
        }
        if (mode == "watch") {
            const auto seconds = number_at(2, 60u);
            skater::request_prop_watch(name, seconds);
            out("Hand props: watching for " + std::to_string(seconds) +
                "s. Idle for ~10s, then press the gesture three times; the phase flips by itself.");
            return;
        }
        if (mode == "mark") {
            skater::request_prop_mark();
            out("Hand props: phase marked; presses from now on are tagged with it.");
            return;
        }
        if (mode == "weight") {
            const auto seconds = number_at(1, 45u);
            skater::request_prop_weight(seconds);
            out("Hand props: watching the layer area for " + std::to_string(seconds) +
                "s; press the gesture a few times, letting each finish.");
            return;
        }
        if (mode == "pose") {
            const auto seconds = number_at(1, 45u);
            skater::request_prop_pose(seconds);
            out("Hand props: watching every joint for " + std::to_string(seconds) +
                "s; idle ~10s, then hold the phone out (pause menu) until it ends.");
            return;
        }
        if (mode == "assets") {
            const auto seconds = number_at(2, 45u);
            skater::request_prop_assets(name, seconds);
            out("Hand props: resolving the phone's graph assets by name, then watching where the animation instance "
                "references them for " + std::to_string(seconds) +
                "s; idle ~10s, then do the selfie (or hold the menu phone).");
            return;
        }
        if (mode == "poke") {
            if (text_at(1) == "off") {
                skater::request_prop_poke_stop();
                out("Hand props: releasing the poke and restoring the original weights.");
                return;
            }
            const auto seconds = number_at(1, 12u);
            skater::request_prop_poke(seconds);
            out("Hand props: writing 1.0 into the three layer weights for " + std::to_string(seconds) +
                "s, then restoring. Watch your skater; 'prop poke off' restores now.");
            return;
        }
        if (mode == "trace") {
            if (text_at(1) == "off") {
                skater::request_prop_trace_off();
                out("Hand props: disarming the creation trace; captured records still drain to the log.");
                return;
            }
            const auto seconds = number_at(1, 90u);
            skater::request_prop_trace(seconds);
            out("Hand props: arming the creation trace for " + std::to_string(seconds) +
                "s. Idle a few seconds, then play the selfie (and the menu phone); every entity creation lands in "
                "the log with its caller stack.");
            return;
        }
        if (mode == "derive") {
            const auto seconds = number_at(1, 90u);
            skater::request_prop_derive(seconds);
            out("Hand props: scanning for the phone blueprint every second for " + std::to_string(seconds) +
                "s. Close the console and bring the phone out; the derive check is read-only.");
            return;
        }
        if (mode == "hand") {
            if (text_at(1) == "off") {
                skater::request_prop_hand_detach();
                out("Hand props: releasing the held prop; it goes back to where it was placed.");
                return;
            }
            const auto item = text_at(1);
            const auto side = text_at(2);
            skater::request_prop_hand_attach(item, side == "left");
            out("Hand props: taking the last placed object" +
                (item.empty() ? std::string{} : " matching \"" + item + "\"") +
                (side == "left" ? " into the left hand." : " into the right hand.") +
                " It follows the wrist until 'prop hand off'.");
            return;
        }
        out("usage: prop | prop list [name] | prop watch [name] [seconds] | prop mark | prop weight [seconds] | "
            "prop pose [seconds] | prop assets [asset] [seconds] | prop poke [seconds|off] | prop trace [seconds|off] | "
            "prop derive [seconds] | prop hand [name] [left|off]");
    };
    registry.add(std::move(prop));

    // Character morphs: the CAS sliders clamp to the data mapping's authored
    // Min/Max (0..1 for every body region). This rewrites the live range.
    auto morph = action("morph",
        "Character morphs: morph find | morph unclamp [max] | morph clamp",
        Group::gameplay, {argument("mode", Type::text, true), argument("max", Type::text, true)});
    morph.execution = Execution::local;
    morph.inspect = [](const Model &) { return State{true, {}, {}, skater::prop_status(), false}; };
    morph.run = [](const Model &, const Values &args, const Output &out) {
        const auto text = [&](std::size_t index) -> std::string {
            if (index >= args.size() || !std::holds_alternative<std::string>(args[index])) return {};
            return lower(std::get<std::string>(args[index]));
        };
        const auto mode = text(0);
        if (mode.empty() || mode == "find") {
            skater::request_morph_find();
            out("Morph: reading the data mapping (see the log)...");
            return;
        }
        if (mode == "unclamp") {
            float max_value = 4.0f;
            try {
                const auto spec = text(1);
                if (!spec.empty()) max_value = std::stof(spec);
            } catch (...) {
            }
            skater::request_morph_unclamp(max_value);
            out(std::format("Morph: raising the body inputs' maximum to {:.2f}; 'morph clamp' restores it.",
                            max_value));
            return;
        }
        if (mode == "clamp") {
            skater::request_morph_clamp();
            out("Morph: restoring the authored clamp values...");
            return;
        }
        out("usage: morph find | morph unclamp [max] | morph clamp");
    };
    registry.add(std::move(morph));

    // The ragdoll mode switchboard: the SkaterCorePhysicsRagdollConfigAsset
    // references the bool graph parameters that pick how a wipeout is driven.
    // `config` dumps the whole chain, `watch` samples the candidate value bytes
    // through a bail, and `set` holds one byte at 0/1 to find the live one.
    auto ragdoll = action("ragdoll",
        "Ragdoll modes: ragdoll | ragdoll config | ragdoll bools | ragdoll watch [seconds] | "
        "ragdoll set <field|index> <0|1> [seconds] | ragdoll setdata <field|index> <0|1> <offset> | "
        "ragdoll nobail on|off | ragdoll off",
        Group::gameplay, {argument("mode", Type::text, true), argument("field", Type::text, true),
                          argument("value", Type::text, true), argument("extra", Type::text, true)});
    ragdoll.execution = Execution::local;
    ragdoll.inspect = [](const Model &) { return State{true, {}, {}, skater::prop_status(), false}; };
    ragdoll.run = [](const Model &, const Values &args, const Output &out) {
        const auto text = [&](std::size_t index) -> std::string {
            if (index >= args.size() || !std::holds_alternative<std::string>(args[index])) return {};
            return lower(std::get<std::string>(args[index]));
        };
        const auto number = [&](std::size_t index, unsigned fallback) -> unsigned {
            if (index >= args.size()) return fallback;
            try {
                if (std::holds_alternative<double>(args[index])) return static_cast<unsigned>(std::get<double>(args[index]));
                if (std::holds_alternative<std::int64_t>(args[index])) return static_cast<unsigned>(std::get<std::int64_t>(args[index]));
                if (std::holds_alternative<std::uint64_t>(args[index])) return static_cast<unsigned>(std::get<std::uint64_t>(args[index]));
                if (const auto value = text(index); !value.empty()) return static_cast<unsigned>(std::stoul(value, nullptr, 0));
            } catch (...) {
            }
            return fallback;
        };
        const auto mode = text(0);
        if (mode == "config") {
            skater::request_ragdoll_config();
            out("Ragdoll: resolving skatercorephysicsragdollconfig and its bool references (see the log)...");
            return;
        }
        if (mode == "bools") {
            skater::request_ragdoll_bools();
            out("Ragdoll: resolving the named physics bool assets (see the log)...");
            return;
        }
        if (mode == "refs") {
            skater::request_ragdoll_refs();
            out("Ragdoll: scanning the animation instance for references to the ragdoll bool assets (see the log).");
            return;
        }
        if (mode == "dump") {
            skater::request_ragdoll_data();
            out("Ragdoll: trying every plausible base for the +0x28 data offsets (see the log).");
            return;
        }
        if (mode == "watch") {
            const auto seconds = number(1, 30u);
            skater::request_ragdoll_watch(seconds);
            out("Ragdoll: watching every switch byte for " + std::to_string(seconds) +
                "s. Skitch a player, hold the grab, wipe out: changed bytes land in the log with labels.");
            return;
        }
        if (mode == "set" || mode == "setdata") {
            const auto field = text(1);
            const auto value = number(2, 0u);
            if (mode == "set") {
                const auto seconds = number(3, 45u);
                skater::request_ragdoll_set(field, value, seconds);
                out("Ragdoll: holding " + field + " = " + std::to_string(value ? 1u : 0u) +
                    " (asset Default) for " + std::to_string(seconds) + "s; 'ragdoll off' restores.");
                return;
            }
            const auto offset = number(3, 0u);
            skater::request_ragdoll_set(field, value, 45u, true, offset);
            out("Ragdoll: holding " + field + " = " + std::to_string(value ? 1u : 0u) + " at instance+" +
                std::to_string(offset) + " for 45s; 'ragdoll off' restores.");
            return;
        }
        if (mode == "off") {
            skater::request_ragdoll_stop();
            out("Ragdoll: restoring every held byte.");
            return;
        }
        if (mode == "nobail") {
            const auto on = text(1) == "on" || number(1, 0u) != 0;
            dingosdk::player_skitch::set_grip_no_bail(on);
            out(on ? "Ragdoll: while a grip is held, wipeouts are now suppressed (stay on the board, physical tow)."
                   : "Ragdoll: grip no longer suppresses wipeouts (a bail becomes a ragdoll again).");
            return;
        }
        out("usage: ragdoll | ragdoll config | ragdoll bools | ragdoll refs | ragdoll dump | ragdoll watch [seconds] | "
            "ragdoll set <field|index> <0|1> [seconds] | ragdoll setdata <field|index> <0|1> <offset> | "
            "ragdoll nobail on|off | ragdoll off");
    };
    registry.add(std::move(ragdoll));

    // Native ragdoll drag: the bail motion states (Falling, FollowRagdoll,
    // FollowAnimatedRagdoll) are animation-driven and recompute their position
    // from the context every update, so the render-pose drag could only move
    // what is drawn. This hooks the three state updates and, while armed,
    // replaces the motion with the tether follow slot through the engine's own
    // placement channel -- the core, the render placement and the camera follow.
    auto dragstate = action("dragstate",
        "Native ragdoll drag: dragstate on | dragstate off | dragstate status | dragstate probe [seconds]",
        Group::gameplay, {argument("mode", Type::text, true), argument("seconds", Type::text, true)});
    dragstate.execution = Execution::local;
    dragstate.inspect = [](const Model &) {
        return State{true, {}, {}, dingosdk::player_skitch::drag_state_status(), false};
    };
    dragstate.run = [](const Model &, const Values &args, const Output &out) {
        const auto text = [&](std::size_t index) -> std::string {
            if (index >= args.size() || !std::holds_alternative<std::string>(args[index])) return {};
            return lower(std::get<std::string>(args[index]));
        };
        const auto mode = text(0);
        if (mode == "on" || mode == "off") {
            const bool on = mode == "on";
            dingosdk::player_skitch::set_drag_state(on);
            out(on ? "Drag state: armed. Skitch a player, hold the grab, bail: the tether motion now drives the "
                     "ragdoll through the engine's own placement channel (camera follows)."
                   : "Drag state: off. Bail motion returns to the native ragdoll states.");
            return;
        }
        if (mode == "probe") {
            unsigned seconds = 20;
            try {
                if (const auto value = text(1); !value.empty()) seconds = static_cast<unsigned>(std::stoul(value));
            } catch (...) {
            }
            dingosdk::player_skitch::drag_state_probe(seconds);
            out("Drag state: logging the driven position against the goal for " + std::to_string(seconds) + "s.");
            return;
        }
        out(dingosdk::player_skitch::drag_state_status());
    };
    registry.add(std::move(dragstate));

    // Route B: overwrite the local skater's pose with a baked custom animation.
    auto poseanim = action("poseanim",
        "Custom animation: poseanim test | record | off | play | save <path> | mask auto|full|legs | trace 0|1 | <file>.rska",
        Group::movement, {argument("clip", Type::text, true), argument("path", Type::text, true)});
    poseanim.execution = Execution::local;
    poseanim.inspect = [](const Model &) {
        return State{true, {}, {},
                     skater::pose_playback_status() + "  [mask: " + skater::pose_mask_name() + "]", false};
    };
    poseanim.run = [](const Model &, const Values &args, const Output &out) {
        if (args.empty()) {
            out(skater::pose_playback_status());
            out("Masking: " + skater::pose_mask_name() + ".");
            out(skater::pose_playback_cost());
            out(std::string("Layer trace: ") + (skater::pose_trace() ? "on" : "off") + " (poseanim trace 0|1).");
            return;
        }
        const auto clip = std::get<std::string>(args[0]);
        if (lower(clip) == "off") { skater::request_pose_playback_stop(); out("Stopping custom animation..."); return; }
        if (lower(clip) == "trace") {
            const auto mode = args.size() > 1 ? std::get<std::string>(args[1]) : std::string{};
            const bool enabled = mode == "1" || lower(mode) == "on";
            skater::set_pose_trace(enabled);
            out(enabled ? "Layer trace on: one geometry line every two seconds."
                        : "Layer trace off.");
            return;
        }
        if (lower(clip) == "mask") {
            // Which joints the clip is allowed to drive. "auto" hands the legs
            // and pelvis to the game while riding or moving, so a clip layers on
            // top of the board stance and the walk cycle.
            const auto mode = args.size() > 1 ? lower(std::get<std::string>(args[1])) : std::string{};
            if (mode != "auto" && mode != "full" && mode != "legs") {
                out("usage: poseanim mask auto|full|legs  (currently " + skater::pose_mask_name() + ")");
                return;
            }
            skater::set_pose_mask(mode == "auto" ? skater::PoseMask::automatic
                                 : mode == "full" ? skater::PoseMask::full
                                                  : skater::PoseMask::legs);
            out(mode == "auto"
                    ? "Custom animation masking: auto (the game keeps the legs while riding or moving)."
                    : "Custom animation masking: " + mode + ".");
            return;
        }
        if (lower(clip) == "play") {
            // "play" alone replays the in-memory recording; "play <name>" loads
            // the file, which is what a bare name does too.
            if (args.size() > 1) {
                const auto name = std::get<std::string>(args[1]);
                skater::request_pose_playback(name);
                out("Playing " + name + "...");
            } else {
                skater::request_pose_record_playback();
                out("Playing the recorded pose...");
            }
            return;
        }
        if (lower(clip) == "save") {
            const auto path = args.size() > 1 ? std::get<std::string>(args[1]) : std::string{};
            if (path.empty()) { out("usage: poseanim save <path>"); return; }
            const auto error = skater::save_recorded_clip(path);
            out(error.empty() ? "Saved: " + path : "error: " + error);
            return;
        }
        skater::request_pose_playback(clip);
        out("Starting custom animation " + clip + "...");
    };
    registry.add(std::move(poseanim));

    auto dumpskeleton = action("dumpskeleton", "Write the live skater skeleton resource beside the log",
        Group::movement);
    dumpskeleton.execution = Execution::local;
    dumpskeleton.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    dumpskeleton.run = [](const Model &, const Values &, const Output &out) {
        skater::request_skeleton_dump();
        out("Dumping the skeleton...");
    };
    registry.add(std::move(dumpskeleton));
}
void register_ai_commands(Commands &registry) {
    const auto ready = [](const Model &m) {
        return State{m.debug.available, {}, "Waiting for local player controls.", {}, false};
    };
    auto count = argument("count", Type::unsigned_integer, true);
    count.minimum = 1;
    count.maximum = 8;
    auto physics = argument("physics", Type::boolean, true);
    auto brain = argument("brain", Type::boolean, true);
    auto spawn = action("ai spawn", "Experimental: spawn native AI skaters beside your skater; results are logged",
                        Group::gameplay, {count, physics, brain});
    spawn.aliases = {"ai.spawn"};
    spawn.inspect = ready;
    spawn.run = [](const Model &, const Values &args, const Output &out) {
        const auto n = args.empty() ? 1u : static_cast<unsigned>(std::get<std::uint64_t>(args[0]));
        const bool enabled = args.size() < 2 || std::get<bool>(args[1]);
        const bool thinking = args.size() < 3 || std::get<bool>(args[2]);
        ai_skaters::request_spawn(n, enabled, thinking);
        out(std::format("Spawning {} AI skater(s) with physics {} and the brain {}.", n,
                        enabled ? "enabled" : "disabled", thinking ? "running" : "paused"));
    };
    registry.add(std::move(spawn));
    auto toggle = action("ai brain", "Start (1) or pause (0) the brain tree of every AI skater", Group::gameplay,
                         {argument("0|1", Type::boolean)});
    toggle.aliases = {"ai.brain"};
    toggle.inspect = ready;
    toggle.run = [](const Model &, const Values &args, const Output &out) {
        const bool enabled = std::get<bool>(args[0]);
        ai_skaters::request_brain(enabled);
        out(enabled ? "Starting AI skater brains." : "Pausing AI skater brains.");
    };
    registry.add(std::move(toggle));
    auto clear = action("ai clear", "Remove every spawned AI skater", Group::gameplay);
    clear.aliases = {"ai.clear"};
    clear.inspect = ready;
    clear.run = [](const Model &, const Values &, const Output &out) {
        ai_skaters::request_clear();
        out("Removing AI skaters.");
    };
    registry.add(std::move(clear));
    auto status = action("ai status", "Log skater sources, the AI blueprint and each AI skater's state",
                         Group::gameplay);
    status.aliases = {"ai.status"};
    status.inspect = ready;
    status.run = [](const Model &, const Values &, const Output &out) {
        ai_skaters::request_report();
        out(std::format("{} AI skater(s) active; details follow in the log.", ai_skaters::active_count()));
    };
    registry.add(std::move(status));
}
} // namespace dingosdk::console
