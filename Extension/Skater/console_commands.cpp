#include "Gestures/board_gesture.h"
#include "Extension/Console/commands.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "ai_skaters.h"
#include "Skitch/player_skitch.h"
#include "Skitch/drag_state.h"
#include "effect_attach.h"
#include "custom_animation.h"
#include "prop_attach.h"
#include "drone_flight.h"
#include "drone_outfit.h"
#include "Extension/Multiplayer/Remote/native_prop.h"
#include "Extension/Multiplayer/Remote/drone_sound.h"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>
#include <vector>
namespace dingosdk::console {
void register_movement_commands(Commands &registry) {
    auto skitch = variable("skitch", "Player hip towing: hold V or R1 (both rebindable) nearby", Group::movement, argument("0|1", Type::boolean));
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
        {"firstperson", "debug.first_person", "See through the skater's eyes", Debug::set_first_person,
         &overlay::DebugModel::first_person, &overlay::DebugModel::camera_available},
        {"dronefollow", "drone.follow", "Skate while the FPV drone flies itself behind you, as if someone were flying it",
         Debug::set_drone_follow, &overlay::DebugModel::drone_follow, &overlay::DebugModel::camera_available},
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
    auto drone_mode = variable("dronemode",
        "Fly the free camera as an FPV drone. Easiest from Addons > Drone in the ReSkate menu (Insert): Start flying",
        Group::movement, argument("0|1", Type::boolean));
    drone_mode.inspect = [](const Model &m) {
        return boolean_state(m.debug.available, drone::is_enabled(), "Waiting for local player controls.",
                             "Flies while freecam is on");
    };
    drone_mode.run = [](const Model &m, const Values &args, const Output &out) {
        const bool on = std::get<bool>(args[0]);
        drone::set_enabled(on);
        if (on) { drone::set_armed(true); drone::reset_flag().store(false); }
        if (!on) out("Drone mode off. Freecam flies normally again (freecam 0 takes you back to your skater).");
        else if (!m.debug.free_camera) out("Drone mode on. Turn on freecam (freecam 1) to take off.");
        else out("Drone mode on. W/S: tilt forward/back. A/D: roll left/right. E/Q: throttle up/down. Mouse: turn.");
    };
    drone_mode.reset = [](const Model &, const Output &) { drone::set_enabled(false); };
    registry.add(std::move(drone_mode));
    auto tune_name = argument("setting", Type::text, true);
    for (const auto &knob : drone::knob_info) tune_name.choices.emplace_back(knob.name);
    tune_name.choices.emplace_back("reset");
    auto tune_value = argument("value", Type::number, true);
    auto tune = action("dronetune",
        "Change a drone setting: dronetune <setting> <value>. On its own it lists every setting; 'dronetune reset' puts them all back",
        Group::movement, {tune_name, tune_value});
    tune.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    tune.run = [](const Model &, const Values &args, const Output &out) {
        const auto show = [&out](std::size_t i) {
            out(std::format("{:<9} {:<8g} {} (default {:g}, range {:g}-{:g})", drone::knob_info[i].name,
                            drone::knobs().value[i].load(), drone::knob_info[i].help, drone::knob_info[i].def,
                            drone::knob_info[i].lo, drone::knob_info[i].hi));
        };
        if (args.empty()) {
            for (std::size_t i = 0; i < drone::knob_count; ++i) show(i);
            return;
        }
        const auto name = std::get<std::string>(args[0]);
        if (name == "reset") {
            std::vector<std::pair<std::string, Json>> saved;
            for (std::size_t i = 0; i < drone::knob_count; ++i) {
                drone::knobs().value[i] = drone::knob_info[i].def;
                saved.emplace_back(std::format("Drone.Tune.{}", drone::knob_info[i].name),
                                   static_cast<double>(drone::knob_info[i].def));
            }
            profile_runtime::set_local_values(saved);
            out("Every drone setting is back to its default.");
            return;
        }
        const int index = drone::find_knob(name);
        if (index < 0) {
            out(std::format("error: There is no setting called '{}'. Type dronetune on its own to see them all.", name));
            return;
        }
        const auto i = static_cast<std::size_t>(index);
        if (args.size() < 2) {
            show(i);
            return;
        }
        const auto wanted = static_cast<float>(std::get<double>(args[1]));
        const auto value = std::clamp(wanted, drone::knob_info[i].lo, drone::knob_info[i].hi);
        drone::knobs().value[i] = value;
        profile_runtime::set_local_values({{std::format("Drone.Tune.{}", name), static_cast<double>(value)}});
        out(value == wanted ? std::format("{} = {:g}", name, value)
                            : std::format("{} = {:g} (the closest allowed value)", name, value));
    };
    registry.add(std::move(tune));
    auto menu_command = action("dronemenu", "Open the ReSkate menu on Addons > Drone", Group::movement);
    menu_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    menu_command.run = [](const Model &, const Values &, const Output &out) {
        drone::menu_flag().store(true);
        out("Opening Addons > Drone.");
    };
    registry.add(std::move(menu_command));
    auto open_menu = action("menu", "Open the ReSkate menu (bind it to a controller combo to open it with a pad)", Group::console);
    open_menu.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    open_menu.run = [](const Model &, const Values &, const Output &out) {
        overlay::menu_open_flag().store(true);
        out("Opening the ReSkate menu.");
    };
    registry.add(std::move(open_menu));
    auto arm_command = action("dronearm", "Arm the drone: motors on, ready to fly", Group::movement);
    arm_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    arm_command.run = [](const Model &, const Values &, const Output &out) {
        drone::set_armed(true);
        out("Armed.");
    };
    registry.add(std::move(arm_command));
    auto disarm_command = action("dronedisarm",
        "Disarm the drone: motors off and the sticks do nothing. It falls (or hangs in the air with dronetune disarmfreeze 1)", Group::movement);
    disarm_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    disarm_command.run = [](const Model &, const Values &, const Output &out) {
        drone::set_armed(false);
        out("Disarmed.");
    };
    registry.add(std::move(disarm_command));
    auto reset_command = action("dronereset",
        "Put the drone back where it took off, with a fresh battery and timer", Group::movement);
    reset_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    reset_command.run = [](const Model &, const Values &, const Output &out) {
        drone::request_reset();
        out("Back to the takeoff point.");
    };
    registry.add(std::move(reset_command));
    auto radio_mode = variable("droneradio",
        "Fly the drone with a controller: a gamepad, or an RC radio in USB Joystick mode",
        Group::movement, argument("0|1", Type::boolean));
    radio_mode.inspect = [](const Model &m) {
        return boolean_state(m.debug.available, drone::radio::config().enabled.load(), "Waiting for local player controls.",
                             "Set it up in Addons > Drone (dronemenu), CONTROLLER tab");
    };
    radio_mode.run = [](const Model &, const Values &args, const Output &out) {
        const bool on = std::get<bool>(args[0]);
        drone::radio::config().enabled = on;
        profile_runtime::set_local_values({{"Drone.Radio", on ? 1.0 : 0.0}});
        out(on ? "Controller on. If a stick does the wrong thing, run Set up controller (dronemenu, CONTROLLER tab)."
               : "Controller off. The drone uses the keyboard and mouse.");
    };
    radio_mode.reset = [](const Model &, const Output &) {
        drone::radio::config().enabled = true;
        profile_runtime::set_local_values({{"Drone.Radio", 1.0}});
    };
    registry.add(std::move(radio_mode));
    // A test toward the drone's 3D model (Multiplayer/Remote/native_prop.h): can a spawned prop
    // be moved every frame?
    auto model_name = argument("blueprint", Type::text, true);
    auto model_mode = argument("mode", Type::integer, true);
    auto model_test = action("dronemodeltest",
        "Test for the drone model: spawns the drone (dronemodeltest drone), the traffic cone (cone) or a named blueprint in front of you and moves it "
        "round a circle. Mode 0, 1 or 2 tries a different way of moving it. 'dronemodeltest remote' has a pretend other player fly a drone "
        "in front of you, shown as theirs would be. 'dronemodeltest stop' removes them; on its own it reports",
        Group::movement, {model_name, model_mode});
    model_test.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    model_test.run = [](const Model &, const Values &args, const Output &out) {
        const auto name = args.empty() ? std::string() : std::get<std::string>(args[0]);
        const int mode = args.size() > 1 ? static_cast<int>(std::get<std::int64_t>(args[1])) : 1;
        if (name.empty() || name == "status") {
            out(multiplayer::prop_test_status());
            out("Other players' drones: " + multiplayer::drone_models_status());
            out("Their sound: " + multiplayer::drone_sounds_status());
            return;
        }
        if (name == "stop") {
            multiplayer::request_prop_test({}, 0);
            multiplayer::request_fake_remote_drone(false);
            out("Removing the test prop.");
            return;
        }
        if (name == "remote") {
            multiplayer::request_fake_remote_drone(true);
            out("A pretend other player takes off in front of you. Run dronemodeltest again in a few seconds for what happened.");
            return;
        }
        const std::string blueprint = name == "drone" ? std::string(multiplayer::drone_model_blueprint)
                                    : name == "cone" || name == "start" ? std::string(multiplayer::prop_test_default) : name;
        multiplayer::request_prop_test(blueprint, mode);
        out(std::format("Spawning {} with mode {}. Run dronemodeltest again in a moment for what happened.", blueprint, mode));
    };
    registry.add(std::move(model_test));
    auto radio_check = action("droneradiocheck", "Show what your controller is sending right now, and how the drone reads it", Group::movement);
    radio_check.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    radio_check.run = [](const Model &, const Values &, const Output &out) {
        const auto sample = drone::radio::poll();
        if (!sample.valid) {
            out("No controller found. Plug in a gamepad, or an RC radio with its USB mode set to Joystick, then try again.");
            return;
        }
        out(std::format("Controller: {}", sample.name));
        out(std::format("Raw axes 1-6: {:+.2f} {:+.2f} {:+.2f} {:+.2f} {:+.2f} {:+.2f}", sample.axes[0], sample.axes[1],
                        sample.axes[2], sample.axes[3], sample.axes[4], sample.axes[5]));
        std::string pressed;
        for (int n = 1; n <= 32; ++n)
            if (sample.buttons & (1u << (n - 1))) pressed += (pressed.empty() ? "" : " ") + std::to_string(n);
        out(std::format("Buttons pressed: {}", pressed.empty() ? "none" : pressed));
        const auto map = drone::radio::mapping_snapshot();
        const auto cmd = drone::read_sticks(sample.axes);
        out(std::format("Mapping (roll pitch throttle yaw): {} {} {} {}", map[0], map[1], map[2], map[3]));
        out(std::format("Drone reads: roll {:+.2f} (right +)  pitch {:+.2f} (forward +)  throttle {:.2f}  yaw {:+.2f} (right +)",
                        cmd.roll, cmd.pitch, cmd.throttle, cmd.yaw));
        out("A stick doing the wrong thing? Easiest fix: dronemenu, CONTROLLER tab, Set up controller.");
    };
    registry.add(std::move(radio_check));
    const auto axis_argument = [](const char *name) {
        auto a = argument(name, Type::number);
        a.minimum = -6;
        a.maximum = 6;
        return a;
    };
    // Finding the cosmetics the drone could put on the pilot (goggles while flying): see drone_outfit.h.
    auto items_command = action("droneitems",
        "List installed cosmetics whose name contains a word (droneitems goggle), or your skater's outfit slots (droneitems slots)",
        Group::movement, {argument("word", Type::text)});
    items_command.inspect = [](const Model &) { return State{true, {}, {}, {}, false}; };
    items_command.run = [](const Model &, const Values &args, const Output &out) {
        const auto word = std::get<std::string>(args[0]);
        try {
            if (word == "slots") {
                out("Your skater's outfit slots (number, slot, what it holds now):");
                for (const auto &line : drone::outfit::slots()) out(line);
                return;
            }
            std::size_t total{};
            const auto found = drone::outfit::find_items(word, 80, total);
            out(std::format("{} installed item(s) contain '{}'{}", total, word, total > found.size() ? " (the first 80):" : ":"));
            for (const auto &name : found) out("  " + name);
        } catch (const std::exception &error) {
            out(std::format("error: {}", error.what()));
        }
    };
    registry.add(std::move(items_command));
    auto radio_axes = action("droneaxes", "Choose which controller axis is roll, pitch, throttle and yaw (1-6 each; a minus reverses one). Set up controller does this for you",
        Group::movement, {axis_argument("roll"), axis_argument("pitch"), axis_argument("throttle"), axis_argument("yaw")});
    radio_axes.inspect = [](const Model &) {
        const auto map = drone::radio::mapping_snapshot();
        return State{true, std::format("{} {} {} {}", map[0], map[1], map[2], map[3]), {}, {}, false};
    };
    radio_axes.run = [](const Model &, const Values &args, const Output &out) {
        std::array<int, 4> next{};
        for (std::size_t i = 0; i < 4; ++i) {
            next[i] = static_cast<int>(std::lround(std::get<double>(args[i])));
            if (next[i] == 0 || std::abs(next[i]) > 6) {
                out("error: Each axis must be 1 to 6, or -1 to -6 to reverse it.");
                return;
            }
        }
        std::vector<std::pair<std::string, Json>> saved;
        for (std::size_t i = 0; i < 4; ++i) {
            drone::radio::config().map[i] = next[i];
            saved.emplace_back(std::format("Drone.Axis{}", i), static_cast<double>(next[i]));
        }
        profile_runtime::set_local_values(saved);
        out(std::format("Axes saved: roll {} pitch {} throttle {} yaw {}", next[0], next[1], next[2], next[3]));
    };
    registry.add(std::move(radio_axes));
    auto speed = argument("speed", Type::number);
    // Any speed in range, so the drone's presets and top speed slider can set theirs; the usual
    // ones are still offered as completions.
    speed.minimum = 0.6;
    speed.maximum = 1500.0;
    speed.complete = [](const Model &, auto) {
        return std::vector<std::string>{"0.6", "3", "5", "15", "60", "300", "1500"};
    };
    auto flight = variable("flyspeed", "Free-flight speed in world units per second (0.6-1500)", Group::movement, speed);
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
    auto mask = argument("button_or_key", Type::unsigned_integer);
    mask.minimum = 0;
    mask.maximum = UINT32_MAX;
    auto freecamcontroller =
        action("freecam_controller", "Block input and use controller for Freecam", Group::movement, {argument("enabled", Type::boolean)});
    freecamcontroller.inspect = [](const Model &m) {
        return boolean_state(m.bindings.available, m.bindings.freecam_controller, "Controller bindings are unavailable.");
    };
    freecamcontroller.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_freecam_controller(std::get<bool>(args[0]));
        out(saved ? "" : "error: Invalid setting or save failed.");
    };
    registry.add(std::move(freecamcontroller));

