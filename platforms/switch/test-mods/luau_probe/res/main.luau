-- H36 Luau probe (platforms/switch/test-mods/luau_probe).
--
-- Shows that a script mod packaged as a .dusk on the SD card is discovered, runs on the
-- Luau runtime compiled into the NRO, reads its own resources from the package and can be
-- turned off from the Mods window. Every line it logs starts with [h36-probe]. It registers
-- no settings, touches no save and writes no file; the one thing on screen is a notice.
local log = require("dusklight.log")
local host = require("dusklight.host")

local TAG = "[h36-probe] "
log.info(TAG .. "start: " .. host.mod_id() .. " v" .. host.mod_version() .. " from " .. host.mod_dir())

local failures = 0
local function step(name, fn)
    local ok, result = pcall(fn)
    if ok then
        log.info(TAG .. "ok " .. name .. ": " .. tostring(result))
    else
        failures += 1
        log.error(TAG .. "FAIL " .. name .. ": " .. tostring(result))
    end
end

-- 1. A resource read from the package itself (the card), not from the NRO.
step("resource", function()
    local resource = require("dusklight.resource")
    local text = resource.load("probe.txt")
    local first = string.match(text, "^[^\n]*")
    return string.format("probe.txt %d bytes, first line '%s'", #text, first)
end)

-- 2. A second script of the package, through require.
step("require", function()
    local util = require("./probe_util")
    return util.describe()
end)

-- 3. The VM itself: a computation with a known answer.
step("compute", function()
    local sum = 0
    for i = 1, 1000 do
        sum += i * i
    end
    assert(sum == 333833500, "sum of squares was " .. sum)
    return "sum of squares 1..1000 = " .. sum
end)

-- 4. A notice on screen.
step("notice", function()
    local ui = require("dusklight.ui")
    ui.push_toast({ title_rml = "Luau probe", body_rml = "Script loaded from the SD card", duration_ms = 6000 })
    return "shown"
end)

local updates = 0
host.on_update(function()
    updates += 1
    if updates == 600 then
        log.info(TAG .. "update: 600 frames")
    end
end)

host.on_shutdown(function()
    log.info(TAG .. "shutdown after " .. updates .. " update(s)")
end)

if failures == 0 then
    log.info(TAG .. "success: 4 of 4 steps")
else
    log.error(TAG .. "finished with " .. failures .. " failed step(s)")
    host.fail(failures .. " probe step(s) failed; see the log")
end
