-- main.lua — commands, chat model, and position reporting.
--
-- Everything that can block lives in the C++ side. This file only ever pushes to
-- a queue or drains one, so a slow network cannot hitch a frame.
--
-- Division of labour:
--   C++  : socket, TLS, Engine.IO/Socket.IO framing, AES-256-GCM, ImGui window
--   Lua  : slash parsing, chat model, config, position sampling, /who rendering

local json = require("json")

local M = {}

-- ---------------------------------------------------------------------- config

local CONFIG_PATH = "rp-config.json"

local config = {
    -- Printed by the installer: `palworld-rp url`
    backendUrl = "https://212-28-190-199.sslip.io",

    -- Only needed when the backend runs on a home machine behind a router, where
    -- Let's Encrypt cannot validate and Caddy issues its own certificate. Point
    -- this at the exported local-ca.crt. Leave empty for a public IP host.
    caFile = "",

    positionHz = 3,

    -- /1 .. /10 are a client-side convenience. Channels go on the wire by name,
    -- so this is just the slot mapping.
    channels = { "ooc", "trade" },

    rememberPassword = false,
    savedName = "",
    savedPassword = "",

    timestamps = true,
}

local function configFile()
    -- UE4SS runs scripts with the mod folder as the working directory in current
    -- versions, but that has changed before. If the file is not found, log the
    -- resolved path rather than silently falling back to defaults.
    return CONFIG_PATH
end

local function loadConfig()
    local f = io.open(configFile(), "r")
    if not f then
        print("[RP] No rp-config.json found; using defaults. Backend: " .. config.backendUrl .. "\n")
        return
    end
    local contents = f:read("*a")
    f:close()
    local parsed = json.decode(contents)
    if type(parsed) ~= "table" then
        print("[RP] rp-config.json is not valid JSON; using defaults.\n")
        return
    end
    for k, v in pairs(parsed) do
        config[k] = v
    end
end

local function saveConfig()
    local encoded = json.encode(config)
    if not encoded then return end
    local f = io.open(configFile(), "w")
    if not f then return end
    f:write(encoded)
    f:close()
end

-- ----------------------------------------------------------------- chat model

-- Kept UI-agnostic on purpose: the ImGui window is fed formatted lines, so a
-- UMG front end later needs no changes here.
local model = {
    lines = {},
    maxLines = 2000,
    status = "Offline",
    characterName = "",

    -- Fallbacks only. Every one of these is replaced by the `limits` event, which
    -- the C++ side emits whenever it re-reads GET /info. The server's admin
    -- overrides file can change them mid-session, so nothing here may be treated
    -- as constant.
    maxBytes = 99984,
    maxProfileBytes = 262144,
    radius = 3000,
    minPasswordLength = 8,
    requireEncryption = true,

    guild = nil,
    channels = {},
    party = nil,
}

local function stamp()
    if not config.timestamps then return "" end
    return os.date("[%H:%M] ")
end