    auto freecamcontroller_bind =
        action("bind freecamcontroller", "Bind a controller combo or keyboard key to freecam controller toggle; 0 clears the binding", Group::movement, {mask});
    freecamcontroller_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    freecamcontroller_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_freecam_controller_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(freecamcontroller_bind));

    auto tp_to_freecam =
        action("tp_to_freecam", "Teleport to freecam position and disable freecam", Group::movement);
    tp_to_freecam.inspect = [](const Model &m) {
        return boolean_state(m.debug.camera_available && m.debug.camera_position_valid, m.debug.free_camera, 
            !m.debug.camera_position_valid ? "Camera position unavailable" : "Camera controls unavailable");
    };
    tp_to_freecam.run = [](const Model &m, const Values &, const Output &) {
        if (!m.debug.camera_position_valid) return;
        teleport_local_skater(m.debug.camera_position);
        if (m.debug.free_camera)
            request_debug(overlay::DebugAction::set_free_camera, false);
    };
    registry.add(std::move(tp_to_freecam));

    auto tp_to_freecam_bind =
        action("bind tptofreecam", "Bind a controller combo or keyboard key to TP to Freecam; 0 clears the binding", Group::movement, {mask});
    tp_to_freecam_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    tp_to_freecam_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_tp_to_freecam_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(tp_to_freecam_bind));
    auto freecam_bind =
        action("bind freecam", "Bind a controller combo or keyboard key to freecam; 0 clears the binding", Group::movement, {mask});
    freecam_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    freecam_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_freecam_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(freecam_bind));
    auto bind =
        action("bind noclip", "Bind a controller combo or keyboard key to noclip; 0 clears the binding", Group::movement, {mask});
    bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_noclip_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(bind));
    auto boost_bind =
        action("bind forwardvelocity", "Bind a controller combo or keyboard key to Forward Boost; 0 clears the binding", Group::movement, {mask});
    boost_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    boost_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_forward_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(boost_bind));
    auto up_bind =
        action("bind upvelocity", "Bind a controller combo or keyboard key to Up Boost; 0 clears the binding", Group::movement, {mask});
    up_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    up_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_up_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(up_bind));
    auto skitch_bind =
        action("bind skitch", "Bind a controller combo or keyboard key to Skitch; 0 clears the binding (default R1)", Group::movement, {mask});
    skitch_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    skitch_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_skitch_combo_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(skitch_bind));
    auto skitch_key_argument = argument("virtual_key", Type::unsigned_integer);
    skitch_key_argument.minimum = 0;
    skitch_key_argument.maximum = 255;
    auto skitch_key_bind = action("bind skitchkey",
        "Bind the keyboard key for player skitching; 0 disables the keyboard key (the controller binding remains)",
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
    auto skitch_toggle_bind =
        action("bind skitchtoggle", "Bind a controller combo or keyboard key to Skitch toggle (press once to latch the grab); 0 clears it", Group::movement, {mask});
    skitch_toggle_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    skitch_toggle_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_skitch_toggle_combo_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(skitch_toggle_bind));
    auto skitch_toggle_key_bind = action("bind skitchtogglekey",
        "Bind the keyboard key for Skitch toggle (press once to latch the grab); 0 disables the keyboard key",
        Group::movement, {skitch_key_argument});
    skitch_toggle_key_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    skitch_toggle_key_bind.run = [](const Model &, const Values &args, const Output &out) {
        const auto key = static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0]));
        const bool saved = set_local_skitch_toggle_key_binding(key);
        out(saved ? local_profile_controller_bindings().status : "error: Invalid key or save failed.");
    };
    registry.add(std::move(skitch_toggle_key_bind));

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
        "Hand props: prop | prop list [name] | prop watch [name] [seconds] | prop clip <name> | prop codec [name] [seconds] | prop state | prop trigger [seconds] | prop trace [seconds] - gesture props, the definition cluster, and the clip probes",
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
        if (mode == "clip") {
            skater::request_prop_clip(name);
            out("Clip probe: resolving \"" + name + "\", following its +0x28 data and comparing against the rig's "
                "output pose (see the log). Trigger that gesture first so it is loaded.");
            return;
        }
        if (mode == "codec") {
            // Lanes: `prop codec`, `prop codec <seconds>`, `prop codec <clip> <seconds>`.
            auto seconds = number_at(2, 0u);
            auto clip = name;
            if (!seconds) {
                seconds = number_at(1, 20u);
                if (clip == std::to_string(seconds)) clip.clear();
            }
            skater::request_prop_codec(clip, seconds);
            out("Clip codec: scanning the game modules for the EDAA codec word, then watching the clip's arenas for " +
                std::to_string(seconds) + "s (see the log). Run it FIRST, then trigger the gesture and keep it looping "
                "until the watch ends.");
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
        if (mode == "state") {
            skater::request_prop_state();
            out("Hand props: dumping the definition cluster (rig+0x18, +0x1c0..+0x1cc) and what its pointer is. "
                "Run it once idle and once with the gesture held, then compare the two.");
            return;
        }
        if (mode == "trigger") {
            if (text_at(1) == "off") {
                skater::request_prop_trigger_stop();
                out("Hand props: releasing the trigger hold and restoring the idle cluster.");
                return;
            }
            const auto seconds = number_at(1, 45u);
            skater::request_prop_trigger(seconds);
            out("Hand props: armed for " + std::to_string(seconds) +
                "s. Press the gesture once (that is the capture), then stand still and watch your skater; "
                "'prop trigger off' releases.");
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
        "Custom animation: poseanim test | record | off | play | save <path> | mask auto|full|legs | ik <0..1> | "
        "trace 0|1 | <file>.rska [ik|noik]",
        Group::movement, {argument("clip", Type::text, true), argument("path", Type::text, true),
                          argument("flag", Type::text, true)});
    poseanim.execution = Execution::local;
    poseanim.inspect = [](const Model &) {
        return State{true, {}, {},
                     skater::pose_playback_status() + "  [mask: " + skater::pose_mask_name() +
                         ", ik: " + std::to_string(skater::pose_ik()).substr(0, 4) + "]", false};
    };
    poseanim.run = [](const Model &, const Values &args, const Output &out) {
        if (args.empty()) {
            out(skater::pose_playback_status());
            out("Masking: " + skater::pose_mask_name() + ".");
            out("Floor correction: " + std::to_string(skater::pose_ik()).substr(0, 4) +
                " (per clip: poseanim <file> ik opts a clip in; off by default).");
            out(skater::pose_playback_cost());
            out(std::string("Layer trace: ") + (skater::pose_trace() ? "on" : "off") + " (poseanim trace 0|1).");
            return;
        }
        const auto clip = std::get<std::string>(args[0]);
        if (lower(clip) == "off") { skater::request_pose_playback_stop(); out("Stopping custom animation..."); return; }
        if (lower(clip) == "ik") {
            // How hard a clip that sinks below the game's planted feet is
            // lifted back. 0 leaves the clip exactly as authored.
            const auto mode = args.size() > 1 ? lower(std::get<std::string>(args[1])) : std::string{};
            char *end = nullptr;
            const float strength = mode.empty() ? -1.0f : std::strtof(mode.c_str(), &end);
            if (end == mode.c_str() || strength < 0.0f || strength > 1.0f) {
                out("usage: poseanim ik <0..1>  (currently " + std::to_string(skater::pose_ik()).substr(0, 4) + ")");
                return;
            }
            skater::set_pose_ik(strength);
            out(strength > 0.0f
                    ? "Floor correction: " + std::to_string(strength).substr(0, 4) +
                          " (the clip's feet and hands are lifted above the game's floor)."
                    : "Floor correction off: the clip's limbs are written as authored.");
            return;
        }
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
                // An optional trailing ik/noik opts this clip into floor
                // correction and is remembered for the next run of the same name.
                if (args.size() > 2) {
                    const auto flag = lower(std::get<std::string>(args[2]));
                    if (flag != "ik" && flag != "noik") {
                        out("usage: poseanim play <file>.rska [ik|noik]");
                        return;
                    }
                    skater::set_pose_clip_ik(name, flag == "ik");
                }
                skater::request_pose_playback(name);
                out("Playing " + name +
                    (skater::pose_clip_ik(name) ? " (floor correction on for this clip)..." : "..."));
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
        // A bare file name plays it; an optional trailing ik/noik opts this
        // clip into floor correction -- "poseanim dance ik" plays the clip with
        // the leg, foot and torso correction on and remembers that for the
        // next run of the same name. Without the flag a clip is written
        // exactly as authored.
        if (args.size() > 1) {
            const auto flag = lower(std::get<std::string>(args[1]));
            if (flag != "ik" && flag != "noik") {
                out("usage: poseanim <file>.rska [ik|noik]");
                return;
            }
            skater::set_pose_clip_ik(clip, flag == "ik");
        }
        skater::request_pose_playback(clip);
        out("Starting custom animation " + clip +
            (skater::pose_clip_ik(clip) ? " (floor correction on for this clip)..." : "..."));
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
    auto offboard_up_bind =
        action("bind offboardupvelocity", "Bind a controller combo or keyboard key to Off-board Up Boost; 0 clears the binding", Group::movement, {mask});
    offboard_up_bind.inspect = [](const Model &m) {
        return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
    };
    offboard_up_bind.run = [](const Model &, const Values &args, const Output &out) {
        const bool saved = set_local_offboard_up_velocity_binding(static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
        out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
    };
    registry.add(std::move(offboard_up_bind));
    for (const bool yes : {true, false}) {
        auto vote_bind = action(yes ? "bind voteyes" : "bind voteno",
            yes ? "Bind a controller combo or keyboard key to Yes in a server's vote; 0 clears the binding"
                : "Bind a controller combo or keyboard key to No in a server's vote; 0 clears the binding", Group::movement, {mask});
        vote_bind.inspect = [](const Model &m) {
            return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
        };
        vote_bind.run = [yes](const Model &, const Values &args, const Output &out) {
            const bool saved = set_local_vote_binding(yes, static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
            out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
        };
        registry.add(std::move(vote_bind));
    }
    // The switches a player can put on a button (action_binds): each bind runs its command.
    for (std::size_t i = 0; i < action_binds.size(); ++i) {
        const auto &slot = action_binds[i];
        auto entry = action("bind " + std::string(slot.name),
            "Bind a controller combo or keyboard key to: " + std::string(slot.label) + " (" + std::string(slot.command) + "); 0 clears the binding",
            Group::movement, {mask});
        entry.inspect = [](const Model &m) {
            return State{m.bindings.available, {}, "Controller bindings are unavailable.", {}, false};
        };
        entry.run = [i](const Model &, const Values &args, const Output &out) {
            const bool saved = set_local_action_binding(i, static_cast<std::uint32_t>(std::get<std::uint64_t>(args[0])));
            out(saved ? local_profile_controller_bindings().status : "error: Invalid binding or save failed.");
        };
        registry.add(std::move(entry));
    }
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
