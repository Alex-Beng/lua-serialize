local s = require "serialize"

local f = io.open(arg[1], "rb")
local data = f:read("*all")
f:close()

local key = "Z1Battle"
local iv  = "20231012"
local decrypted = s.des_decrypt_cbc(key, iv, data)
print(string.format("decrypted len: %d, first byte: %02x", #decrypted, decrypted:byte(1)))

local results = { pcall(s.deseristring_string_lz4, decrypted) }
local ok = table.remove(results, 1)
if not ok then
	print("deserialize error:", results[1])
else
	local function dump(t, indent)
		indent = indent or 0
		if type(t) ~= "table" then
			print(string.rep("  ", indent) .. tostring(t))
			return
		end
		for k, v in pairs(t) do
			local ks = type(k) == "string" and k or string.format("[%d]", k)
			if type(v) == "table" then
				print(string.rep("  ", indent) .. ks .. " =")
				dump(v, indent + 1)
			else
				print(string.rep("  ", indent) .. ks .. " = s" .. string.len(tostring(v)))
			end
		end
	end
	for i, v in ipairs(results) do
		print("--- result " .. i .. " ---")
		dump(v)
	end
end
