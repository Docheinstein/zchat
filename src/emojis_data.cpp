// The emoji built into zchat (see emojis::all()): apart from emojis.cpp, so that the tests can do without them.

#include "emojis.hpp"

#include "gemoji_json.hpp"

namespace zchat::emojis {

const std::vector<Emoji>& all() {
    static const std::vector<Emoji> table =
        parse(std::string_view(reinterpret_cast<const char*>(gemoji_json), gemoji_json_size));
    return table;
}

} // namespace zchat::emojis
