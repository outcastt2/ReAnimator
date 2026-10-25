-- The animation menu: the .rska clips in <game>/CustomAnimations, played on your skater.
--
-- The page lives in the menu's PLUGINS section. Every clip is a button; the one playing is marked and
-- drawn as the card's main action. The page is rebuilt while it is open, so the highlight follows what
-- the animation system is actually playing rather than only what was last clicked.
--
-- reskate.animation.play("rska:<name>") is the same clip the console's `poseanim <name>` plays.

local current = nil   -- the clip this page started, while it is still playing
local failure = ""    -- the last play that did not start

local function refresh()
    -- It may have ended, or the console may have stopped or replaced it.
    if current and not reskate.animation.playing() then current = nil end
end

reskate.menu.page("Animations", function(ui)
    refresh()

    ui:heading("Custom animations")
    local names = reskate.animation.rska_list()

    if #names == 0 then
        ui:note("No .rska files in CustomAnimations yet.")
        ui:note("Put them in the game folder's CustomAnimations folder.")
        return
    end

    if current then
        local at = reskate.animation.time()
        ui:info("Playing", at and string.format("%s   %.2f s", current, at) or current)
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
            local ok, why = reskate.animation.play("rska:" .. name, { loop = true })
            if ok then
                current = name
                failure = ""
            else
                failure = tostring(why)
            end
        end
    end
end)
