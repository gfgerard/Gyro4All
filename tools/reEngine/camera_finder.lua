-- Gyro4All camera finder for RE Engine games (REFramework autorun script; research tool, never shipped).
-- See tools/reEngine/README.md for how to install and use it.
--
-- Finds the rotation Gyro4All must turn for a game's player camera, like the Unreal module's camera probe:
--   1. Scan: walks the game's objects from REFramework's list of managed singletons and from the camera's own
--      GameObject (fields, arrays, lists, a GameObject's components, a camera or player component's GameObject) and
--      keeps every rotation: via.Quaternion fields, float fields named like yaw/pitch/angle, and both components of
--      every via.vec2 field (a yaw/pitch pair kept as two angles).
--   2. Captures: you turn the camera with the right stick and stop, 3 times. A rotation is kept only if it turned
--      by the same amount as the real camera (the primary camera's transform).
--   3. Live test: each remaining rotation is turned by 5 degrees just before LateUpdateBehavior (where Gyro4All
--      writes) and undone; the real camera must follow it. A pitch test gives the sign Gyro4All needs (PitchSign).
-- The result is shown in REFramework's menu, written to REFramework's log (prefix [Gyro4All camera]) and to
-- reframework\data\gyro4all_camera.txt.

---------------------------------------------------------------------------------------------------------- settings

local MAX_OBJECTS = 60000         -- objects visited by the scan
local OBJECTS_PER_FRAME = 1500    -- scan work per frame (the game keeps running)
local DEPTH_CAMERA_ROOTS = 9      -- path length from roots whose type names a camera (RE Requiem's chain is 8 long)
local DEPTH_OTHER_ROOTS = 6       -- from other singletons (RE7: ObjectManager -> PlayerObj -> components -> ...)
local ARRAY_ITEMS = 32            -- elements followed per array
local MIN_TURN = 25               -- degrees the camera must turn for a capture
local STILL_FRAMES = 30           -- frames without camera motion before a baseline or a capture
local CAPTURES = 3
local TEST_DEGREES = 5            -- live test turn
local TEST_FRAMES = 12            -- frames to wait after a live write
local FLOAT_NAME = { "yaw", "pitch", "angle", "rot" }

---------------------------------------------------------------------------------------------------------- state

local state = "idle"     -- idle, scanning, settling, ready, turning, testing, done, failed
local message = "Press 'Find the camera' in gameplay, camera free (not aiming, no cutscene)."
local candidates = {}    -- { path, owner, field, kind ("quat"/"float"), comp (via.vec2: "x"/"y"), base, unit, axis, ... }
local results = {}       -- confirmed candidates with their test outcome
local captures = 0
local log_lines = {}

local function note(s)
    table.insert(log_lines, 1, s)
    if #log_lines > 14 then table.remove(log_lines) end
    log.info("[Gyro4All camera] " .. s)
end

---------------------------------------------------------------------------------------------------------- math
-- Same conventions as Gyro4All (src/engines/reEngine/re_camera.cpp): quaternion forward +Z, up +Y; read_rotation's
-- yaw is positive to the left and pitch positive up for a +Z-forward rotation.

local function qmul(a, b)
    return {
        x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    }
end

local function axis_angle(x, y, z, deg)
    local h = math.rad(deg) / 2
    local s = math.sin(h)
    return { x = x * s, y = y * s, z = z * s, w = math.cos(h) }
end

local function qcopy(q) return { x = q.x, y = q.y, z = q.z, w = q.w } end

local function qlen(q) return math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) end

-- yaw, pitch in degrees; forward_sign 1 = +Z forward (Gyro4All's read_rotation), -1 = -Z forward (a camera).
local function yaw_pitch(q, forward_sign)
    local x, y, z, w = q.x, q.y, q.z, q.w
    local fx, fy, fz = 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)
    fx, fy, fz = fx * forward_sign, fy * forward_sign, fz * forward_sign
    return math.deg(math.atan(fx, fz)), math.deg(math.asin(math.max(-1, math.min(1, fy))))
end

local function wrap(d)
    while d > 180 do d = d - 360 end
    while d < -180 do d = d + 360 end
    return d
end

-- Gyro4All's turn_rotation without the pitch clamp: yaw positive = right, pitch positive = up for pitch_sign 1.
local function turn_quat(q, yaw, pitch, pitch_sign)
    local r = qmul(qmul(axis_angle(0, 1, 0, -yaw), q), axis_angle(1, 0, 0, -pitch_sign * pitch))
    local n = qlen(r)
    return { x = r.x / n, y = r.y / n, z = r.z / n, w = r.w / n }
end

---------------------------------------------------------------------------------------------------------- game access

local function try(f, ...)
    local ok, v = pcall(f, ...)
    if ok then return v end
    return nil
end

-- The real camera's world rotation (yaw, pitch; a camera looks down -Z), or nil.
local function camera_angles()
    local cam = try(sdk.get_primary_camera)
    local go = cam and try(cam.call, cam, "get_GameObject")
    local tr = go and try(go.call, go, "get_Transform")
    local q = tr and try(tr.call, tr, "get_Rotation")
    if not q then return nil end
    return yaw_pitch(q, -1)
end

local function read_value(c)
    local v = try(c.owner.get_field, c.owner, c.field)
    if v == nil then return nil end
    if c.comp then
        local n = try(function() return v[c.comp] end)
        return type(n) == "number" and n or nil
    end
    if c.kind == "quat" then
        if not v.x then return nil end
        local q = qcopy(v)
        local n = qlen(q)
        if n < 0.9 or n > 1.1 then return nil end
        return q
    end
    return type(v) == "number" and v or nil
end

local function write_value(c, value)
    if c.kind == "quat" then
        local q = try(c.owner.get_field, c.owner, c.field)
        if not q then return false end
        q.x, q.y, q.z, q.w = value.x, value.y, value.z, value.w
        return try(c.owner.set_field, c.owner, c.field, q) ~= false
    end
    if c.comp then
        local v = try(c.owner.get_field, c.owner, c.field)
        if not v or not pcall(function() v[c.comp] = value end) then return false end
        return try(c.owner.set_field, c.owner, c.field, v) ~= false
    end
    return try(c.owner.set_field, c.owner, c.field, value) ~= false
end

---------------------------------------------------------------------------------------------------------- scan

local type_cache = {}   -- type name -> { fields = {{name, kind}}, component = bool, follow = bool }

local function name_matches(name, words)
    local n = name:lower()
    for _, w in ipairs(words) do
        if n:find(w, 1, true) then return true end
    end
    return false
end

local function type_info(td)
    local full = td:get_full_name()
    local info = type_cache[full]
    if info then return info end
    info = { name = full, fields = {} }
    info.component = try(td.is_a, td, "via.Component") == true
    -- Managed objects worth following: the game's own types, arrays and lists. Not engine natives (via.*, except
    -- GameObject, handled below) or System types.
    info.follow = not (full:find("^via%.") or full:find("^System%.")) or full:find("%[%]$") ~= nil
        or full:find("^System%.Collections%.Generic%.List") ~= nil
    info.game_object = full == "via.GameObject"
    info.camera_ish = name_matches(full, { "camera", "player" })
    local t = td
    while t do
        for _, f in ipairs(try(t.get_fields, t) or {}) do
            if not f:is_static() then
                local ft = f:get_type()
                local tname = ft and ft:get_full_name() or ""
                local fname = f:get_name()
                if tname == "via.Quaternion" then
                    table.insert(info.fields, { fname, "quat" })
                elseif tname == "System.Single" and name_matches(fname, FLOAT_NAME) then
                    table.insert(info.fields, { fname, "float" })
                elseif tname == "via.vec2" or tname == "via.Float2" then
                    table.insert(info.fields, { fname, "vec2" })
                elseif ft and not try(ft.is_value_type, ft) then
                    table.insert(info.fields, { fname, "obj" })
                end
            end
        end
        t = t:get_parent_type()
    end
    type_cache[full] = info
    return info
