local DES = require("lockbox.cipher.des")
local Array = require("lockbox.util.array")

local des = {}

function des.decrypt_cbc(key_str, iv_str, ciphertext)
    local key = Array.fromString(key_str)
    local iv_bytes = Array.fromString(iv_str)

    local out_chars = {}
    local idx = 1
    for i = 1, #ciphertext, 8 do
        local block = {}
        for j = 1, 8 do
            block[j] = ciphertext:byte(i + j - 1)
        end

        local decrypted = DES.decrypt(key, block)

        for j = 1, 8 do
            out_chars[idx] = string.char(bit32.bxor(decrypted[j], iv_bytes[j]))
            idx = idx + 1
        end

        iv_bytes = block
    end

    local result = table.concat(out_chars)

    local pad_len = result:byte(#result)
    result = result:sub(1, #result - pad_len)

    return result
end

function des.encrypt_cbc(key_str, iv_str, plaintext)
    local key = Array.fromString(key_str)
    local iv_bytes = Array.fromString(iv_str)

    local pad_len = 8 - (#plaintext % 8)
    if pad_len == 0 then pad_len = 8 end
    plaintext = plaintext .. string.rep(string.char(pad_len), pad_len)

    local output = {}
    for i = 1, #plaintext, 8 do
        local block = {}
        for j = 1, 8 do
            block[j] = bit32.bxor(plaintext:byte(i + j - 1), iv_bytes[j])
        end

        local encrypted = DES.encrypt(key, block)

        for j = 1, 8 do
            output[#output + 1] = encrypted[j]
        end

        iv_bytes = encrypted
    end

    return string.char(table.unpack(output))
end

return des
