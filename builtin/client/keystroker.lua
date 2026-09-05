local hudpos = {x = 0.925, y = 0.725}

-- ShowCPS: separate position table so it can be dragged independently of
-- KeyStroker (see cpspos usage in initialize_cps_hud()/update_cps_hud_position()
-- below).
local cpspos = {x = 0.925, y = 0.725}

-- Global size multiplier for MineBoost's custom HUD elements, combined
-- with KeyStroker's own independent multiplier -- see "hud_size" and
-- "keys_size" in src/gui/custom_menu/Menu.cpp ("HUD Size" slider and
-- scroll-to-resize in "Move HUD" edit mode, respectively).
local function clamp_size(v)
    if v < 0.5 then v = 0.5 end
    if v > 2.5 then v = 2.5 end
    return v
end

local function get_hud_size()
    local base = clamp_size(tonumber(minetest.settings:get("hud_size")) or 1.0)
    local own = clamp_size(tonumber(minetest.settings:get("keys_size")) or 1.0)
    return base * own
end

-- ShowCPS: split out of KeyStroker into its own independently
-- toggleable/movable HUD (see "show_cps"/"cps_x"/"cps_y"/"cps_size" and
-- the matching drag box in src/gui/custom_menu/Menu.cpp).
local function get_cps_hud_size()
    local base = clamp_size(tonumber(minetest.settings:get("hud_size")) or 1.0)
    local own = clamp_size(tonumber(minetest.settings:get("cps_size")) or 1.0)
    return base * own
end

local version = ""
local huds = {}
local keys = {"up", "left", "down", "right", "jump", "aux1", "sneak"}
local before = {}

