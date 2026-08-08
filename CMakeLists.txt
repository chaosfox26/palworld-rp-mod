# CMakeLists.txt — build for the UE4SS C++ mod.
#
# UE4SS's documented default build system is CMake (>= 3.22); xmake is being
# phased out upstream and a mod built with xmake risks ABI mismatch against a
# CMake-built UE4SS, which is what shipped releases now are. See CLAUDE.md.
#
# This file is included via add_subdirectory() from a small top-level project
# that CI generates at build time (it also add_subdirectory()s a checkout of
# RE-UE4SS itself, sibling to this one) — see the "creating-a-c++-mod.md"
# guide in the UE4SS docs. It is not meant to be configured standalone.
#
# ixwebsocket and nlohmann_json come from vcpkg, not FetchContent: vcpkg is
# already provisioned on GitHub's Windows runners and its ixwebsocket port
# already selects mbedTLS as the default SSL backend on Windows.

set(TARGET PalworldRpChat)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/net.cpp
)

target_include_directories(${TARGET} PRIVATE src)
target_compile_features(${TARGET} PRIVATE cxx_std_17)

# This is UE4SS's own CMake target. Linking against it PUBLICly is what pulls
# in its include dirs (Mod/CppUserModBase.hpp, imgui.h, LuaType/*, ...) and
# gives xmake.lua's old "ue4ss.mod" rule's DLL naming/output-path behavior for
# free, since it is UE4SS's own build tree doing it.
target_link_libraries(${TARGET} PUBLIC UE4SS)

find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(${TARGET} PRIVATE ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json)

# bcrypt: AES-256-GCM and BCryptGenRandom.
# crypt32: ixwebsocket reads the Windows root store through it.
# ws2_32:  sockets.
target_link_libraries(${TARGET} PRIVATE bcrypt crypt32 ws2_32)

# Unicode, and keep <windows.h> from defining min/max macros that break
# std::min / std::max in the standard headers.
target_compile_definitions(${TARGET} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    target_compile_definitions(${TARGET} PRIVATE RP_DEBUG)
endif()
