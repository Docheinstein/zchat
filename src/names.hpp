#pragma once

#include <random>
#include <string>

namespace zchat {

// Returns a random cool name such as "Sneaky Velociraptor".
std::string random_name(std::mt19937_64& rng);

} // namespace zchat
