// The changelog built into the executable, apart from changelog.cpp so that the tests need no generated header.

#include "changelog.hpp"

#include "changelog_md.hpp"

namespace zchat::changelog {

std::string_view text() {
    return {reinterpret_cast<const char*>(changelog_md), changelog_md_size};
}

} // namespace zchat::changelog
