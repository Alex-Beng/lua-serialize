-- Compare C DES vs Lua Lockbox DES on ddata
local s = require "serialize"
local des = require "des"

local f = io.open("data2", "rb")
local data = f:read("*a")
f:close()

print("ddata file size: " .. #data)

-- C DES decryption
local t0 = os.clock()
local c_out = s.des_decrypt_cbc("Z1Battle", "20231012", data)
local t1 = os.clock()
print(string.format("C DES:   len=%d, first=0x%02x, last=0x%02x, time=%.3fs",
    #c_out, c_out:byte(1), c_out:byte(#c_out), t1 - t0))

-- Lua DES decryption
local t0 = os.clock()
local lua_out = des.decrypt_cbc("Z1Battle", "20231012", data)
local t1 = os.clock()
print(string.format("Lua DES: len=%d, first=0x%02x, last=0x%02x, time=%.3fs",
    #lua_out, lua_out:byte(1), lua_out:byte(#lua_out), t1 - t0))

-- Compare lengths
if #c_out == #lua_out then
    print("Lengths match: " .. #c_out)
else
    print(string.format("Lengths differ: C=%d, Lua=%d (diff=%d)",
        #c_out, #lua_out, #c_out - #lua_out))
end

-- Compare content block by block
local min_len = math.min(#c_out, #lua_out)
local diff_count = 0
local first_diff = nil
for i = 1, min_len do
    if c_out:byte(i) ~= lua_out:byte(i) then
        diff_count = diff_count + 1
        if not first_diff then
            first_diff = i
        end
    end
end

if diff_count == 0 and #c_out == #lua_out then
    print("Content identical!")
else
    print(string.format("Differences: %d bytes out of %d compared (%.2f%%)",
        diff_count, min_len, diff_count / min_len * 100))
    if first_diff then
        print("First difference at byte " .. first_diff)
    end
    if #c_out ~= #lua_out then
        print(string.format("Uncompared tail: C extra=%d, Lua extra=%d",
            math.max(0, #c_out - min_len), math.max(0, #lua_out - min_len)))
    end
end

-- Deserialize with both
print("\n--- C DES deserialize ---")
local ok1, r1 = pcall(s.deseristring_string_lz4, c_out)
if ok1 then
    print("OK, results: " .. select("#", r1))
    local function count(t)
        local n = 0; for _ in pairs(t or {}) do n = n + 1 end; return n
    end
    print("Result type: " .. type(r1) .. ", fields: " .. (type(r1) == "table" and count(r1) or "N/A"))
else
    print("FAIL: " .. r1)
end

print("\n--- Lua DES deserialize ---")
local ok2, r2 = pcall(s.deseristring_string_lz4, lua_out)
if ok2 then
    print("OK, results: " .. select("#", r2))
else
    print("FAIL: " .. r2)
end
