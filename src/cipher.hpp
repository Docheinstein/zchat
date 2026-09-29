#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zchat::cipher {

// A light scrambling of the datagrams, so chat lines do not travel as plaintext that anyone running a packet sniffer
// can read at a glance. It is NOT real encryption: the key is built into zchat, so any zchat (or anyone reading
// this code) can unscramble them.
//
// Scrambled datagram: "ZX1" + 4 random nonce bytes + body, where each body byte is the plain byte XORed with a
// keystream (splitmix64, seeded by the key and the nonce) and then added to the previous scrambled byte, so the same
// message never looks the same twice and one changed byte garbles the rest.

std::string scramble(std::string_view plain);

// Returns nullopt for anything that is not a scrambled datagram (e.g. a plaintext one from an older version).
std::optional<std::string> unscramble(std::string_view data);

} // namespace zchat::cipher
