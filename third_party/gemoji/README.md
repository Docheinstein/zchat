# gemoji, the emoji and their names

`emoji.json` is GitHub's list of the Unicode emoji (`db/emoji.json` of [gemoji](https://github.com/github/gemoji),
version 4.1.0): each emoji with its description, category, aliases (the `:shortcodes:`, like `:rofl:` and `:+1:`) and
tags, which zchat's emoji picker and shortcodes use (see `src/emojis.hpp`). MIT licensed, see `LICENSE`.

Got with: `curl -L -o emoji.json https://raw.githubusercontent.com/github/gemoji/v4.1.0/db/emoji.json`, as it is.