local function addLine(text)
    model.lines[#model.lines + 1] = text
    if #model.lines > model.maxLines then
        table.remove(model.lines, 1)
    end
    rp_ui_log(text)
end

local function notice(text)
    addLine(stamp() .. "* " .. text)
end

local function errorLine(text)
    -- chat_error covers rate limits, oversized messages and unjoined channels.
    -- Swallowing these silently is what makes a chat mod feel broken.
    addLine(stamp() .. "!! " .. text)
end

-- --------------------------------------------------------- inbound formatting

local function renderChat(frame)
    local kind = frame.type or "say"
    local sender = frame.sender or "?"
    local message = frame.message or ""
    local prefix = stamp()

    if kind == "say" then
        addLine(prefix .. sender .. ' says: "' .. message .. '"')
    elseif kind == "emote" then
        -- Already composed: with encryption on, the C++ side prepends the
        -- server-assigned sender name, because the server cannot read the text.
        addLine(prefix .. message)
    elseif kind == "global" then
        addLine(prefix .. "[Global] " .. sender .. ": " .. message)
    elseif kind == "guild" then
        addLine(prefix .. "[Guild] " .. sender .. ": " .. message)
    elseif kind == "party" then
        addLine(prefix .. "[Party] " .. sender .. ": " .. message)
    elseif kind == "channel" then
        addLine(prefix .. "[" .. (frame.channel or "?") .. "] " .. sender .. ": " .. message)
    elseif kind == "whisper_in" then
        addLine(prefix .. sender .. " whispers: " .. message)
    elseif kind == "whisper_out" then
        addLine(prefix .. "To " .. (frame.target or "?") .. ": " .. message)
    else
        addLine(prefix .. sender .. ": " .. message)
    end

    if frame.decryptFailed then
        errorLine("A message could not be decrypted. Your key may not match the sender's build.")
    end
end

-- ------------------------------------------------------- request bookkeeping

local nextRequestId = 1
local pending = {}

local function request(handler)
    local id = nextRequestId
    nextRequestId = id + 1
    pending[id] = handler
    return id
end

-- Convenience wrapper so no call site forgets the encryptBody argument. Profile
-- writes MUST pass true: while the server has REQUIRE_ENCRYPTION on, which is the
-- deployed default, a plaintext profile body is rejected.
--
-- Named apiCall rather than rest because several command handlers take a
-- parameter called `rest` (the rest of the typed line) and would shadow it.
local function apiCall(method, path, body, handler, encryptBody)
    rp_rest(method, path, body or "", request(handler or function() end), encryptBody or false)
end

-- ------------------------------------------------------------- validation

-- Same rules the server applies, checked here so the player gets an instant
-- answer instead of a round trip. Kept identical on purpose: if these drift, the
-- mod starts rejecting names the server would have accepted.
local RESERVED = {
    con = true, prn = true, aux = true, nul = true,
}
for i = 0, 9 do
    RESERVED["com" .. i] = true
    RESERVED["lpt" .. i] = true
end

local function validateName(name)
    if type(name) ~= "string" then return nil, "Name is missing." end
    local n = name:gsub("^%s+", ""):gsub("%s+$", ""):lower()
    if not n:match("^[a-z0-9][a-z0-9_-]+$") or #n < 2 or #n > 24 then
        return nil, "Names are 2 to 24 characters: lowercase letters, digits, _ and -, starting with a letter or digit."
    end
    if RESERVED[n] then
        return nil, "'" .. n .. "' is a reserved device name on Windows and cannot be used."
    end
    return n
end

local function validateChannel(channel)
    if type(channel) ~= "string" then return nil, "Channel is missing." end
    local c = channel:gsub("^%s+", ""):gsub("%s+$", ""):lower()
    if not c:match("^[a-z0-9][a-z0-9_-]+$") or #c < 2 or #c > 32 then
        return nil, "Channel names are 2 to 32 characters: lowercase letters, digits, _ and -."
    end
    if RESERVED[c] then
        return nil, "'" .. c .. "' is a reserved device name on Windows and cannot be used."
    end
    return c
end

-- ------------------------------------------------------------ command parsing

local commands = {}

local function splitFirstWord(text)
    local word, rest = text:match("^(%S+)%s*(.*)$")
    return word, rest or ""
end

local function slotToChannel(slot)
    local n = tonumber(slot)
    if not n then return nil end
    if n < 1 or n > 10 then return nil end
    return config.channels[n]
end

local function send(command, target, channel, message)
    if message == nil or message == "" then
        errorLine("Nothing to send.")
        return
    end
    if #message > model.maxBytes then
        errorLine("That message is " .. #message .. " bytes; the limit is " .. model.maxBytes .. ".")
        return
    end
    rp_send(command, target or "", channel or "", message)
end

function commands.s(rest) send("say", nil, nil, rest) end
function commands.say(rest) send("say", nil, nil, rest) end

function commands.em(rest) send("emote", nil, nil, rest) end
function commands.e(rest) send("emote", nil, nil, rest) end
function commands.me(rest) send("emote", nil, nil, rest) end

function commands.w(text)
    local target, message = splitFirstWord(text)
    if not target or message == "" then
        errorLine("Usage: /w <name> <message>")
        return
    end
    -- Matched case-insensitively server-side, but validating here means a typo
    -- gets an instant answer rather than a round trip.
    local name, err = validateName(target)
    if not name then
        errorLine(err)
        return
    end
    send("whisper", name, nil, message)
end
commands.whisper = commands.w
commands.tell = commands.w

function commands.p(rest) send("party", nil, nil, rest) end
function commands.g(rest) send("guild", nil, nil, rest) end
function commands.gl(rest) send("global", nil, nil, rest) end
commands.global = commands.gl

function commands.c(text)
    local channel, message = splitFirstWord(text)
    if not channel or message == "" then
        errorLine("Usage: /c <channel> <message>")
        return
    end
    local name, err = validateChannel(channel)
    if not name then
        errorLine(err)
        return
    end
    send("channel", nil, name, message)
end

function commands.who(rest)
    local query = (rest or ""):gsub("%s+", ""):lower()
    if query == "" then
        errorLine("Usage: /who <name or prefix>")
        return
    end
    rp_who(query, request(function(result)
        local results = result.results or {}
        if #results == 0 then
            notice("No profiles found for '" .. query .. "'.")
        elseif #results == 1 then
            local name = results[1].name
            -- Exactly one match: show the profile, as API.md suggests.
            apiCall("GET", "/profile/" .. name, nil, function(r)
                if not r.ok then
                    notice(name .. " has no profile yet.")
                    return
                end
                local body = r.body or {}
                notice("--- " .. name .. (body.online and " (online)" or " (offline)") .. " ---")
                local fields = body.fields or {}
                if fields.decryptFailed then
                    notice("  (profile is encrypted with a different key and cannot be read)")
                else
                    for key, value in pairs(fields) do
                        notice("  " .. key .. ": " .. tostring(value))
                    end
                end
            end)
        else
            notice(#results .. " matches:")
            for _, entry in ipairs(results) do
                notice("  " .. entry.name .. (entry.online and " (online)" or ""))
            end
        end
    end))
end

function commands.party(rest)
    local action, arg = splitFirstWord(rest or "")
    action = (action or "info"):lower()

    local function report(result)
        if result.ok == false then
            errorLine(result.error or "That did not work.")
            return
        end
        if not result.code then
            notice("You are not in a party.")
            return
        end
        notice("Party " .. result.code .. " (leader " .. (result.leader or "?") .. "): " ..
               table.concat(result.members or {}, ", "))
    end

    if action == "new" or action == "create" then
        rp_party("new", "", request(report))
    elseif action == "join" then
        if not arg or arg == "" then
            errorLine("Usage: /party join <code>")
            return
        end
        rp_party("join", arg:upper(), request(report))
    elseif action == "leave" then
        rp_party("leave", "", request(function() notice("You left the party.") end))
    else
        rp_party("info", "", request(report))
    end
end

function commands.guild(rest)
    local action, arg = splitFirstWord(rest or "")
    action = (action or ""):lower()
    local name, passcode = splitFirstWord(arg or "")

    if action == "create" then
        apiCall("POST", "/guild", json.encode({ name = name, passcode = passcode }), function(r)
            if r.ok then notice("Guild created.") else errorLine((r.body or {}).error or "Failed.") end
        end)
    elseif action == "join" then
        apiCall("POST", "/guild/" .. (name or "") .. "/join", json.encode({ passcode = passcode }), function(r)
            if r.ok then notice("Joined guild.") else errorLine((r.body or {}).error or "Failed.") end
        end)
    elseif action == "leave" then
        apiCall("POST", "/guild/leave", nil, function() notice("Left guild.") end)
    else
        errorLine("Usage: /guild create <name> <passcode> | join <name> <passcode> | leave")
    end
end

function commands.channel(rest)
    local action, name = splitFirstWord(rest or "")
    action = (action or ""):lower()
    if action == "join" and name ~= "" then
        apiCall("POST", "/channels/" .. name, nil, function(r)
            if r.ok then notice("Joined #" .. name) else errorLine("Could not join #" .. name) end
        end)
    elseif action == "leave" and name ~= "" then
        apiCall("DELETE", "/channels/" .. name, nil, function() notice("Left #" .. name) end)
    elseif action == "list" then
        apiCall("GET", "/channels", nil, function(r)
            notice("Channels: " .. table.concat((r.body or {}).channels or {}, ", "))
        end)
    else
        errorLine("Usage: /channel join <name> | leave <name> | list")
    end
end

function commands.mute(rest)
    local name = (rest or ""):gsub("%s+", ""):lower()
    if name == "" then
        apiCall("GET", "/mutes", nil, function(r)
            notice("Muted: " .. table.concat((r.body or {}).mutes or {}, ", "))
        end)
        return
    end
    apiCall("POST", "/mutes/" .. name, nil, function(r)
        if r.ok then notice("Muted " .. name .. ".") else errorLine("Could not mute " .. name) end
    end)
end

function commands.unmute(rest)
    local name = (rest or ""):gsub("%s+", ""):lower()
    if name == "" then errorLine("Usage: /unmute <name>") return end
    apiCall("DELETE", "/mutes/" .. name, nil, function() notice("Unmuted " .. name .. ".") end)
end

-- Locally cached copy of our own profile fields, so setting one field does not
-- wipe the others. The server stores the profile as opaque ciphertext now, so it
-- cannot merge fields for us — a write replaces the whole payload.
local myProfile = {}

local function loadMyProfile(afterwards)
    if model.characterName == "" then
        if afterwards then afterwards(false) end
        return
    end
    apiCall("GET", "/profile/" .. model.characterName, nil, function(r)
        if r.ok and type(r.body) == "table" then
            local fields = r.body.fields
            if type(fields) == "table" and not fields.decryptFailed then
                myProfile = fields
            elseif type(fields) == "table" and fields.decryptFailed then
                errorLine("Your stored profile could not be decrypted; it may predate your current key.")
            end
        end
        if afterwards then afterwards(true) end
    end)
end

function commands.profile(text)
    local key, value = splitFirstWord(text or "")
    if key == "" then
        errorLine("Usage: /profile <field> <value>   e.g. /profile title Wandering Tamer")
        if next(myProfile) then
            notice("Your profile:")
            for k, v in pairs(myProfile) do notice("  " .. k .. ": " .. tostring(v)) end
        end
        return
    end
    if model.characterName == "" then
        errorLine("Log in first.")
        return
    end
    -- The server rejects credential-shaped keys outright; refuse locally too so
    -- the mistake is obvious rather than a 400.
    local banned = { password = true, _password = true, token = true, salt = true, hash = true }
    if banned[key:lower()] then
        errorLine("Profiles cannot hold a field called '" .. key .. "'.")
        return
    end

    myProfile[key] = value

    local body = json.encode({ fields = myProfile })
    if not body then
        errorLine("Could not encode that profile value.")
        return
    end
    if #body > model.maxProfileBytes - 16 then
        myProfile[key] = nil
        errorLine("Profile is too large: " .. #body .. " bytes, limit " .. (model.maxProfileBytes - 16) .. ".")
        return
    end

    -- encryptBody = true. This is mandatory while the server requires
    -- encryption; a plaintext profile body is rejected outright.
    apiCall("POST", "/profile/" .. model.characterName, body, function(r)
        if r.ok then
            notice("Profile updated: " .. key)
        else
            local why = type(r.body) == "table" and r.body.error or "unknown error"
            errorLine("Could not update profile: " .. tostring(why))
        end
    end, true)
end

function commands.help()
    notice("/s /say         local, within " .. model.radius .. " units")
    notice("/em /e /me      emote, same range")
    notice("/w <name>       whisper")
    notice("/p /g /gl       party, guild, global")
    notice("/c <channel>    a channel you have joined")
    notice("/1 .. /10       channel slots from rp-config.json")
    notice("/who <name>     search profiles")
    notice("/party          new | join <code> | leave")
    notice("/guild          create | join | leave")
    notice("/channel        join | leave | list")
    notice("/mute /unmute   server-side, saves your bandwidth too")
    notice("/profile <k> <v>")
    notice("Local chat is semi-public: position is client-reported and can be faked.")
end

local function dispatch(text)
    if text == nil or text == "" then return end

    if text:sub(1, 1) ~= "/" then
        -- Bare text goes to local chat, which is what a roleplayer wants by
        -- default.
        send("say", nil, nil, text)
        return
    end

    local word, rest = splitFirstWord(text:sub(2))
    if not word then return end
    local name = word:lower()

    -- Numeric channel slots.
    local channel = slotToChannel(name)
    if channel then
        if rest == "" then
            errorLine("Usage: /" .. name .. " <message>")
        else
            send("channel", nil, channel, rest)
        end
        return
    end

    local handler = commands[name]
    if handler then
        handler(rest)
    else
        errorLine("Unknown command /" .. name .. ". Try /help.")
    end
end

-- ---------------------------------------------------------------- event pump

local function handleEvent(event, payload, requestId)
    if requestId and requestId ~= 0 and pending[requestId] then
        local handler = pending[requestId]
        pending[requestId] = nil
        local ok, err = pcall(handler, payload or {})
        if not ok then print("[RP] handler error: " .. tostring(err) .. "\n") end
        return
    end

    if event == "chat_receive" then
        renderChat(payload)
    elseif event == "chat_error" then
        local text = payload.message or "Something went wrong."
        if payload.retryAfterSeconds then
            text = text .. " (retry in " .. math.ceil(payload.retryAfterSeconds) .. "s)"
        end
        errorLine(text)
    elseif event == "ready" then
        model.characterName = payload.name or ""
        model.guild = payload.guild
        model.channels = payload.channels or {}
        model.party = payload.party
        local limits = payload.limits or {}
        model.radius = limits.localChatRadius or model.radius
        notice("Connected as " .. model.characterName .. ".")
        if model.guild then notice("Guild: " .. model.guild) end
        if #model.channels > 0 then notice("Channels: " .. table.concat(model.channels, ", ")) end
        -- Pull our own profile down so a later /profile write merges instead of
        -- replacing. The server stores it as ciphertext now and cannot merge for us.
        loadMyProfile()
    elseif event == "limits" then
        -- Emitted whenever the C++ side re-reads /info. Do not cache these
        -- anywhere else.
        model.maxBytes = payload.maxPlaintextBytes or model.maxBytes
        if payload.transmittableMessageBytes then
            -- Lower of the two: base64 inflation can make the transport buffer,
            -- not the stated limit, the real ceiling.
            model.maxBytes = math.min(model.maxBytes, payload.transmittableMessageBytes - 16)
        end
        model.maxProfileBytes = payload.maxProfileBytes or model.maxProfileBytes
        model.radius = payload.localChatRadius or model.radius
        model.minPasswordLength = payload.minPasswordLength or model.minPasswordLength
        if payload.requireEncryption ~= nil then
            model.requireEncryption = payload.requireEncryption
        end
    elseif event == "membership" then
        model.guild = payload.guild
        model.channels = payload.channels or {}
        notice("Memberships updated.")
    elseif event == "party" then
        if payload.code then
            model.party = payload.code
            notice("Party " .. payload.code .. ": " .. table.concat(payload.members or {}, ", "))
        else
            model.party = nil
            notice("You are no longer in a party.")
        end
    elseif event == "who_result" then
        -- Only arrives unsolicited if an ack was not used; our /who always acks.
        notice("Search returned " .. #(payload.results or {}) .. " results.")
    elseif event == "force_disconnect" then
        errorLine("Disconnected: " .. (payload.reason or "no reason given"))
    elseif event == "server_shutdown" then
        notice(payload.message or "Server is restarting; reconnecting shortly.")
    elseif event == "status" then
        model.status = payload.status or model.status
        rp_ui_set_status(model.status, "")
    elseif event == "log" then
        print("[RP] " .. (payload.text or "") .. "\n")
    end
end

local function pump()
    -- Text typed in the ImGui window.
    while true do
        local text = rp_ui_take_input()
        if not text then break end
        local ok, err = pcall(dispatch, text)
        if not ok then print("[RP] dispatch error: " .. tostring(err) .. "\n") end
    end

    -- Network events. Bounded per tick so a burst cannot stall a frame.
    for _ = 1, 64 do
        local event, payloadJson, requestId = rp_poll()
        if not event then break end
        local payload = {}
        if payloadJson and payloadJson ~= "" then
            local decoded, err = json.decode(payloadJson)
            if type(decoded) == "table" then
                payload = decoded
            elseif err then
                print("[RP] could not decode a " .. event .. " payload: " .. err .. "\n")
            end
        end
        local ok, err = pcall(handleEvent, event, payload, requestId)
        if not ok then print("[RP] event error: " .. tostring(err) .. "\n") end
    end
end

-- ------------------------------------------------------------------- position

-- Position is CLIENT-REPORTED. A modified client can claim to stand anywhere and
-- read local chat from across the map. That is accepted by design: fixing it
-- would require the backend to talk to the game server, which this whole
-- architecture exists to avoid. /s and /em are semi-public; whispers and
-- passcode channels are the private options.
local function samplePosition()
    local ok, err = pcall(function()
        local UEHelpers = require("UEHelpers")
        local controller = UEHelpers.GetPlayerController()
        if not controller or not controller:IsValid() then return end
        local pawn = controller.Pawn
        if not pawn or not pawn:IsValid() then return end
        local location = pawn:K2_GetActorLocation()
        rp_position(location.X, location.Y, location.Z)
    end)
    if not ok then
        -- Loading screens and menus have no pawn; that is normal, so this is a
        -- one-off notice rather than a per-tick spam.
        if not M._warnedPosition then
            M._warnedPosition = true
            print("[RP] position unavailable for now: " .. tostring(err) .. "\n")
        end
    end
end

-- ----------------------------------------------------------------- entrypoint

loadConfig()

local ok, err = rp_start(config.backendUrl, config.caFile, config.positionHz)
if not ok then
    print("[RP] could not start networking: " .. tostring(err) .. "\n")
else
    print("[RP] networking started against " .. config.backendUrl .. "\n")
end

if config.rememberPassword and config.savedName ~= "" and config.savedPassword ~= "" then
    -- Stored in the mod's own config file, in the clear. Only when the player
    -- explicitly asked for it.
    rp_login(config.savedName, config.savedPassword, false)
end

RegisterKeyBind(Key.F9, function()
    rp_ui_toggle()
end)

-- One loop, two jobs: drain queues and sample the pawn. UObject access has to
-- happen on the game thread; the queue drain does not, but doing both here keeps
-- ordering obvious.
LoopAsync(1000 // math.max(1, config.positionHz), function()
    ExecuteInGameThread(function()
        samplePosition()
        pump()
    end)
    return false
end)

notice("RP chat ready. F9 toggles the window, /help lists commands.")

M.dispatch = dispatch
M.model = model
M.config = config
M.saveConfig = saveConfig
return M
