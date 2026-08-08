// key.h — the baked-in application key.
//
// Generate it on the backend:
//
//     npx -y github:chaosfox26/palworld-rp-backend keygen
//
// and paste the 32 bytes below. Everyone who plays on your server needs a build
// with the SAME key; a mismatch shows up as "Message failed its authentication
// check" on every incoming line, which is the failure mode to look for if half
// your players can read each other and half cannot.
//
// WHAT THIS KEY DOES AND DOES NOT DO
// ----------------------------------
// It stops YOU, the host, from reading chat off your own disk, out of your own
// logs, or out of a backup. That is a real property and it is the reason the
// envelope exists.
//
// It does not protect players from each other. Every player has the mod, so
// every player has this key; anyone willing to open the DLL can extract it and
// decrypt anything they can capture. This is not end-to-end encryption, and the
// backend's ENCRYPTION.md says so in as many words. Do not describe it that way
// in the mod's description either.
//
// Keeping it out of the Lua files is still worth doing: it means another Lua mod
// running in the same process cannot read it out of a shared Lua state, and it
// keeps the key out of any text file a player might paste into a bug report.

#pragma once

#include <cstddef>

namespace rp
{
    // ==========================================================================
    // THIS IS A REAL KEY. THE REPOSITORY MUST STAY PRIVATE.
    //
    // Making the repo public means regenerating the key, rebuilding, and every
    // player reinstalling the mod. Git remembers, so removing it in a later
    // commit does not help.
    //
    // Installed from `palworld-rp keygen`, verified against the printed hex:
    //   8ba6d3f4c7153120dd3ebbe01820688144988e9800fbe1ce91ebcd45460bcd4d
    // ==========================================================================
    //
    // Exactly 32 bytes. AES-256.
    inline constexpr unsigned char kAppKey[32] = {
        0x8B, 0xA6, 0xD3, 0xF4, 0xC7, 0x15, 0x31, 0x20,
        0xDD, 0x3E, 0xBB, 0xE0, 0x18, 0x20, 0x68, 0x81,
        0x44, 0x98, 0x8E, 0x98, 0x00, 0xFB, 0xE1, 0xCE,
        0x91, 0xEB, 0xCD, 0x45, 0x46, 0x0B, 0xCD, 0x4D,
    };

    // Catches the failure that is otherwise invisible: a build that shipped with
    // the placeholder still in place would encrypt happily and be readable by
    // anyone who guessed the obvious key.
    static_assert(kAppKey[0] != 0x00 || kAppKey[1] != 0x00,
                  "The application key is still the all-zero placeholder.");

    static_assert(sizeof(kAppKey) == 32, "The application key must be exactly 32 bytes.");
} // namespace rp
