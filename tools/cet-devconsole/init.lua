-- CyberCraft dev console (development only, not shipped).
--
-- A remote Lua console for Cyber Engine Tweaks: write Lua into cmd.lua (in this mod's folder) and it
-- runs on the game's main thread on the next frame; print() output and the result go to out.txt.
-- Each command starts with a line "-- <id>" so the same text can be sent twice; out.txt echoes the id.
--
--   tools\cet.ps1 "return Game.GetPlayer():GetWorldPosition()"

local lastId = nil
local timer = 0

local function fmt(v)
	local t = type(v)
	if t == "userdata" or t == "table" then
		local ok, s = pcall(Dump, v, false)
		if ok and s then
			return s
		end
		ok, s = pcall(GameDump, v)
		if ok and s then
			return s
		end
	end
	return tostring(v)
end

local function run(src, id)
	local lines = {}
	local env = setmetatable({}, { __index = _G })
	env.print = function(...)
		local parts = {}
		for i = 1, select("#", ...) do
			parts[#parts + 1] = fmt(select(i, ...))
		end
		lines[#lines + 1] = table.concat(parts, "\t")
	end
	local chunk, err = load(src, "cmd", "t", env)
	if not chunk then
		lines[#lines + 1] = "compile error: " .. tostring(err)
	else
		local results = table.pack(pcall(chunk))
		if not results[1] then
			lines[#lines + 1] = "error: " .. tostring(results[2])
		else
			for i = 2, results.n do
				lines[#lines + 1] = "=> " .. fmt(results[i])
			end
		end
	end
	local out = io.open("out.txt", "w")
	if out then
		out:write("-- " .. id .. "\n" .. table.concat(lines, "\n") .. "\n")
		out:close()
	end
end

registerForEvent("onUpdate", function(dt)
	timer = timer + dt
	if timer < 0.1 then
		return
	end
	timer = 0
	local f = io.open("cmd.lua", "r")
	if not f then
		return
	end
	local src = f:read("*a")
	f:close()
	local id = src:match("^%-%- (%S+)")
	if not id or id == lastId then
		return
	end
	lastId = id
	run(src, id)
end)

registerForEvent("onInit", function()
	print("[CyberCraft dev console] ready")
end)
