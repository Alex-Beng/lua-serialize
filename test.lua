s = require "serialize"

b = s.pack{[10] = {1,2}}
s.dump(b)
bb = s.unpack(b)
for k,v in pairs(bb[10]) do
	print(k,v)
end

--[[

a = s.pack { hello={3,4}, false, 1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9 }

s.dump(a)

a = s.append(a, 42,4.2,-1,1000,80000,"hello",true,false,nil,"1234567890123456789012345678901234567890")

s.dump(a)
print(a)

function pr(t,...)
	for k,v in pairs(t) do
		print(k,v)
	end
	print(...)
end

print ("------")

local seri, length = s.serialize(a)
print(seri, length)

pr(s.unpack(a))

print("-------")

pr(s.deserialize(seri))
]]

a = s.serialize_string( 1,2,3,4,5 )
print(#a, s.deseristring_string(a))

-- test lz4 compression (< 1024)
a = s.serialize_string_lz4( 1 )
print("small tag:", string.byte(a,1), "len:", #a, "expected: 0x00, len=7")
local r = { s.deseristring_string_lz4(a) }
for _, v in ipairs(r) do print(v) end

-- test lz4 compression (>= 1024)
local big = string.rep("Hello World! ", 1000)
a = s.serialize_string_lz4( big )
print("large tag:", string.byte(a,1), "len:", #a, "raw len:", #s.serialize_string(big))
print(s.deseristring_string_lz4(a))
