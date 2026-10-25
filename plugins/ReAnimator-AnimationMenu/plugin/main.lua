-- The animation menu: the .rska clips in <game>/CustomAnimations, played on your skater.
--
-- Each clip keeps two settings while the game runs: the floor correction ("ik") and which joints the
-- clip drives (the mask). A clip that sits right with the correction is often one whose legs go
-- bow-legged with it, so they are per clip rather than global.
--
-- They are set explicitly on every play. The animation system's correction and mask are global state
-- that `poseanim` also writes, so without this the menu would inherit whatever the console last left
-- behind -- which is why every clip used to start corrected.
--
-- reskate.animation.play("rska:<name>") is the same clip the console's `poseanim <name>` plays.

local current = nil    -- the clip this page started, while it is still playing
local failure = ""     -- the last play that did not start
local settings = {}    -- [name] = { floor = 0..1, mask = "auto" | "full" | "legs" }

local masks = { "auto", "full", "legs" }

local function settings_of(name)
    local s = settings[name]
    if not s then
        -- Off, and automatic: what `poseanim <file>` does with no argument.
        s = { floor = 0, mask = "auto" }
        settings[name] = s
    end
    return s
end

local function mask_index(mode)
    for i, m in ipairs(masks) do
        if m == mode then return i end
    end
    return 1
end

local function play(name)
    local s = settings_of(name)
    -- Before it starts, so the first frame is already the one asked for.
    reskate.animation.floor(s.floor)
    reskate.animation.mask(s.mask)
    local ok, why = reskate.animation.play("rska:" .. name, { loop = true })
    if not ok then
        failure = tostring(why)
        return false
    end
    failure = ""
    return true
end

reskate.menu.page("Animations", function(ui)
    if current and not reskate.animation.playing() then current = nil end

    ui:heading("Custom animations")
    local names = reskate.animation.rska_list()
    if #names == 0 then
        ui:note("No .rska files in CustomAnimations yet.")
        ui:note("Put them in the game folder's CustomAnimations folder.")
        return
    end

    if current then
        local s = settings_of(current)
        local at = reskate.animation.time()
        ui:info("Playing", at and string.format("%s   %.2f s", current, at) or current)

        ui:card("SETTINGS", "For " .. current)
        local corrected = ui:toggle("ik", "Floor correction (ik)", s.floor > 0,
            "Lifts the clip's limbs clear of the floor the game planted. Off is what poseanim does by default.")
        s.floor = corrected and 1 or 0
        local tab = ui:tabs("mask", { "AUTO", "FULL", "LEGS" }, mask_index(s.mask))
        s.mask = masks[tab] or "auto"
        ui:note("auto: the game keeps the legs while you ride. full: the clip drives everything. legs: always the game's.")
        -- Apply as they are changed, not only on the next play.
        reskate.animation.floor(s.floor)
        reskate.animation.mask(s.mask)

        if ui:button("stop", "STOP", false) then
            reskate.animation.stop()
            current = nil
        end
    else
        ui:note("Pick a clip to play it on your skater.")
    end

    if failure ~= "" then ui:warn(failure) end

    ui:card("CLIPS", string.format("%d clip%s", #names, #names == 1 and "" or "s"))
    for _, name in ipairs(names) do
        local playing = (name == current)
        -- "> " marks it as well as the button's own emphasis: the fonts are ASCII, so no arrows.
        if ui:button(name, (playing and "> " or "   ") .. name, playing) then
            if play(name) then current = name end
        end
    end
end)
