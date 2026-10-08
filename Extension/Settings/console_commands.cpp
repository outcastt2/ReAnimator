#include "Extension/Console/commands.h"
#include "named_settings.h"
#include "gameplay_settings_override.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/Profile/local_profile_runtime.h"
#include <format>
namespace dingosdk::console {
namespace {
const NamedSettingModel *named_model(const Model &model, std::size_t index, std::string_view name) {
    // The runtime only appends names, so the registry's index normally still
    // matches. Avoid searching thousands of rows for every autocomplete suggestion.
    if (index < model.engine_settings.size() && equal(model.engine_settings[index].name, name))
        return &model.engine_settings[index];
    for (const auto &setting : model.engine_settings)
        if (equal(setting.name, name)) return &setting;
    return nullptr;
}
std::string join(const std::vector<std::string> &values) {
    std::string result;
    for (const auto &value : values) {
        if (!result.empty()) result += ", ";
        result += value;
    }
    return result;
}
}
void register_settings_commands(Commands &registry) {
    using GroupModel = overlay::OfflineFeatureGroupModel;
    struct Setting {
        const char *name;
        const char *alias;
        const char *description;
        overlay::OfflineFeatureGroup group;
        GroupModel overlay::OfflineFeatureModel::*member;
    };
    const Setting settings[]{
        {"activities", "feature.activities", "Enable offline activity features",
         overlay::OfflineFeatureGroup::activities, &overlay::OfflineFeatureModel::activities},
        {"fasttravel", "feature.fast_travel", "Enable fast travel features", overlay::OfflineFeatureGroup::fast_travel,
         &overlay::OfflineFeatureModel::fast_travel},
        {"progression enabled", "feature.progression", "Enable offline progression systems",
         overlay::OfflineFeatureGroup::progression, &overlay::OfflineFeatureModel::progression},
        {"developermenus", "feature.developer_menus", "Enable the available developer menus",
         overlay::OfflineFeatureGroup::developer_menus, &overlay::OfflineFeatureModel::developer_menus},
        {"boardwear", "feature.board_wear", "Enable the shipped board wear graph",
         overlay::OfflineFeatureGroup::board_wear, &overlay::OfflineFeatureModel::board_wear},
        {"playercollision", "feature.player_collision", "Allow the game's own party collision option",
         overlay::OfflineFeatureGroup::player_collision, &overlay::OfflineFeatureModel::player_collision}};
    for (const auto &setting : settings) {
        auto entry = variable(setting.name, setting.description, Group::gameplay, argument("0|1", Type::boolean));
        entry.aliases = {setting.alias};
        entry.inspect = [setting](const Model &m) {
            const auto &value = m.offline.*setting.member;
            auto result = boolean_state(value.available, value.effective, "These engine settings are unavailable.");
            result.overridden = value.override_active;
            return result;
        };
        entry.run = [setting](const Model &, const Values &args, const Output &) {
            request_feature(setting.group, std::get<bool>(args[0]));
        };
        entry.reset = [setting](const Model &, const Output &) { request_feature(setting.group, false); };
        registry.add(std::move(entry));
    }
    // Definitions and runtime reads come from the same native field contract.
    for (const auto &definition : gameplay_engine_variable_definitions()) {
        auto entry = variable(definition.name, definition.description, Group::engine, argument("0|1", Type::boolean));
        entry.aliases = {definition.qualified_name};
        entry.inspect = [name = definition.name](const Model &m) {
            const auto it = std::find_if(m.offline.variables.begin(), m.offline.variables.end(),
                                         [&](const auto &v) { return equal(name, v.name); });
            if (it == m.offline.variables.end())
                return State{false, {}, "The engine settings object has not been resolved.", {}, false};
            auto result =
                boolean_state(it->available, it->value, "The engine field is unavailable for the current game state.");
            result.overridden = it->override_active;
            result.detail = it->override_active ? "Session override active" : "Engine value";
            return result;
        };
        entry.run = [name = definition.name](const Model &, const Values &args, const Output &) {
            request_engine_variable(name, false, std::get<bool>(args[0]));
        };
        entry.reset = [name = definition.name](const Model &, const Output &) {
            request_engine_variable(name, true, false);
        };
        registry.add(std::move(entry));
    }
    // Engine settings are listed from the game's own settings registry
    // (named_settings.cpp); names seen in earlier sessions are included.
    const auto names = named_setting_names();
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto &name = names[index];
        if (registry.find(name) || !parse_console_command(name))
            continue;
        auto value = argument("value");
        value.complete = [index, name](const Model &m, auto) {
            if (const auto *v = named_model(m, index, name))
                return v->choices;
            return std::vector<std::string>{};
        };
        auto entry = variable(name, {}, Group::engine, std::move(value));
        entry.inspect = [index, name](const Model &m) {
            if (const auto *v = named_model(m, index, name))
                return State{v->available, v->available ? std::optional<std::string>(v->value) : std::nullopt,
                             v->reason, v->type + (v->override_active ? "; session override" : "; engine value"),
                             v->override_active};
            return State{false, {}, "Waiting for native settings on the game update thread.", {}, false};
        };
        entry.run = [name](const Model &, const Values &args, const Output &out) {
            out(request_named_setting(name, std::get<std::string>(args[0]), false));
        };
        entry.reset = [name](const Model &, const Output &out) { out(request_named_setting(name, {}, true)); };
        registry.add(std::move(entry));
    }
    auto scale = argument("scale", Type::number);
    scale.minimum = min_menu_scale;
    scale.maximum = max_menu_scale;
    auto menu = variable("ui scale", "Scale the ReSkate menu and its text", Group::console, std::move(scale));
    menu.execution = Execution::local;
    menu.inspect = [](const Model &m) {
        return State{true, std::format("{:.2f}", m.menu_scale), {}, "Saved with your profile.", false};
    };
    menu.run = [](const Model &, const Values &args, const Output &out) {
        const auto value = static_cast<float>(std::get<double>(args[0]));
        out(profile_runtime::set_local_menu_scale(value)
                ? std::format("Menu scale set to {:.2f}.", value)
                : std::format("error: Choose a scale from {:.2f} to {:.2f}.", min_menu_scale, max_menu_scale));
    };
    menu.reset = [](const Model &, const Output &out) {
        profile_runtime::set_local_menu_scale(default_menu_scale);
        out("Menu scale reset.");
    };
    registry.add(std::move(menu));