end

-- Components that hold a player camera's rotation in known games, asked for by type on every GameObject the scan
-- reaches (RE7/RE8: app.PlayerCamera on the player's GameObject).
local KNOWN_COMPONENTS = { "app.PlayerCamera", "app.PlayerCameraTPS", "app.VillageCamera", "app.ropeway.camera.CameraSystem",
    "chainsaw.CameraSystem" }

local scan = nil   -- { queue, head, tail, seen, visited }; tail is kept by hand: # on a queue with nil holes is undefined

local function push(obj, path, depth, max_depth)
    if not obj or depth > max_depth then return end
    local addr = try(obj.get_address, obj)
    if not addr or addr == 0 or scan.seen[addr] then return end
    scan.seen[addr] = true
    scan.tail = scan.tail + 1
    scan.queue[scan.tail] = { obj = obj, path = path, depth = depth, max = max_depth }
end

local function as_object(addr)
    if not addr or addr == 0 then return nil end
    return try(sdk.to_managed_object, addr)
end

local function visit(node)
    local obj = node.obj
    local td = try(obj.get_type_definition, obj)
    if not td then return end
    local info = type_info(td)
    local d, max = node.depth, node.max
    scan.types[info.name] = (scan.types[info.name] or 0) + 1

    -- A GameObject: its components, chained from its transform (+0x18, next component +0x18; as in re_camera.cpp).
    if info.game_object then
        local first = try(obj.read_qword, obj, 0x18)
        local addr, count, stop = first, 0, "end of chain"
        for _ = 1, 256 do
            local comp = as_object(addr)
            if not comp then
                stop = string.format("component 0x%x not a managed object", addr or 0)
                break
            end
            local ctd = try(comp.get_type_definition, comp)
            push(comp, node.path .. "{" .. (ctd and ctd:get_full_name() or "?") .. "}", d + 1, max)
            count = count + 1
            addr = try(comp.read_qword, comp, 0x18)
            if not addr or addr == 0 or addr == first then break end
        end
        -- Known camera components asked for by type too, in case the chain stopped early.
        local found = {}
        for _, name in ipairs(KNOWN_COMPONENTS) do
            local t = try(sdk.typeof, name)
            local comp = t and try(obj.call, obj, "getComponent(System.Type)", t)
            if comp then
                found[#found + 1] = name
                push(comp, node.path .. "{" .. name .. "}", d + 1, max)
            end
        end
        scan.game_objects = (scan.game_objects or 0) + 1
        if scan.game_objects > 30 and #found == 0 then return end  -- keep the log short
        local go_name = try(obj.call, obj, "get_Name") or "?"
        note(string.format("GameObject %s at %s: %d components chained (%s)%s", tostring(go_name), node.path, count,
            stop, #found > 0 and (", by type: " .. table.concat(found, ", ")) or ""))
        return
    end
    -- An array: its elements.
    if info.name:find("%[%]$") then
        local n = try(obj.get_size, obj) or 0
        for i = 0, math.min(n, ARRAY_ITEMS) - 1 do
            local e = try(obj.get_element, obj, i)
            if e and type(e) == "userdata" and try(sdk.is_managed_object, e) then
                push(e, node.path .. "[" .. i .. "]", d + 1, max)
            end
        end
        return
    end
    if not info.follow and not info.component then return end
    for _, f in ipairs(info.fields) do
        local name, kind = f[1], f[2]
        if kind == "obj" then
            local v = try(obj.get_field, obj, name)
            if v and type(v) == "userdata" and try(sdk.is_managed_object, v) then
                push(v, node.path .. "." .. name, d + 1, max)
            end
        elseif kind == "vec2" then
            -- Each component is an angle candidate of its own (often x = pitch, y = yaw).
            for _, comp in ipairs({ "x", "y" }) do
                local c = { path = node.path .. "." .. name .. "." .. comp, owner = obj, field = name, kind = "float",
                    comp = comp, owner_type = info.name }
                if read_value(c) ~= nil then table.insert(candidates, c) end
            end
        else
            local c = { path = node.path .. "." .. name, owner = obj, field = name, kind = kind, owner_type = info.name }
            if read_value(c) ~= nil then table.insert(candidates, c) end
        end
    end
    -- A camera or player component: its GameObject too (the camera rotation may sit in a sibling component).
    if info.component and info.camera_ish then
        push(as_object(try(obj.read_qword, obj, 0x10)), node.path .. ".<GameObject>", d + 1, max)
    end
end

-- Singletons known to lead to the player camera, tried when REFramework's list comes back empty (RE8: app.PropsManager
-- -> <Player>k__BackingField -> the player's GameObject -> its app.PlayerCamera component).
local KNOWN_SINGLETONS = { "app.PropsManager", "app.ObjectManager", "app.CameraSystem", "app.PlayerManager",
    "app.CharacterManager", "app.GameObjectManager", "app.CameraManager", "app.ropeway.camera.CameraSystem",
    "chainsaw.CameraSystem", "chainsaw.CharacterManager", "offline.camera.CameraSystem" }

-- Managed singletons as { instance = obj }. REFramework's list first; if it is empty or fails (it was empty in RE8
-- with REFramework 1.5.9.1+514), every top-level game type whose name sounds like a singleton is asked for one by name.
local function managed_singletons()
    local ok, list = pcall(sdk.get_managed_singletons)
    local out = {}
    if ok and type(list) == "table" then
        for _, s in ipairs(list) do
            local inst = type(s) == "table" and s.instance or s
            if inst then table.insert(out, { instance = inst }) end
        end
    end
    note(string.format("sdk.get_managed_singletons: %s, %d singletons", ok and type(list) or ("error: " .. tostring(list)),
        #out))
    if #out > 0 then return out end
    local names, seen = {}, {}
    for _, n in ipairs(KNOWN_SINGLETONS) do names[#names + 1] = n end
    local tdb = try(sdk.get_tdb)
    local count = tdb and try(tdb.get_num_types, tdb) or 0
    for i = 0, count - 1 do
        local t = try(tdb.get_type, tdb, i)
        local name = t and try(t.get_full_name, t)
        -- Top-level types (no nested or generic ones) named like a manager or a camera system.
        if name and not name:find("[<`%[]") and name:find("^[%w_]+%.[%w_]+$")
            and (name:find("Manager$") or name:find("System$") or name:find("Master$") or name:find("Director$")
                or name:find("Camera")) then
            names[#names + 1] = name
        end
    end
    for _, n in ipairs(names) do
        local inst = not seen[n] and try(sdk.get_managed_singleton, n)
        seen[n] = true
        if inst then
            table.insert(out, { instance = inst })
            note("singleton found: " .. n)
        end
    end
    note(string.format("singletons by name: %d found among %d names (%d types in the TDB)", #out, #names, count))
    return out
end

local function start_scan()
    candidates, results, captures = {}, {}, 0
    scan = { queue = {}, head = 1, tail = 0, seen = {}, visited = 0, types = {} }
    -- Roots: the camera's own GameObject, then singletons whose type names a camera, then the others.
    local cam = try(sdk.get_primary_camera)
    local go = cam and try(cam.call, cam, "get_GameObject")
    if go then push(go, "primary camera <GameObject>", 0, DEPTH_CAMERA_ROOTS) end
    local singletons = managed_singletons()
    local others = {}
    for _, s in ipairs(singletons) do
        local inst = s.instance
        local td = inst and try(inst.get_type_definition, inst)
        if td then
            local name = td:get_full_name()
            if name_matches(name, { "camera" }) then
                push(inst, "singleton " .. name, 0, DEPTH_CAMERA_ROOTS)
            else
                table.insert(others, { inst, name })
            end
        end
    end
    -- The other singletons only after the camera roots' walk: a player manager alone can fill MAX_OBJECTS (MH Wilds).
    scan.later = others
    note(string.format("scan started: %d singletons, primary camera %s", #singletons, go and "found" or "NOT found"))
    state = "scanning"
end

local function scan_step()
    local budget = OBJECTS_PER_FRAME
    while budget > 0 and scan.head <= scan.tail and scan.visited < MAX_OBJECTS do
        local node = scan.queue[scan.head]
        scan.queue[scan.head] = nil
        scan.head = scan.head + 1
        scan.visited = scan.visited + 1
        pcall(visit, node)
        budget = budget - 1
    end
    message = string.format("Scanning... %d objects, %d rotations found", scan.visited, #candidates)
    if scan.head > scan.tail and scan.later and scan.visited < MAX_OBJECTS then
        note(string.format("camera roots done: %d objects visited, %d rotations; now the other singletons",
            scan.visited, #candidates))
        for _, o in ipairs(scan.later) do push(o[1], "singleton " .. o[2], 0, DEPTH_OTHER_ROOTS) end
        scan.later = nil
    end
    if scan.head > scan.tail or scan.visited >= MAX_OBJECTS then
        note(string.format("scan done: %d objects visited%s, %d rotations", scan.visited,
            scan.visited >= MAX_OBJECTS and " (limit reached)" or "", #candidates))
        if #candidates == 0 then
            -- Where the walk went, to see why it found nothing.
            local list = {}
            for name, n in pairs(scan.types) do list[#list + 1] = { name, n } end
            table.sort(list, function(x, y) return x[2] > y[2] end)
            local parts = {}
            for i = 1, math.min(#list, 40) do parts[i] = list[i][1] .. " x" .. list[i][2] end
            note("types visited: " .. table.concat(parts, ", "))
        end
        scan = nil
        if #candidates == 0 then
            state, message = "failed", "No rotation found. Are you in gameplay?"
            return
        end
        state = "settling"
    end
end

---------------------------------------------------------------------------------------------------------- captures

local still, last_yaw, last_pitch = 0, nil, nil
local base_yaw, base_pitch = nil, nil

local function take_baseline(yaw, pitch)
    base_yaw, base_pitch = yaw, pitch
    local kept = {}
    for _, c in ipairs(candidates) do
        c.base = read_value(c)
        if c.base ~= nil then table.insert(kept, c) end
    end
    candidates = kept
end

-- Keeps the candidates that turned like the camera since the baseline.
local function capture(yaw, pitch)
    local dy, dp = wrap(yaw - base_yaw), pitch - base_pitch
    local kept = {}
    for _, c in ipairs(candidates) do
        local v = read_value(c)
        if v ~= nil then
            local keep = false
            if c.kind == "quat" then
                local cy = yaw_pitch(c.base, 1)
                local ny = yaw_pitch(v, 1)
                keep = math.abs(wrap(ny - cy) - dy) <= math.max(2, 0.08 * math.abs(dy))
            else
                -- A float angle: degrees or radians, either sign, following the yaw or the pitch.
                local d = v - c.base
                for _, unit in ipairs({ 1, math.pi / 180 }) do
                    for _, axis in ipairs({ { "yaw", dy }, { "pitch", dp } }) do
                        local want = axis[2] * unit
                        if math.abs(axis[2]) >= 5 and math.abs(math.abs(d) - math.abs(want)) <= 0.1 * math.abs(want)
                            and (not c.axis or c.axis == axis[1]) then
                            keep, c.unit, c.axis = true, unit, axis[1]
                            c.sign = (d * want >= 0) and 1 or -1
                        end
                    end
                end
            end
            if keep then table.insert(kept, c) end
        end
    end
    captures = captures + 1
    note(string.format("capture %d: camera yaw %+.1f pitch %+.1f; %d of %d rotations followed it", captures, dy, dp,
        #kept, #candidates))
    candidates = kept
end

---------------------------------------------------------------------------------------------------------- live test

local test = nil   -- { index, phase, frames, original, yaw0, pitch0, kind, ... }
local pending_write = nil

local function next_test(start_index)
    local c = candidates[start_index]
    test = { index = start_index, phase = (c and c.kind == "float") and "float" or "yaw", frames = 0 }
end

-- Writes go through the frame step Gyro4All uses (before LateUpdateBehavior).
local function apply_pending()
    if pending_write then
        write_value(pending_write[1], pending_write[2])
        pending_write = nil
    end
end

local hooked = pcall(re.on_pre_application_entry, "LateUpdateBehavior", apply_pending)

local function request_write(c, value)
    if hooked then pending_write = { c, value } else write_value(c, value) end
end


local function test_step(yaw, pitch)
    local c = candidates[test.index]
    if not c then
        state = "done"
        local lines = { "Gyro4All camera finder results" }
        if #results == 0 then
            table.insert(lines, "No rotation turned the camera.")
        end
        for _, c in ipairs(candidates) do
            if not c.yaw_moved and not c.float_moved then
                table.insert(lines, string.format("followed the captures, but a %d-degree write moved the camera %.1f: %s  (%s, field of %s)",
                    TEST_DEGREES, c.moved or 0, c.path, c.kind, c.owner_type))
            end
        end
        for _, r in ipairs(results) do
            table.insert(lines, string.format("CONFIRMED %s  (%s, field of %s)%s", r.path,
                r.comp and ("via.vec2 " .. r.comp) or r.kind, r.owner_type,
                r.kind == "quat" and string.format("  PitchSign=%s%s", r.pitch_sign or "?", r.yaw_ok and "" or
                "  (yaw turned the other way)") or string.format("  unit=%s sign=%d axis=%s",
                r.unit == 1 and "degrees" or "radians", r.sign, r.axis)))
        end
        local text = table.concat(lines, "\n")
        for _, l in ipairs(lines) do note(l) end
        pcall(fs.write, "gyro4all_camera.txt", text .. "\n")
        message = #results > 0 and "Done: see the results below (also in reframework\\data\\gyro4all_camera.txt)."
            or "Done: no rotation turned the camera. See the README for what to try next."
        return
    end
    test.frames = test.frames + 1
    if test.phase == "yaw" then
        if test.frames == 1 then
            test.original = read_value(c)
            if test.original == nil then return next_test(test.index + 1) end
            test.yaw0, test.pitch0 = yaw, pitch
            request_write(c, turn_quat(test.original, TEST_DEGREES, 0, 1))
        elseif test.frames >= TEST_FRAMES then
            local dy = wrap(yaw - test.yaw0)
            c.yaw_moved = math.abs(math.abs(dy) - TEST_DEGREES) <= 1.5
            c.moved = dy
            c.yaw_ok = dy < 0   -- a turn to the right lowers the yaw (positive = left)
            request_write(c, test.original)
            test.phase, test.frames = "yaw_undo", 0
        end
    elseif test.phase == "yaw_undo" then
        if test.frames >= TEST_FRAMES then
            if not c.yaw_moved then return next_test(test.index + 1) end
            test.phase, test.frames = "pitch", 0
        end
    elseif test.phase == "float" then
        -- A float angle moves the camera axis it followed in the captures by +TEST_DEGREES (sign and unit from there).
        if test.frames == 1 then
            test.original = read_value(c)
            if test.original == nil then return next_test(test.index + 1) end
            test.yaw0, test.pitch0 = yaw, pitch
            request_write(c, test.original + c.sign * c.unit * TEST_DEGREES)
        elseif test.frames >= TEST_FRAMES then
            local d = c.axis == "yaw" and wrap(yaw - test.yaw0) or (pitch - test.pitch0)
            c.float_moved = math.abs(d - TEST_DEGREES) <= 1.5
            c.moved = d
            request_write(c, test.original)
            test.phase, test.frames = "float_undo", 0
        end
    elseif test.phase == "float_undo" then
        if test.frames >= TEST_FRAMES then
            if c.float_moved then table.insert(results, c) end
            next_test(test.index + 1)
        end
    elseif test.phase == "pitch" then
        if test.frames == 1 then
            test.original = read_value(c)
            if test.original == nil then return next_test(test.index + 1) end
            test.pitch0 = pitch
            request_write(c, turn_quat(test.original, 0, TEST_DEGREES, 1))
        elseif test.frames >= TEST_FRAMES then
            local dp = pitch - test.pitch0
            if math.abs(dp) >= 1 then c.pitch_sign = dp > 0 and "1" or "-1" end
            request_write(c, test.original)
            test.phase, test.frames = "pitch_undo", 0
        end
    elseif test.phase == "pitch_undo" then
        if test.frames >= TEST_FRAMES then
            table.insert(results, c)
            next_test(test.index + 1)
        end
    end
    message = string.format("Live test %d of %d. Don't touch the controller.", math.min(test.index, #candidates),
        #candidates)
end

---------------------------------------------------------------------------------------------------------- per frame

re.on_frame(function()
    if state == "idle" or state == "done" or state == "failed" then return end
    if state == "scanning" then return scan_step() end
    local yaw, pitch = camera_angles()
    if not yaw then
        message = "No primary camera right now (loading or menu?): waiting."
        return
    end
    local moving = last_yaw and (math.abs(wrap(yaw - last_yaw)) > 0.2 or math.abs(pitch - last_pitch) > 0.2)
    last_yaw, last_pitch = yaw, pitch
    still = moving and 0 or still + 1

    if state == "settling" then
        message = "Keep the camera still..."
        if still >= STILL_FRAMES then
            take_baseline(yaw, pitch)
            state = "ready"
        end
    elseif state == "ready" then
        message = string.format("Capture %d of %d: turn the camera with the RIGHT STICK more than %d degrees sideways "
            .. "(and a bit up or down), then stop. %d rotations left.", captures + 1, CAPTURES, MIN_TURN, #candidates)
        if math.abs(wrap(yaw - base_yaw)) > MIN_TURN then state = "turning" end
    elseif state == "turning" then
        message = "Stop turning and leave the stick..."
        if still >= STILL_FRAMES then
            capture(yaw, pitch)
            if #candidates == 0 then
                state, message = "failed", "No rotation followed the camera. See the README for what to try next."
            elseif captures >= CAPTURES then
                note(string.format("live test of %d rotations (writes %s)", #candidates,
                    hooked and "before LateUpdateBehavior" or "from on_frame: no application entry hook"))
                state = "testing"
                next_test(1)
            else
                take_baseline(yaw, pitch)
                state = "ready"
            end
        end
    elseif state == "testing" then
        test_step(yaw, pitch)
    end
end)

---------------------------------------------------------------------------------------------------------- menu

local function quat_str(q)
    return q and string.format("x=%.3f y=%.3f z=%.3f w=%.3f", q.x, q.y, q.z, q.w) or "nil"
end

re.on_draw_ui(function()
    if not imgui.tree_node("Gyro4All camera finder") then return end
    if state == "idle" or state == "done" or state == "failed" then
        if imgui.button("Find the camera") then start_scan() end
    else
        if imgui.button("Cancel") then
            if test and pending_write == nil and test.original and candidates[test.index] then
                write_value(candidates[test.index], test.original)
            end
            state, scan, test, message = "idle", nil, nil, "Cancelled."
        end
    end
    imgui.text(message)

    if #results > 0 then
        imgui.text("Rotations that turn the camera (use the first, shortest path, in the camera model):")
        for _, r in ipairs(results) do
            imgui.text("  " .. r.path)
            if r.kind == "quat" then
                imgui.text(string.format("     %s on %s, PitchSign=%s%s", r.field, r.owner_type, r.pitch_sign or "?",
                    r.yaw_ok and "" or ", yaw turned the other way"))
            else
                imgui.text(string.format("     %s %s on %s: %s, sign %d, %s", r.comp and ("via.vec2 ." .. r.comp)
                    or "float", r.field, r.owner_type,
                    r.unit == 1 and "degrees" or "radians", r.sign, r.axis))
            end
        end
    elseif state == "ready" or state == "turning" or state == "testing" then
        imgui.text(string.format("%d rotations left. First ones:", #candidates))
        for i = 1, math.min(#candidates, 8) do
            local c = candidates[i]
            local v = read_value(c)
            imgui.text("  " .. c.path .. ": " .. (c.kind == "quat" and quat_str(v) or tostring(v)))
        end
    end

    if #log_lines > 0 then
        imgui.text("Log (also in re2_framework_log.txt):")
        for _, l in ipairs(log_lines) do imgui.text("  " .. l) end
    end
    imgui.tree_pop()
end)
