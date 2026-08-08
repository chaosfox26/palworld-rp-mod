-- json.lua — small JSON codec for the Lua side.
--
-- The C++ layer hands Lua JSON text, so something has to decode it. This is
-- deliberately minimal: decode for everything the backend sends, encode for the
-- small request bodies we POST (profiles, mutes, guild passcodes).
--
-- It never touches chat ciphertext. Envelopes are built and opened in C++, so a
-- rounding or escaping quirk in here can never corrupt a GCM tag.

local json = {}

-- ------------------------------------------------------------------- decoding

local function skipWhitespace(str, i)
    local _, j = str:find("^[ \n\r\t]*", i)
    return (j or i - 1) + 1
end

local escapes = {
    ['"'] = '"', ['\\'] = '\\', ['/'] = '/',
    b = '\b', f = '\f', n = '\n', r = '\r', t = '\t',
}

local parseValue

local function parseString(str, i)
    -- i points at the opening quote.
    i = i + 1
    local parts = {}
    while true do
        local c = str:sub(i, i)
        if c == '' then
            return nil, i, 'unterminated string'
        elseif c == '"' then
            return table.concat(parts), i + 1
        elseif c == '\\' then
            local esc = str:sub(i + 1, i + 1)
            if esc == 'u' then
                local hex = str:sub(i + 2, i + 5)
                local code = tonumber(hex, 16)
                if not code then return nil, i, 'bad \\u escape' end
                -- Encode as UTF-8. Surrogate pairs are joined so non-BMP
                -- characters survive; players do use emoji in roleplay names.
                if code >= 0xD800 and code <= 0xDBFF and str:sub(i + 6, i + 7) == '\\u' then
                    local low = tonumber(str:sub(i + 8, i + 11), 16)
                    if low and low >= 0xDC00 and low <= 0xDFFF then
                        code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)
                        i = i + 6
                    end
                end
                if code < 0x80 then
                    parts[#parts + 1] = string.char(code)
                elseif code < 0x800 then
                    parts[#parts + 1] = string.char(0xC0 | (code >> 6), 0x80 | (code & 0x3F))
                elseif code < 0x10000 then
                    parts[#parts + 1] = string.char(0xE0 | (code >> 12),
                                                    0x80 | ((code >> 6) & 0x3F),
                                                    0x80 | (code & 0x3F))
                else
                    parts[#parts + 1] = string.char(0xF0 | (code >> 18),
                                                    0x80 | ((code >> 12) & 0x3F),
                                                    0x80 | ((code >> 6) & 0x3F),
                                                    0x80 | (code & 0x3F))
                end
                i = i + 6
            else
                local mapped = escapes[esc]
                if not mapped then return nil, i, 'bad escape' end
                parts[#parts + 1] = mapped
                i = i + 2
            end
        else
            -- Take a whole run of ordinary characters at once rather than one at
            -- a time: a 100 KB message would otherwise be 100,000 concatenations.
            local chunkEnd = str:find('["\\]', i)
            if not chunkEnd then return nil, i, 'unterminated string' end
            parts[#parts + 1] = str:sub(i, chunkEnd - 1)
            i = chunkEnd
        end
    end
end

local function parseNumber(str, i)
    local s, e = str:find('^-?%d+%.?%d*[eE]?[-+]?%d*', i)
    if not s then return nil, i, 'bad number' end
    local n = tonumber(str:sub(s, e))
    if n == nil then return nil, i, 'bad number' end
    return n, e + 1
end

local function parseArray(str, i)
    local out = {}
    i = skipWhitespace(str, i + 1)
    if str:sub(i, i) == ']' then return out, i + 1 end
    while true do
        local value, err
        value, i, err = parseValue(str, i)
        if err then return nil, i, err end
        out[#out + 1] = value
        i = skipWhitespace(str, i)
        local c = str:sub(i, i)
        if c == ',' then
            i = skipWhitespace(str, i + 1)
        elseif c == ']' then
            return out, i + 1
        else
            return nil, i, 'expected , or ] in array'
        end
    end
end

local function parseObject(str, i)
    local out = {}
    i = skipWhitespace(str, i + 1)
    if str:sub(i, i) == '}' then return out, i + 1 end
    while true do
        if str:sub(i, i) ~= '"' then return nil, i, 'expected a key' end
        local key, err
        key, i, err = parseString(str, i)
        if err then return nil, i, err end
        i = skipWhitespace(str, i)
        if str:sub(i, i) ~= ':' then return nil, i, 'expected :' end
        i = skipWhitespace(str, i + 1)
        local value
        value, i, err = parseValue(str, i)
        if err then return nil, i, err end
        out[key] = value
        i = skipWhitespace(str, i)
        local c = str:sub(i, i)
        if c == ',' then
            i = skipWhitespace(str, i + 1)
        elseif c == '}' then
            return out, i + 1
        else
            return nil, i, 'expected , or } in object'
        end
    end
end

parseValue = function(str, i)
    i = skipWhitespace(str, i)
    local c = str:sub(i, i)
    if c == '{' then return parseObject(str, i) end
    if c == '[' then return parseArray(str, i) end
    if c == '"' then return parseString(str, i) end
    if c == 't' and str:sub(i, i + 3) == 'true' then return true, i + 4 end
    if c == 'f' and str:sub(i, i + 4) == 'false' then return false, i + 5 end
    if c == 'n' and str:sub(i, i + 3) == 'null' then return nil, i + 4 end
    if c == '-' or c:match('%d') then return parseNumber(str, i) end
    return nil, i, 'unexpected character ' .. (c == '' and '<end>' or c)
end

-- Returns table, or nil plus an error string. Never raises: a malformed frame
-- from the network must not be able to take down the mod.
function json.decode(str)
    if type(str) ~= 'string' or str == '' then return nil, 'empty input' end
    local ok, value, _, err = pcall(function()
        local v, i, e = parseValue(str, 1)
        return v, i, e
    end)
    if not ok then return nil, 'decode failed' end
    if err then return nil, err end
    return value
end

-- ------------------------------------------------------------------- encoding

local encodeValue

local stringEscapes = {
    ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b',
    ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function encodeString(s)
    return '"' .. s:gsub('[%c"\\]', function(c)
        return stringEscapes[c] or string.format('\\u%04x', c:byte())
    end) .. '"'
end

local function isArray(t)
    local n = 0
    for k in pairs(t) do
        if type(k) ~= 'number' then return false end
        n = n + 1
    end
    return n == #t
end

encodeValue = function(v)
    local t = type(v)
    if v == nil then return 'null' end
    if t == 'boolean' then return v and 'true' or 'false' end
    if t == 'number' then
        if v ~= v or v == math.huge or v == -math.huge then return 'null' end
        if math.type(v) == 'integer' then return tostring(v) end
        return string.format('%.14g', v)
    end
    if t == 'string' then return encodeString(v) end
    if t == 'table' then
        local parts = {}
        if isArray(v) then
            for i = 1, #v do parts[#parts + 1] = encodeValue(v[i]) end
            return '[' .. table.concat(parts, ',') .. ']'
        end
        for k, val in pairs(v) do
            if type(k) == 'string' then
                parts[#parts + 1] = encodeString(k) .. ':' .. encodeValue(val)
            end
        end
        return '{' .. table.concat(parts, ',') .. '}'
    end
    return 'null'
end

function json.encode(v)
    local ok, result = pcall(encodeValue, v)
    if not ok then return nil, 'encode failed' end
    return result
end

return json
