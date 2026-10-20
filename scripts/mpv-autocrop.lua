-- Auto Crop [AUTO] for 240-MP: crops black bars that are part of the picture,
-- so a 4:3 show inside a 16:9 file (most YouTube uploads) fills a 4:3 screen
-- while real 16:9 video stays letterboxed.
--
-- Loaded by --script= only when auto_crop is "Auto" and the decode path can
-- crop. ffmpeg's cropdetect measures the non-black area; the result goes in
-- --video-crop, which applies before scaling, so the OSC CROP button (panscan)
-- still works on top of it.
--
-- reset=0 makes cropdetect report the largest area seen since the filter was
-- added, so the crop only ever widens: a dark opening scene can crop too much
-- at first, and the picture opens up as soon as brighter frames arrive. The
-- filter stays for the whole file; it only sums rows and columns of luma.
--
-- Needs frames in system memory: skipped when decoding to a GPU surface
-- (e.g. HEVC on a Pi 5, hwdec=drm), where reading frames back costs too much.

local LABEL = "mp240autocrop"
local MIN_AREA = 0.25     -- a smaller result means the frames so far are black
local MIN_BAR = 0.01      -- bars thinner than this (of the frame) are left alone

local timer = nil
local applied = nil

local start   -- below; stop() refers to it

local function stop()
    mp.unregister_event(start)
    if timer then
        timer:kill(); timer = nil
        mp.command("no-osd vf remove @" .. LABEL)
    end
end

local function check()
    local meta = mp.get_property_native("vf-metadata/" .. LABEL)
    local W = mp.get_property_number("video-dec-params/w")
    local H = mp.get_property_number("video-dec-params/h")
    if not meta or not W or not H then return end
    local w = tonumber(meta["lavfi.cropdetect.w"])
    local h = tonumber(meta["lavfi.cropdetect.h"])
    local x = tonumber(meta["lavfi.cropdetect.x"])
    local y = tonumber(meta["lavfi.cropdetect.y"])
    if not (w and h and x and y) or w < W * MIN_AREA or h < H * MIN_AREA then return end

    if x < W * MIN_BAR and W - x - w < W * MIN_BAR then x, w = 0, W end
    if y < H * MIN_BAR and H - y - h < H * MIN_BAR then y, h = 0, H end
    local crop = (w == W and h == H) and "" or string.format("%dx%d+%d+%d", w, h, x, y)
    if crop ~= (applied or "") then
        mp.msg.info("crop " .. (crop == "" and "none" or crop) .. " of " .. W .. "x" .. H)
        mp.set_property("video-crop", crop)
        applied = crop
    end
end

start = function()
    mp.unregister_event(start)
    local hwdec = mp.get_property("hwdec-current", "no")
    if hwdec ~= "no" and hwdec ~= "" and not hwdec:find("%-copy$") then
        mp.msg.info("not detecting: frames are on the GPU (hwdec " .. hwdec .. ")")
        return
    end
    mp.command("no-osd vf add @" .. LABEL .. ":lavfi=[cropdetect=limit=24:round=2:reset=0]")
    timer = mp.add_periodic_timer(0.5, check)
end

mp.register_event("file-loaded", function()
    stop()
    if applied then mp.set_property("video-crop", ""); applied = nil end
    local track = mp.get_property_native("current-tracks/video")
    if not track or track.image or track.albumart then return end
    -- hwdec-current is known once the decoder runs: wait for the first frame.
    mp.register_event("playback-restart", start)
end)

mp.register_event("end-file", stop)
