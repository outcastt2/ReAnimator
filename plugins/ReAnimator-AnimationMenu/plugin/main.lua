-- The animation grid: every .rska clip in <game>/CustomAnimations as a tile with a stick figure of its
-- pose and the name under it. Hovering previews a clip once the cursor rests on it; clicking pins it.
--
-- A grid needs the plugin to draw it, so this is a screen with panel = false -- the whole screen is ours
-- and reskate.input.cursor() gives the mouse for hover, clicks and the wheel. The menu page is the way
-- in, because the PLUGINS section is where players look.

local screen_title = "Animations"
local screen_open = false

local pinned = nil         -- the clip the player clicked
local previewing = nil     -- the clip a hover started, while it is only a preview
local hovered = nil
local hover_frames = 0
local was_left = false
local scroll = 0
local tick = 0          -- draw frames, for the flipbook

local settings = {}        -- [name] = { floor = 0..1, mask = "auto"|"full"|"legs" }
local thumbs = {}          -- [name] = { {x1,y1,x2,y2}, ... } in a 0..1 box, or false when it failed

local masks = { "auto", "full", "legs" }

-- The joints a thumbnail draws, and the bones between them (indices into that list).
local JOINTS = { 7, 42, 44, 101, 103, 275, 277, 278, 46, 48, 49, 341, 342, 343, 344, 8, 9, 10, 11 }
local BONES = {
    { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 5 },             -- hips, lower back, chest, neck, head
    { 3, 6 }, { 6, 7 }, { 7, 8 },                       -- left shoulder, elbow, hand
    { 3, 9 }, { 9, 10 }, { 10, 11 },                    -- right shoulder, elbow, hand
    { 1, 12 }, { 12, 13 }, { 13, 14 }, { 14, 15 },      -- left leg, knee, foot, toes
    { 1, 16 }, { 16, 17 }, { 17, 18 }, { 18, 19 },      -- right leg, knee, foot, toes
}

local COLS, PAD, GAP, HEADER = 4, 24, 14, 132
-- "#RRGGBB" or "#RRGGBBAA", as the overlay reads a colour.
local C_TEXT, C_DIM = "#FFFFFF", "#B0B0B0"
local C_BG, C_TILE = "#141210E8", "#1E1C1A50"
local C_HOT, C_PIN, C_BONE = "#FFFFFF", "#FFC030", "#D8D8D8"

local function settings_of(name)
    local s = settings[name]
    if not s then
        s = { floor = 0, mask = "auto" }   -- what `poseanim <file>` does with no argument
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
    -- Explicitly, or the clip inherits whatever the console last left in the animation system.
    reskate.animation.floor(s.floor)
    reskate.animation.mask(s.mask)
    local ok, why = reskate.animation.play("rska:" .. name, { loop = true })
    if not ok then reskate.log("animation menu: " .. tostring(why)) end
    return ok
end

-- Three moments through the clip, cycled as a flipbook so a pose reads as movement rather than a
-- snapshot. Each is one frame read from the file, so this costs the same for a 50 MB clip as a small
-- one and nothing is held afterwards.
local POSES = { 0.2, 0.5, 0.8 }
local THUMB_FRAMES = 20          -- draw frames one pose holds

