-- xmake.lua — build for the UE4SS C++ mod.
--
-- UE4SS builds with xmake, so using it here means one toolchain rather than two
-- and no vcpkg manifest to keep in sync.
--
-- HOW TO BUILD
--   1. Clone RE-UE4SS and build it once, so its own dependencies are resolved.
--   2. Copy this whole folder into the UE4SS tree's C++ mods directory.
--   3. From the UE4SS root:  xmake f -m Game__Shipping__Win64 && xmake
--
-- VERIFY BEFORE TRUSTING THIS FILE: the rule name below (`ue4ss.mod`) and the
-- mods directory have both changed across UE4SS releases. If xmake reports an
-- unknown rule, copy the xmake.lua from a working C++ mod template in the UE4SS
-- version you actually ship and paste the target body from here into it. The
-- deps and syslinks are the part that matters; the scaffolding around them is
-- UE4SS's business.

set_languages("c++17")

-- ixwebsocket gives us WebSocket framing, reconnect and an HTTP client that
-- shares the same TLS stack, so REST and the socket cannot disagree about
-- certificate trust.
--
-- mbedTLS is its default Windows backend. See the TLS note in README.md for what
-- it does and does not let us configure.
add_requires("ixwebsocket", { configs = { ssl = "mbedtls", zlib = false } })
add_requires("nlohmann_json")

target("PalworldRpChat")
    -- This rule comes from UE4SS. It sets the DLL naming, output path and the
    -- Game__Shipping__Win64 style build modes UE4SS expects.
    add_rules("ue4ss.mod")

    add_files("src/dllmain.cpp")
    add_files("src/net.cpp")
    add_includedirs("src")

    add_packages("ixwebsocket", "nlohmann_json")

    -- bcrypt: AES-256-GCM and BCryptGenRandom.
    -- crypt32: ixwebsocket reads the Windows root store through it.
    -- ws2_32:  sockets.
    add_syslinks("bcrypt", "crypt32", "ws2_32")

    -- Unicode, and keep <windows.h> from defining min/max macros that break
    -- std::min / std::max in the standard headers.
    add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")

    if is_mode("debug") then
        add_defines("RP_DEBUG")
    end
target_end()

-- -----------------------------------------------------------------------------
-- Host-side tests. These build and run anywhere, including CI on Linux, because
-- b64.hpp and sio.hpp deliberately have no platform or third-party dependencies.
-- They are the two files where a bug is silent rather than loud.
--
--   xmake f -m release && xmake build test_protocol && xmake run test_protocol

target("test_protocol")
    set_kind("binary")
    set_default(false)
    add_files("tests/test_protocol.cpp")
    add_includedirs("src")
target_end()

target("test_envelope")
    set_kind("binary")
    set_default(false)
    add_files("tests/test_envelope.cpp")
    add_includedirs("src")
target_end()

-- Windows only: exercises the BCrypt path without needing UE4SS or Palworld.
-- Pipe it through tests/envelope_interop.js to check interop with Node in both
-- directions. This is the test that catches an unreadable-chat bug before players
-- see it.
if is_plat("windows") then
    target("test_crypto_win")
        set_kind("binary")
        set_default(false)
        add_files("tests/test_crypto_win.cpp")
        add_includedirs("src")
        add_syslinks("bcrypt")
    target_end()
end