-- Конфигурации для разных версий (baseline values, assume hud_size == 1.0;
-- see build_scaled_huddefs() below for how "hud_size" is applied)
local BASE_SCALE = 2
local huddefs_54_base = {
    up = {hud_elem_type = "image", position = hudpos,
        offset = {x = 0, y = 0}, text = "w_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    left = {hud_elem_type = "image", position = hudpos,
        offset = {x = -33, y = 33}, text = "a_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    down = {hud_elem_type = "image", position = hudpos,
        offset = {x = 0, y = 33}, text = "s_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    right = {hud_elem_type = "image", position = hudpos,
        offset = {x = 33, y = 33}, text = "d_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    jump = {hud_elem_type = "image", position = hudpos,
        offset = {x = -33, y = 99}, text = "space_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    place = {hud_elem_type = "image", position = hudpos,
        offset = {x = 17, y = 66}, text = "rmb_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    dig = {hud_elem_type = "image", position = hudpos,
        offset = {x = -33, y = 66}, text = "lmb_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    aux1 = {hud_elem_type = "image", position = hudpos,
        offset = {x = 33, y = 0}, text = "e_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    sneak = {hud_elem_type = "image", position = hudpos,
        offset = {x = -66, y = 33}, text = "shift_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
}

local huddefs_pre54_base = {
    up = {hud_elem_type = "image", position = hudpos,
        offset = {x = 0, y = 0}, text = "w_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    left = {hud_elem_type = "image", position = hudpos,
        offset = {x = -33, y = 33}, text = "a_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    down = {hud_elem_type = "image", position = hudpos,
        offset = {x = 0, y = 33}, text = "s_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    right = {hud_elem_type = "image", position = hudpos,
        offset = {x = 33, y = 33}, text = "d_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    jump = {hud_elem_type = "image", position = hudpos,
        offset = {x = -33, y = 99}, text = "space_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    RMB = {hud_elem_type = "image", position = hudpos,
        offset = {x = 17, y = 66}, text = "rmb_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    LMB = {hud_elem_type = "image", position = hudpos,
        offset = {x = -33, y = 66}, text = "lmb_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    aux1 = {hud_elem_type = "image", position = hudpos,
        offset = {x = 33, y = 0}, text = "e_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
    sneak = {hud_elem_type = "image", position = hudpos,
        offset = {x = -66, y = 33}, text = "shift_key.png", alignment = {x = 1, y = 1}, number = 0xFFFFFF},
}

-- Builds the actual (scaled) hud_add definitions for the given base table,
-- applying the current "hud_size" to both the icon scale and its offset
-- (so icons grow/shrink together without starting to overlap each other).
local function build_scaled_huddefs(base_defs, size)
    local scaled = {}
    for key, def in pairs(base_defs) do
        scaled[key] = {
            hud_elem_type = def.hud_elem_type,
            position = def.position,
            offset = {x = def.offset.x * size, y = def.offset.y * size},
            text = def.text,
            alignment = def.alignment,
            scale = {x = BASE_SCALE * size, y = BASE_SCALE * size},
            number = def.number,
        }
    end
    return scaled
end

local image_press_54 = {
    up = "w_key_press.png", left = "a_key_press.png", down = "s_key_press.png",
    right = "d_key_press.png", jump = "space_key_press.png", place = "rmb_key_press.png",
    dig = "lmb_key_press.png", aux1 = "e_key_press.png", sneak = "shift_key_press.png",
}
local image_press_pre54 = {
    up = "w_key_press.png", left = "a_key_press.png", down = "s_key_press.png",
    right = "d_key_press.png", jump = "space_key_press.png", RMB = "rmb_key_press.png",
    LMB = "lmb_key_press.png", aux1 = "e_key_press.png", sneak = "shift_key_press.png",
}

local image_normal_54 = {
    up = "w_key.png", left = "a_key.png", down = "s_key.png", right = "d_key.png",
    jump = "space_key.png", place = "rmb_key.png", dig = "lmb_key.png",
    aux1 = "e_key.png", sneak = "shift_key.png",
}
local image_normal_pre54 = {
    up = "w_key.png", left = "a_key.png", down = "s_key.png", right = "d_key.png",
    jump = "space_key.png", RMB = "rmb_key.png", LMB = "lmb_key.png",
    aux1 = "e_key.png", sneak = "shift_key.png",
}

local huddefs_base, image_press, image_normal
local last_hud_size = nil
local rmbcps, lmbcps
local last_cps_hud_size = nil
local rmbclicks, lmbclicks = 0, 0
local rmbpress, lmbpress = false, false
local rmbtimer, lmbtimer = 0, 0
local last_rmb_text, last_lmb_text = nil, nil
local huds_initialized = false
local cps_huds_initialized = false
local last_keys_base_pos = nil
local last_cps_base_pos = nil

local function positions_equal(a, b)
    return a and b and a.x == b.x and a.y == b.y
end

-- KEYS_BG_OFFSET/CPS_BG_OFFSET describe where the background panel
-- Hud::drawKeyStrokerCpsBackgrounds() draws in src/client/hud.cpp sits
-- relative to the shared position anchor used for the KeyStroker icon
-- cluster / ShowCPS text below (see the comment on base_pos in
-- update_hud_positions()/update_cps_hud_position()). BG_BASE_SCALE(2) *
-- native texture size (80x80 / 90x27, see textures/base/pack/
-- keys_panel_bg.png, cps_panel_bg.png -- unused as HUD images now, kept
-- only as the source of these numbers) is that panel's on-screen size:
-- 160x160 / 180x54 at hud_size 1.0, matching the literal constants in
-- Hud::drawKeyStrokerCpsBackgrounds() and the C++ drag-preview box in
-- Menu.cpp exactly.
--
-- KEYS_BG_OFFSET/KEYS_BG_NATIVE_SIZE were derived from the actual
-- KeyStroker icon layout in huddefs_54_base/huddefs_pre54_base above
-- (bounding box of all 9 icons at hud_size=1 is roughly x:[-66,65],
-- y:[0,131] -- see Hud::drawLuaElements()'s HUD_ELEM_IMAGE case in
-- src/client/hud.cpp for the align/offset->pixel-rect formula this is
-- based on), padded out by ~14px on each side.
local KEYS_BG_OFFSET = {x = -80, y = -14}
local CPS_BG_OFFSET = {x = -90, y = -8}

-- Определение версии
local function get_version()
    local player = minetest.localplayer
    if player and player:get_control().place == nil then
        version = "pre5.4"
        table.insert(keys, "RMB")
        table.insert(keys, "LMB")
        huddefs_base = huddefs_pre54_base
        image_press = image_press_pre54
        image_normal = image_normal_pre54
    else
        version = "5.4"
        table.insert(keys, "place")
        table.insert(keys, "dig")
        huddefs_base = huddefs_54_base
        image_press = image_press_54
        image_normal = image_normal_54
    end
end

-- Lightweight version detection for ShowCPS: it needs to know whether
-- "RMB"/"LMB" or "place"/"dig" are the right control names, but (unlike
-- get_version() above) must NOT touch the shared "keys" table -- get_version()
-- appends to it unconditionally, so calling it from both KeyStroker's and
-- ShowCPS's independent lifecycles would duplicate entries every time either
-- one is toggled off/on. If KeyStroker has already run, "version" is already
-- set and this is a no-op.
local function ensure_version()
    if version ~= "" then return end
    local player = minetest.localplayer
    if not player then return end
    version = (player:get_control().place == nil) and "pre5.4" or "5.4"
end

local function initialize_huds()
    if not minetest.localplayer then return end

    get_version()
    last_hud_size = get_hud_size()
    local huddefs = build_scaled_huddefs(huddefs_base, last_hud_size)

    for _, key in ipairs(keys) do
        if not huds[key] then
            huds[key] = minetest.localplayer:hud_add(huddefs[key])
        end
    end

    huds_initialized = true
end

-- ShowCPS: independent HUD lifecycle, controlled by "show_cps" instead of
-- "show_keys" -- see initialize_huds()/remove_huds() above for the
-- (now-separate) KeyStroker key-icon lifecycle.
local function initialize_cps_hud()
    if not minetest.localplayer then return end
    ensure_version()

    last_cps_hud_size = get_cps_hud_size()

    if not lmbcps then
        lmbcps = minetest.localplayer:hud_add({
            hud_elem_type = "text",
            position = cpspos,
            -- First line of the column.
            offset = {x = 0, y = 0},
            text = "LMB CPS: 0",
            alignment = {x = 0, y = 1},
            size = {x = last_cps_hud_size, y = last_cps_hud_size},
            number = 0xFFFFFF,
        })
    end

    if not rmbcps then
        rmbcps = minetest.localplayer:hud_add({
            hud_elem_type = "text",
            position = cpspos,
            -- Directly below LMB CPS (same column, next line).
            offset = {x = 0, y = 18},
            text = "RMB CPS: 0",
            alignment = {x = 0, y = 1},
            size = {x = last_cps_hud_size, y = last_cps_hud_size},
            number = 0xFFFFFF,
        })
    end

    cps_huds_initialized = true
end

local function remove_cps_hud()
    if not minetest.localplayer then return end

    if lmbcps then
        minetest.localplayer:hud_remove(lmbcps)
        lmbcps = nil
    end
    if rmbcps then
        minetest.localplayer:hud_remove(rmbcps)
        rmbcps = nil
    end

    cps_huds_initialized = false
    last_cps_base_pos = nil
    last_rmb_text = nil
    last_lmb_text = nil
end

local function remove_huds()
    if not minetest.localplayer then return end

    for key, hud_id in pairs(huds) do
        minetest.localplayer:hud_remove(hud_id)
        huds[key] = nil
    end

    before = {}
    huds_initialized = false
    last_keys_base_pos = nil
end

local function update_hud_positions()
    if not minetest.localplayer then return end

    -- Must match the C++ drag-preview box in Menu.cpp, which positions
    -- itself using the actual current viewport size (driver->getScreenSize()),
    -- not the "screen_width"/"screen_height" settings (those are just the
    -- configured fullscreen resolution and can be very different from the
    -- real window size, e.g. when playing windowed or at a small
    -- resolution) -- using the wrong size here is what made the keys HUD
    -- end up somewhere other than where it was dragged to.
    local screen = minetest.get_screen_size()
    local screenW = screen.x
    local screenH = screen.y

    local keys_x_n = minetest.settings:get("keys_x") or 0.925
    local keys_y_n = minetest.settings:get("keys_y") or 0.725

    -- Every KeyStroker icon is hud_add'ed with alignment {1,1}, which the
    -- engine renders as: the element's own top-left corner =
    -- position*screen + element's own "offset" (see the HUD_ELEM_IMAGE
    -- case in Hud::drawLuaElements(), src/client/hud.cpp -- with
    -- alignment 1 the align-based offset term is exactly zero, so it's
    -- purely position + offset). This shared anchor is shifted by
    -- -KEYS_BG_OFFSET*size so the icon cluster lines up with the
    -- background panel Hud::drawKeyStrokerCpsBackgrounds() draws in
    -- src/client/hud.cpp, whose own top-left sits at the raw (keys_x,
    -- keys_y) pixel with no such offset -- same reasoning the C++
    -- drag-preview box in Menu.cpp uses too. (Shifting the shared anchor
    -- moves the icons right along with the panel, as a rigid group --
    -- each icon's own per-icon offset from huddefs_54_base is what keeps
    -- it positioned relative to the panel, completely unaffected by this
    -- shift.)
    local size = get_hud_size()
    local base_pos = {
        x = (keys_x_n - KEYS_BG_OFFSET.x * size) / screenW,
        y = (keys_y_n - KEYS_BG_OFFSET.y * size) / screenH,
    }

    if not positions_equal(base_pos, last_keys_base_pos) then
        last_keys_base_pos = base_pos
        for _, key in ipairs(keys) do
            if huds[key] then
                minetest.localplayer:hud_change(huds[key], "position", base_pos)
            end
        end
    end

    -- Live-apply the "HUD Size" slider without needing to toggle KeyStroker
    -- off/on again. (Reuses `size` computed above for the anchor shift --
    -- same value, since nothing in this function changes settings mid-call.)
    if huds_initialized and size ~= last_hud_size then
        last_hud_size = size
        local scaled = build_scaled_huddefs(huddefs_base, size)
        for _, key in ipairs(keys) do
            if huds[key] and scaled[key] then
                minetest.localplayer:hud_change(huds[key], "scale", scaled[key].scale)
                minetest.localplayer:hud_change(huds[key], "offset", scaled[key].offset)
            end
        end
    end
end
-- "cps_x"/"cps_y" (its own drag box in Menu.cpp) instead of "keys_x"/"keys_y",
-- so it can be moved independently.
local function update_cps_hud_position()
    if not minetest.localplayer then return end

    local screen = minetest.get_screen_size()
    local screenW = screen.x
    local screenH = screen.y

    local cps_x_n = minetest.settings:get("cps_x") or 0
    local cps_y_n = minetest.settings:get("cps_y") or 160

    -- Same reasoning as update_hud_positions() above: the background
    -- panel Hud::drawKeyStrokerCpsBackgrounds() draws in src/client/hud.cpp
    -- sits with its own top-left at the raw (cps_x, cps_y) pixel, so the
    -- shared icon/text anchor here is shifted by -CPS_BG_OFFSET*size to
    -- line up with it, matching the C++ drag-preview box in Menu.cpp.
    -- (lmbcps/rmbcps ride along with the same shift, but each keeps its
    -- own offset={0,0}/{0,18} from initialize_cps_hud() below relative to
    -- the panel, so their position relative to the panel itself is
    -- unaffected.)
    local size = get_cps_hud_size()
    local base_pos = {
        x = (cps_x_n - CPS_BG_OFFSET.x * size) / screenW,
        y = (cps_y_n - CPS_BG_OFFSET.y * size) / screenH,
    }

    if not positions_equal(base_pos, last_cps_base_pos) then
        last_cps_base_pos = base_pos
        if lmbcps then
            minetest.localplayer:hud_change(lmbcps, "position", base_pos)
        end
        if rmbcps then
            minetest.localplayer:hud_change(rmbcps, "position", base_pos)
        end
    end

    -- Live-apply the "HUD Size" slider (and "cps_size") without needing to
    -- toggle ShowCPS off/on again. (Reuses `size` computed above.)
    if cps_huds_initialized and size ~= last_cps_hud_size then
        last_cps_hud_size = size
        if lmbcps then
            minetest.localplayer:hud_change(lmbcps, "size", {x = size, y = size})
        end
        if rmbcps then
            minetest.localplayer:hud_change(rmbcps, "size", {x = size, y = size})
        end
    end
end

local function track_rmb_clicks(dtime)
    if not minetest.localplayer then return end
    ensure_version()

    local ctl = minetest.localplayer:get_control()
    local current_rmb = (version == "pre5.4") and ctl.RMB or ctl.place

    if current_rmb and not rmbpress then
        rmbclicks = rmbclicks + 1
    end
    rmbpress = current_rmb

    if rmbcps then
        local text = "RMB CPS: " .. rmbclicks
        if text ~= last_rmb_text then
            last_rmb_text = text
            minetest.localplayer:hud_change(rmbcps, "text", text)
        end
    end

    rmbtimer = rmbtimer + dtime
    if rmbtimer >= 1 then
        rmbclicks = 0
        rmbtimer = 0
    end
end

local function track_lmb_clicks(dtime)
    if not minetest.localplayer then return end
    ensure_version()

    local ctl = minetest.localplayer:get_control()
    local current_lmb = (version == "pre5.4") and ctl.LMB or ctl.dig

    if current_lmb and not lmbpress then
        lmbclicks = lmbclicks + 1
    end
    lmbpress = current_lmb

    if lmbcps then
        local text = "LMB CPS: " .. lmbclicks
        if text ~= last_lmb_text then
            last_lmb_text = text
            minetest.localplayer:hud_change(lmbcps, "text", text)
        end
    end

    lmbtimer = lmbtimer + dtime
    if lmbtimer >= 1 then
        lmbclicks = 0
        lmbtimer = 0
    end
end

local function update_key_states()
    if not minetest.localplayer then return end

    if minetest.settings:get_bool("show_keys") then
        local player = minetest.localplayer
        local controls = player:get_control()

        if not huds_initialized then
            initialize_huds()
        end

        for _, key in ipairs(keys) do
            if controls[key] and not before[key] then
                if huds[key] then
                    player:hud_change(huds[key], "text", image_press[key])
                end
                before[key] = true
            elseif not controls[key] and before[key] then
                if huds[key] then
                    player:hud_change(huds[key], "text", image_normal[key])
                end
                before[key] = false
            end
        end
    else
        if huds_initialized then
            remove_huds()
        end
    end
end

-- MineBoost: KeyStroker/ShowCPS's own HUD elements (the key icons and
-- CPS counters this file creates via minetest.localplayer:hud_add()) are
-- disabled here -- both are now drawn by MineBoostV2's ImGui-based HUD
-- instead (see Hud::drawKeyStrokerHud()/drawCpsHud() in
-- src/client/hud.cpp and src/gui/imgui_hud.h/.cpp), which replicates the
-- same "up/left/down/right/jump/aux1/sneak/dig/place" key-state reading
-- and LMB/RMB click-per-second counting natively in C++ (via
-- LocalPlayer::getPlayerControl(), no Lua involvement) rather than
-- needing anything from here. Left both blocks below fully intact,
-- wrapped in "if ENABLE_LUA_HUD then" (Lua's equivalent of a C++ "#if 0")
-- rather than deleted, exactly like MineBoostV2's ClientChat is currently
-- disabled on the C++ side (see the comment on that in
-- src/client/game.cpp) -- restore by changing "ENABLE_LUA_HUD = false" to
-- "ENABLE_LUA_HUD = true" below, and reverting whatever disabled
-- the equivalent native drawing (see the two functions named above).
-- Kept for future re-enabling, see comment above -- deliberately using a
-- named constant here rather than a literal "false" in the "if"s below.
-- luacheck constant-folds literal "if false then"/"if true then" and
-- then reports the whole block as unreachable code, which in turn makes
-- every function only called from inside it show up as "unused" too
-- (9 false-positive warnings that failed CI). A named local is opaque
-- to that analysis, so the intentionally-dead code stays lint-clean.
local ENABLE_LUA_HUD = false

if ENABLE_LUA_HUD then

-- Инициализация
minetest.after(0, function()
    if not minetest.localplayer then
        minetest.after(0, initialize_huds)
        return
    end
    if minetest.settings:get_bool("show_keys") then
        initialize_huds()
    end
    if minetest.settings:get_bool("show_cps") then
        initialize_cps_hud()
    end
end)

end -- if ENABLE_LUA_HUD (init block)

if ENABLE_LUA_HUD then

minetest.register_globalstep(function(dtime)
    if not minetest.localplayer then return end

    local show_keys = minetest.settings:get_bool("show_keys")
    local show_cps = minetest.settings:get_bool("show_cps")

    if show_keys then
        -- update_key_states() lazily calls initialize_huds() the first
        -- time show_keys turns on -- that has to happen before
        -- update_hud_positions() below, or the correction it applies has
        -- no elements to apply to yet (they're still nil at that point),
        -- silently does nothing, but still records the position as
        -- already-applied. Every following tick then sees "nothing
        -- changed" and never retries -- meanwhile initialize_huds() goes
        -- on to actually create everything one line later, at its
        -- hardcoded hudpos fallback, which then never gets corrected:
        -- the real KeyStroker HUD ends up stuck wherever hudpos points
        -- forever, regardless of "keys_x"/"keys_y" or where the
        -- Menu.cpp drag-preview box says it should be.
        update_key_states()
        update_hud_positions()
    else
        if huds_initialized then
            remove_huds()
        end
    end

    if show_cps then
        if not cps_huds_initialized then
            initialize_cps_hud()
        end
        update_cps_hud_position()
        track_rmb_clicks(dtime)
        track_lmb_clicks(dtime)
    else
        if cps_huds_initialized then
            remove_cps_hud()
        end
    end
end)

end -- if ENABLE_LUA_HUD (globalstep block)

--[[
    GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    Made by Minetest-j45 -> https://github.com/Minetest-j45
]]--