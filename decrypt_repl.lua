local s = require "serialize"

local filename = select(1, ...)
local key = select(2, ...) or "Z1Battle"
local iv  = select(3, ...) or "20231012"

if not filename then
    print("Usage: lua decrypt_repl.lua <file> [key] [iv]")
    print("  Default key: Z1Battle, iv: 20231012")
    os.exit(1)
end

local f = io.open(filename, "rb")
if not f then
    print("Cannot open file: " .. filename)
    os.exit(1)
end
local encrypted = f:read("*a")
f:close()
print(string.format("Read %d bytes from %s", #encrypted, filename))

local t0 = os.clock()
local decrypted = s.des_decrypt_cbc(key, iv, encrypted)
if not decrypted then
    print("Decryption failed")
    os.exit(1)
end
local t1 = os.clock()
print(string.format("Decrypted: %d bytes (%.3fs)", #decrypted, t1 - t0))
print(string.format("First byte: 0x%02X", decrypted:byte(1)))

local ok = pcall(function()
    local results = { s.deseristring_string_lz4(decrypted) }
    r = results[1]
    all = results
end)
if not ok then
    print("Deserialization failed: " .. tostring(all))
    os.exit(1)
end
print(string.format("Deserialized: %d value(s)", #all))
print()

function keys(t)
    if type(t) ~= "table" then
        print("not a table, type=" .. type(t))
        return
    end
    local count = 0
    for k, v in pairs(t) do
        count = count + 1
        local vs = tostring(v)
        if type(v) == "string" and #v > 60 then
            vs = string.format("string(%d) \"%s...\"", #v, v:sub(1, 57))
        elseif type(v) == "table" then
            local n = 0; for _ in pairs(v) do n = n + 1 end
            vs = string.format("table(%d keys)", n)
        end
        local ks = type(k) == "string" and k or "[" .. tostring(k) .. "]"
        if type(k) == "string" then
            print(string.format("  %-20s = %s", ks, vs))
        else
            print(string.format("  %-20s = %s", ks, vs))
        end
    end
    print(string.format("-- %d keys --", count))
end

function dump(t, indent)
    indent = indent or 0
    local pad = string.rep("  ", indent)
    if type(t) ~= "table" then
        print(pad .. tostring(t))
        return
    end
    print(pad .. "{")
    local keys_arr = {}
    for k in pairs(t) do table.insert(keys_arr, k) end
    table.sort(keys_arr, function(a, b)
        if type(a) ~= type(b) then return type(a) < type(b) end
        return a < b
    end)
    for _, k in ipairs(keys_arr) do
        local v = t[k]
        local ks
        if type(k) == "string" then
            ks = k
        elseif type(k) == "number" then
            ks = "[" .. k .. "]"
        else
            ks = "[" .. tostring(k) .. "]"
        end
        if type(v) == "table" then
            print(pad .. "  " .. ks .. " =")
            dump(v, indent + 2)
        elseif type(v) == "string" then
            if #v > 120 then
                print(pad .. "  " .. ks .. " = \"" .. v:sub(1, 117) .. "...\"  (" .. #v .. " bytes)")
            else
                print(pad .. "  " .. ks .. " = \"" .. v .. "\"")
            end
        else
            print(pad .. "  " .. ks .. " = " .. tostring(v))
        end
    end
    print(pad .. "}")
end

print("--- REPL ---")
print("  r     = deserialized result")
print("  keys  = keys(t)     list table keys")
print("  dump  = dump(t,?)   recursive dump")
print("  exit  = quit")
print("  Enter any Lua expression")
print()

while true do
    io.write("> ")
    io.flush()
    local line = io.read()
    if not line then break end
    line = line:match("^%s*(.-)%s*$")
    if line == "" then goto continue end
    if line == "exit" or line == "quit" then break end

    local f, err = load("return " .. line, "=repl")
    if not f then
        f, err = load(line, "=repl")
    end

    if f then
        local ok_res, res = pcall(f)
        if ok_res then
            if res ~= nil then
                print(tostring(res))
            end
        else
            print("Error: " .. tostring(res))
        end
    else
        print(err)
    end

    ::continue::
end
