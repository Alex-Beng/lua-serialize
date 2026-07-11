-- Test DES-CBC encryption (C serialize.des_encrypt_cbc vs Lua des.encrypt_cbc)
local s = require "serialize"
local des = require "des"

local ok = true
local total = 0
local passed = 0

function check(name, cond)
	total = total + 1
	if cond then
		passed = passed + 1
		io.write("  PASS: " .. name .. "\n")
	else
		ok = false
		io.write("  FAIL: " .. name .. "\n")
	end
end

-- Test vectors with different keys/IVs
local tests = {
	{ key = "Z1Battle", iv = "20231012", data = "" },
	{ key = "Z1Battle", iv = "20231012", data = "a" },
	{ key = "Z1Battle", iv = "20231012", data = "hello" },
	{ key = "Z1Battle", iv = "20231012", data = "1234567" },
	{ key = "Z1Battle", iv = "20231012", data = "12345678" },
	{ key = "Z1Battle", iv = "20231012", data = "123456789" },
	{ key = "Z1Battle", iv = "20231012", data = "1234567887654321" },
	{ key = "Z1Battle", iv = "20231012", data = "The quick brown fox jumps over the lazy dog" },
	{ key = "abcdefgh", iv = "12345678", data = "test data for DES-CBC encryption" },
	{ key = "\x01\x02\x03\x04\x05\x06\x07\x08", iv = "\x10\x20\x30\x40\x50\x60\x70\x80", data = "binary key/iv test" },
	{ key = "deadbeef", iv = "cafebabe", data = "" },
	{ key = "deadbeef", iv = "cafebabe", data = "x" },
	{ key = "deadbeef", iv = "cafebabe", data = "exactly8" },
	{ key = "deadbeef", iv = "cafebabe", data = "morethan8bytelongdata!!" },
}

print("=== Test 1: C encrypt vs Lua encrypt (cross-validation) ===")
for _, t in ipairs(tests) do
	local c_ct = s.des_encrypt_cbc(t.key, t.iv, t.data)
	local lua_ct = des.encrypt_cbc(t.key, t.iv, t.data)
	local data_desc = string.format("data len=%d", #t.data)
	check(string.format("C vs Lua encrypt [key=%q, iv=%q, %s]", t.key, t.iv, data_desc),
		c_ct == lua_ct)
end

print("\n=== Test 2: C encrypt + C decrypt roundtrip ===")
for _, t in ipairs(tests) do
	local ct = s.des_encrypt_cbc(t.key, t.iv, t.data)
	local pt = s.des_decrypt_cbc(t.key, t.iv, ct)
	check(string.format("C roundtrip [key=%q, iv=%q, data len=%d]", t.key, t.iv, #t.data),
		pt == t.data)
end

print("\n=== Test 3: C encrypt + Lua decrypt roundtrip ===")
for _, t in ipairs(tests) do
	local ct = s.des_encrypt_cbc(t.key, t.iv, t.data)
	local pt = des.decrypt_cbc(t.key, t.iv, ct)
	check(string.format("C encrypt + Lua decrypt [key=%q, iv=%q, data len=%d]", t.key, t.iv, #t.data),
		pt == t.data)
end

print("\n=== Test 4: Lua encrypt + C decrypt roundtrip ===")
for _, t in ipairs(tests) do
	local ct = des.encrypt_cbc(t.key, t.iv, t.data)
	local pt = s.des_decrypt_cbc(t.key, t.iv, ct)
	check(string.format("Lua encrypt + C decrypt [key=%q, iv=%q, data len=%d]", t.key, t.iv, #t.data),
		pt == t.data)
end

print("\n=== Test 5: Ciphertext length must be multiple of 8 ===")
for _, t in ipairs(tests) do
	local ct = s.des_encrypt_cbc(t.key, t.iv, t.data)
	local data_desc = string.format("data len=%d", #t.data)
	check(string.format("ciphertext len %% 8 == 0 [%s]", data_desc), #ct % 8 == 0)
	local expected = (#t.data // 8 + 1) * 8
	check(string.format("ciphertext len == %d [%s]", expected, data_desc), #ct == expected)
end

print("\n=== Test 6: Deterministic (same input = same output) ===")
for _, t in ipairs(tests) do
	local ct1 = s.des_encrypt_cbc(t.key, t.iv, t.data)
	local ct2 = s.des_encrypt_cbc(t.key, t.iv, t.data)
	check(string.format("deterministic [key=%q, iv=%q, data len=%d]", t.key, t.iv, #t.data),
		ct1 == ct2)
end

print("\n=== Test 7: Different keys produce different ciphertext ===")
if #tests >= 2 then
	local ct1 = s.des_encrypt_cbc(tests[1].key, tests[1].iv, tests[1].data)
	local ct2 = s.des_encrypt_cbc(tests[#tests].key, tests[#tests].iv, tests[1].data)
	check("different keys -> different ciphertext", ct1 ~= ct2)
end

print("\n=== Test 8: Invalid key/IV length errors ===")
local ok_err, err1 = pcall(s.des_encrypt_cbc, "short", "12345678", "data")
check("short key raises error", not ok_err)
local ok_err2, err2 = pcall(s.des_encrypt_cbc, "12345678", "short", "data")
check("short IV raises error", not ok_err2)

print(string.format("\n=== Results: %d/%d passed ===", passed, total))
if not ok then
	io.write("SOME TESTS FAILED\n")
	os.exit(1)
else
	io.write("ALL TESTS PASSED\n")
end