local function build_thumbnail(name)
    local poses = {}
    for _, at in ipairs(POSES) do
        local ok, points = pcall(reskate.animation.world, "rska:" .. name, at, JOINTS)
        if not ok or type(points) ~= "table" or #points < #JOINTS then
            thumbs[name] = false
            return
        end
        poses[#poses + 1] = points
    end
    -- Front view (X across) unless the clip spreads more along Z, which reads better for a pose that is
    -- turned side-on.
    local lx, hx, lz, hz = math.huge, -math.huge, math.huge, -math.huge
    for _, points in ipairs(poses) do
        for _, p in ipairs(points) do
            if p.x < lx then lx = p.x end
            if p.x > hx then hx = p.x end
            if p.z < lz then lz = p.z end
            if p.z > hz then hz = p.z end
        end
    end
    local use_z = (hz - lz) > (hx - lx)
    local function across(p) return use_z and p.z or p.x end
    -- One box for all three poses, so the figure does not jump between them.
    local minx, maxx, miny, maxy = math.huge, -math.huge, math.huge, -math.huge
    for _, points in ipairs(poses) do
        for _, p in ipairs(points) do
            local a = across(p)
            if a < minx then minx = a end
            if a > maxx then maxx = a end
            if p.y < miny then miny = p.y end
            if p.y > maxy then maxy = p.y end
        end
    end
    local span = math.max(maxx - minx, maxy - miny, 1e-3)
    local padx, pady = (1 - (maxx - minx) / span) * 0.5, (1 - (maxy - miny) / span) * 0.5
    local function nx(v) return (v - minx) / span + padx end
    local function ny(v) return 1 - ((v - miny) / span + pady) end
    local frames = {}
    for _, points in ipairs(poses) do
        local lines = {}
        for _, b in ipairs(BONES) do
            local a, c = points[b[1]], points[b[2]]
            lines[#lines + 1] = { nx(across(a)), ny(a.y), nx(across(c)), ny(c.y) }
        end
        frames[#frames + 1] = lines
    end
    thumbs[name] = frames
end

-- The pose to draw now: a tile with nothing yet draws empty until its turn comes.
local function thumbnail(name, tick)
    local frames = thumbs[name]
    if frames == nil or frames == false then return nil end
    return frames[math.floor(tick / THUMB_FRAMES) % #frames + 1]
end

-- The way in: a page in the menu's PLUGINS section, with the list and the settings. The grid is a
-- screen of its own -- the widget system is a vertical list with no grid, and a plugin-drawn screen is
-- always full-screen -- so the page keeps the quick list and offers the grid for browsing.
reskate.menu.page("Animations", function(ui)
    ui:heading("Custom animations")
    local names = reskate.animation.rska_list()
    ui:info("Clips", tostring(#names))
    ui:note("Press Ctrl+K for the grid: tiles with a stick figure of each pose.")
    ui:note("A screen draws under this menu, so it cannot be opened from here.")

    if pinned then
        local s = settings_of(pinned)
        ui:card("SETTINGS", pinned)
        local corrected = ui:toggle("ik", "Floor correction (ik)", s.floor > 0,
            "Lifts the clip's limbs clear of the floor the game planted.")
        s.floor = corrected and 1 or 0
        local tab = ui:tabs("mask", { "AUTO", "FULL", "LEGS" }, mask_index(s.mask))
        s.mask = masks[tab] or "auto"
        reskate.animation.floor(s.floor)
        reskate.animation.mask(s.mask)
        if ui:button("stop", "STOP", false) then
            reskate.animation.stop()
            pinned, previewing = nil, nil
        end
    end

    if #names == 0 then
        ui:note("No .rska files in CustomAnimations yet.")
        ui:note("Put them in the game folder's CustomAnimations folder.")
        return
    end

    ui:card("CLIPS", string.format("%d clip%s", #names, #names == 1 and "" or "s"))
    for _, name in ipairs(names) do
        local playing = (name == pinned)
        if ui:button("p" .. name, (playing and "> " or "   ") .. name, playing) then
            if play(name) then pinned, previewing = name, nil end
        end
    end
end)

reskate.ui.screen(screen_title, function() end, { panel = false })

-- The grid is opened with a key, not from the page: a plugin's screen draws under the game's menu, so
-- opening it from there leaves it behind the menu with no way to close the menu from a plugin.
reskate.input.bind("grid", "Ctrl+K")
reskate.on("action", function(name)
    if name ~= "grid" then return end
    if screen_open then
        reskate.ui.close()
        screen_open = false
        hovered, previewing, hover_frames = nil, nil, 0
    else
        reskate.ui.open(screen_title)
        screen_open = true
    end
end)

reskate.on("screen_closed", function(title)
    if title ~= screen_title then return end
    screen_open = false
    hovered, previewing, hover_frames = nil, nil, 0
end)

reskate.on("draw", function(d)
    if not screen_open then return end
    tick = tick + 1
    local display = d:size()          -- a vector: the window's width and height
    local w, h = display.x, display.y
    local names = reskate.animation.rska_list()
    local c = reskate.input.cursor()
    local mx = c and c.position and c.position.x or nil
    local my = c and c.position and c.position.y or nil
    local left = c and c.left or false

    d:rect(vector.create(0, 0, 0), vector.create(w, h, 0), { colour = C_BG, fill = true })
    d:text(vector.create(PAD, 30, 0), "CUSTOM ANIMATIONS", { colour = C_TEXT, size = 34, font = "title" })
    d:text(vector.create(PAD, 78, 0), pinned and ("Playing " .. pinned) or "Nothing playing",
           { colour = pinned and C_PIN or C_DIM, size = 20 })

    -- STOP, top right.
    local stop_x, stop_y, stop_w, stop_h = w - PAD - 120, 34, 120, 40
    local over_stop = mx and mx >= stop_x and mx <= stop_x + stop_w and my >= stop_y and my <= stop_y + stop_h
    d:rect(vector.create(stop_x, stop_y, 0), vector.create(stop_x + stop_w, stop_y + stop_h, 0),
           { colour = over_stop and C_HOT or C_DIM, thickness = 2, rounding = 6 })
    d:text(vector.create(stop_x + 40, stop_y + 9, 0), "STOP", { colour = over_stop and C_HOT or C_TEXT, size = 20 })

    if #names == 0 then
        d:text(vector.create(PAD, HEADER, 0), "No .rska files in CustomAnimations yet.", { colour = C_DIM, size = 22 })
        return
    end

    -- One clip read a frame for its thumbnail, then let go: the first pass fills the grid in a second
    -- or so and the tiles draw empty until their turn comes.
    for _, name in ipairs(names) do
        if thumbs[name] == nil then
            build_thumbnail(name)
            break
        end
    end

    local tile_w = (w - PAD * 2 - GAP * (COLS - 1)) / COLS
    local tile_h = tile_w * 0.78
    local cell_h = tile_h + 32
    local row_h = cell_h + GAP
    local rows = math.ceil(#names / COLS)
    local visible = math.max(1, math.floor((h - HEADER - PAD) / row_h))

    if c and c.wheel and c.wheel ~= 0 then
        scroll = math.max(0, math.min(math.max(0, rows - visible) * row_h, scroll - c.wheel * 60))
    end

    local hover_now = nil
    for i, name in ipairs(names) do
        local col = (i - 1) % COLS
        local row = math.floor((i - 1) / COLS)
        local x = PAD + col * (tile_w + GAP)
        local y = HEADER + row * row_h - scroll
        if y + cell_h >= HEADER and y <= h then
            local hot = mx and mx >= x and mx <= x + tile_w and my >= y and my <= y + tile_h
            if hot then hover_now = name end
            local active = (name == pinned)
            d:rect(vector.create(x, y, 0), vector.create(x + tile_w, y + tile_h, 0), {
                colour = active and C_PIN or (hot and C_HOT or C_TILE),
                fill = not (active or hot), thickness = 2, rounding = 6,
            })
            local lines = thumbnail(name, tick)
            if lines then
                local inner_w, inner_h = tile_w - 18, tile_h - 14
                for _, l in ipairs(lines) do
                    d:line(vector.create(x + 9 + l[1] * inner_w, y + 7 + l[2] * inner_h, 0),
                           vector.create(x + 9 + l[3] * inner_w, y + 7 + l[4] * inner_h, 0),
                           { colour = active and C_PIN or C_BONE, thickness = 2 })
                end
            end
            d:text(vector.create(x + 2, y + tile_h + 8, 0), name,
                   { colour = active and C_PIN or C_TEXT, size = 17 })
        end
    end

    -- Hover only highlights. Playing is on the click: a preview that took over the skater as the cursor
    -- passed was the wrong way round.
    hovered = hover_now

    -- Click pins. The cursor reports the button held, so this is the edge.
    if left and not was_left and mx and my then
        if over_stop then
            reskate.animation.stop()
            pinned, previewing = nil, nil
        elseif hovered then
            if play(hovered) then
                pinned, previewing = hovered, nil
            end
        end
    end
    was_left = left
end)