    // Wearing nothing in a slot is a policy over the whole character rather than an edit to one
    // preset, so the saved outfits stay wearable. The game renders an empty asset as nothing,
    // which is the state its own unworn slots are already in.
    // Both arguments are optional so that a bare "hideslot" can report; the registry
    // rejects a call whose arity does not match, before run() is ever reached.
    auto hide_on = argument("0|1", Type::boolean, true);
    auto hideslot = action("hideslot",
        "Hide a cosmetic slot on every outfit: hideslot shoes, hideslot shoes 0, hideslot clear",
        Group::gameplay, {argument("slot", Type::text, true), std::move(hide_on)});
    hideslot.execution = Execution::local;
    hideslot.inspect = [](const Model &) {
        const auto known = !profile_runtime::cosmetic_slot_names().empty();
        if (!known)
            return State{false, std::nullopt,
                "Load a level first: the slot names come from the character's recipe.",
                "No slots hidden.", false};
        const auto hidden = join(profile_runtime::hidden_cosmetic_slots());
        return State{true, hidden.empty() ? std::optional<std::string>{} : std::optional{hidden},
            {}, hidden.empty() ? "No slots hidden." : "Takes effect when the outfit next loads.", false};
    };
    hideslot.run = [](const Model &, const Values &args, const Output &out) {
        // Omitted optional arguments leave Values short, so arity is the size.
        if (args.empty()) {
            const auto hidden = join(profile_runtime::hidden_cosmetic_slots());
            out(hidden.empty() ? "No slots hidden." : "Hidden: " + hidden);
            return;
        }
        const auto slot = std::get<std::string>(args[0]);
        if (lower(slot) == "list") {
            out("Slots: " + join(profile_runtime::cosmetic_slot_names()));
            return;
        }
        if (lower(slot) == "clear") {
            for (const auto& name : profile_runtime::hidden_cosmetic_slots())
                profile_runtime::set_cosmetic_slot_hidden(name, false);
            out("All slots shown again. Takes effect when the outfit next loads.");
            return;
        }
        const bool hide = args.size() < 2 || std::get<bool>(args[1]);
        // Logged as well as printed: the console only logs its errors, so without this
        // there is no record in the log that the command ran at all.
        if (!profile_runtime::set_cosmetic_slot_hidden(slot, hide)) {
            profile_runtime::cosmetic_diagnostic("hide_slot", "unknown_slot", slot);
            out("error: Unknown slot \"" + slot + "\". Try: hideslot list");
            return;
        }
        profile_runtime::cosmetic_diagnostic("hide_slot", hide ? "hidden" : "shown", slot);
        out(std::format("{} {}. Takes effect when the outfit next loads.", slot, hide ? "hidden" : "shown"));
    };
    hideslot.reset = [](const Model &, const Output &out) {
        for (const auto& name : profile_runtime::hidden_cosmetic_slots())
            profile_runtime::set_cosmetic_slot_hidden(name, false);
        out("All slots shown again. Takes effect when the outfit next loads.");
    };
    registry.add(std::move(hideslot));
}
} // namespace dingosdk::console
