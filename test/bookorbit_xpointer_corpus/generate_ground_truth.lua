-- Emits real crengine xpointers for one EPUB by driving KOReader's CreDocument.
--
-- Run through the KOReader emulator's luajit, from the emulator directory:
--   SDL_VIDEODRIVER=dummy ./luajit <this file> <epub> <out.csv> [dom_version]
--
-- SDL_VIDEODRIVER=dummy is mandatory on a headless machine: require("device")
-- segfaults without a video driver. The fake device below is the headless
-- CanvasContext contract documented at frontend/document/canvascontext.lua:14-66.
io.stdout:setvbuf("line")
os.setlocale("C", "numeric")

require("setupkoenv")

local DataStorage = require("datastorage")
G_defaults = require("luadefaults"):open()
G_reader_settings = require("luasettings"):open(DataStorage:getDataDir() .. "/settings.reader.lua")

local screen = {
    getWidth = function() return 600 end,
    getHeight = function() return 800 end,
    getDPI = function() return 160 end,
    getSize = function() return {x = 0, y = 0, w = 600, h = 800} end,
    scaleBySize = function(_, n) return n end,
    isColorEnabled = function() return false end,
    fb_bpp = 8,
}
local device = {
    screen = screen,
    hasBGRFrameBuffer = function() return false end,
    hasEinkScreen = function() return true end,
    isAndroid = function() return false end,
    isDesktop = function() return true end,
    isEmulator = function() return true end,
    isKindle = function() return false end,
    isPocketBook = function() return false end,
    hasSystemFonts = function() return false end,
    canHWDither = function() return false end,
}
require("document/canvascontext"):init(device)

local DocumentRegistry = require("document/documentregistry")

local epub_path = assert(arg[1], "usage: generate_ground_truth.lua <epub> <out.csv> [dom_version]")
local out_path = assert(arg[2], "usage: generate_ground_truth.lua <epub> <out.csv> [dom_version]")

local doc = assert(DocumentRegistry:openDocument(epub_path), "cannot open " .. epub_path)
doc:loadDocument()

-- The DOM version decides xpointer shape, so it is an explicit input and is
-- recorded in the output header.
local dom_version = tonumber(arg[3]) or doc:getLatestDomVersion()
doc:requestDomVersion(dom_version)

doc:setViewMode("page")
doc:setViewDimen({w = 600, h = 800})
doc:render()

local out = assert(io.open(out_path, "w"))
out:write("# epub=", epub_path:gsub(".*/", ""), "\n")
out:write("# dom_version=", tostring(dom_version), "\n")
out:write("# dom_version_with_normalized_xpointers=", tostring(doc:getDomVersionWithNormalizedXPointers()), "\n")
out:write("# oldest_dom_version=", tostring(doc:getOldestDomVersion()), "\n")
out:write("# latest_dom_version=", tostring(doc:getLatestDomVersion()), "\n")
out:write("page,xpointer\n")

local pages = doc:getPageCount()
for page = 1, pages do
    local xp = doc:getPageXPointer(page)
    if xp and xp ~= "" then
        -- The CSV has exactly two columns; a comma inside an xpointer would
        -- break the loader silently, so fail loudly instead.
        assert(not xp:find(","), "xpointer contains a comma: " .. xp)
        out:write(page, ",", xp, "\n")
    end
end
out:close()
doc:close()

io.write("wrote ", out_path, " (", tostring(pages), " pages, dom ", tostring(dom_version), ")\n")
