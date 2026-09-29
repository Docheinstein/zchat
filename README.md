# zchat

### Rules

* **Have fun**.
* Only Vibecoding/prompting is allowed.
* No functional revert: built on top of what exists.

## What it is

A tiny command line chat for everyone on the same local network, for Linux and Windows.
Type `zchat`, get a random cool name (like *Sneaky Velociraptor*), and start chatting with whoever else is running
it. No server, no accounts, no setup.

```
23:40 * Welcome to zchat! You are Mystic Iguanodon.
23:40 * Chatting on UDP port 47474. Type a message and press Enter, /help for commands.
23:40 * Cranky Brontosaurus is here
23:41 Cranky Brontosaurus: anyone up for lunch?
23:41 Mystic Iguanodon: always 🦖
> _
```

## Usage

```
zchat [-p PORT] [-n NAME]
```

| Option             | Meaning                                                |
|--------------------|--------------------------------------------------------|
| `-p`, `--port`     | UDP port of the chat room (default `47474`)            |
| `-n`, `--name`     | pick a name for this session only (not saved)          |
| `-h`, `--help`     | show help                                              |
| `-v`, `--version`  | show the version                                       |

While chatting:

| Input              | Action                                                 |
|--------------------|--------------------------------------------------------|
| text + Enter       | send a message                                         |
| `/who`             | list who is in the chat                                |
| `/whoami`          | show your name                                         |
| `/nick NAME`       | change your name; it is saved and used next time too   |
| `/forget`          | forget the saved name and get a new random one         |
| `/color`           | show your name color and the ones to pick from         |
| `/color NAME`      | change your name color (e.g. `gold`); it is saved      |
| `/color #ff8800`   | any color as a hex code (`#f80` works too), or as RGB values: `/color 255,136,0` |
| `/color random`    | switch to a random color, and forget the saved one     |
| `/tags`            | list the tags you can use in messages                  |
| `/tags NAME`       | explain a tag, with an example (`/tag NAME` works too) |
| `/image FILE`      | send a picture, drawn with blocks of color               |
| `/image SIZE FILE` | the same at another size: `small`, `medium`, `large` (default), a width like `40`, or `40x20` (up to 64x32) |
| drop an image      | drag an image file onto the window: the line becomes `/image PATH`, to send with Enter (or add a size first) |
| `@NAME`            | tag someone in a message: they hear a sound (type `@` to pick from the list) |
| `@everyone`        | tag all the people in the chat: they all hear a sound  |
| `/game`            | list the games everyone in the chat can play           |
| `/game finger`     | fastest finger: some words show up for everyone in 3 seconds, the first to type them exactly wins |
| `/game scores`     | who won what in this session                           |
| `/update`          | get the latest zchat from git, build it and restart (see below) |
| `/help`            | list commands                                          |
| `/quit`, Ctrl+C, Ctrl+D | leave                                             |
| `//text`           | send a message that starts with `/`                    |
| Left / Right       | move the cursor in the line                            |
| Home / End         | go to the start / end of the line (or Ctrl+A / Ctrl+E) |
| Backspace / Delete | delete before / under the cursor                       |
| Ctrl+U / Ctrl+W    | clear the line / delete the word before the cursor     |
| Up / Down          | go through the messages and commands sent this session |

People using a different `--port` are in a different room.

Messages can contain tags, written like HTML: they are not shown, and affect the text after them until they are
closed or the message ends:

| Tag                | Effect                                                   |
|--------------------|----------------------------------------------------------|
| `<color=...>`      | colors the text: a color name, a hex code or RGB values  |
| `<bold>`, `<b>`    | bold text; `<bold=2>` and `<bold=3>` are even bolder (and brighter) |
| `<italic>`, `<i>`  | italic text (not shown by the old Windows console)       |
| `<underscore>`, `<u>` | underlined text                                       |
| `<strikethrough>`, `<s>` | struck through text (not shown by the old Windows console) |
| `<size=...>`       | `tiny` (or `1`), `small` (`2`), `normal` (`3`) or `large` (`4`) text, drawn with Unicode letters: ᵗⁱⁿʸ, ꜱᴍᴀʟʟ, Ｌａｒｇｅ |

```
this is a <color=red>test</color>, <color=#ff8800>orange <color=0,200,255>blue</color> orange again</color>
this is <bold>important</bold>, <italic>so <bold>very</bold> nice</italic>, <b=3>urgent!</b>
read <u>this</u> first, the meeting is on <s>Monday</s> Tuesday
```

Anything that is not a valid tag is shown as typed. Tags are applied by whoever receives the message, so older
zchat versions show them as plain text.

Pictures are sent as colored ASCII art, at most 64 characters wide and 32 lines tall: brighter parts are drawn
with more ink, in the color of the picture. They are shown as solid blocks of color, as dark or bright as the
picture there, so they look the same in every terminal and font (it looks best on a dark terminal; without colors,
they are shown as the black and white ASCII art). PNG, JPEG, GIF, BMP, TGA, PSD and PNM files can be sent. Dropping
a file on a terminal types its path, so as soon as the input line holds nothing but the path of an image file,
however the terminal writes it (plain, in quotes, with `\ ` escapes, or as a `file://` URL), it turns into the
`/image` command for it. A line like that sent anyway (e.g. piped in) also sends the picture. Older zchat versions
don't show pictures.

To be closer to the picture, the art also carries a brightness for its characters (`()[]{}<>^~;?`, from the dimmest
to full, 12 levels, and `` ` `` for black: dark parts stay dark instead of being drawn in full-strength colors), and
characters can have two colors: a background (`&` then a color and a brightness code, `|` for none) and the color of
a block drawn over it. Each character is looked at as 8 x 8 points, and gets the block, among the halves (`▀▄▌▐`),
the eighths (`▁▂▃▅▆▇ ▔`, `▏▎▍▋▊▉ ▕`) and the quadrants (`▘▝▖▗▚▞▛▜▙▟`), and the two colors that look the most
like it: edges fall where they are to an eighth of a character, and thin dark outlines (eyes, teeth, the lines of
a drawing) are kept instead of being averaged away. These blocks are drawn by Windows Terminal itself, and by most
fonts, to fill exactly their part of the character. A line that would be too long for a packet is drawn with one
color per character, or as the plain ASCII art. Versions from before this show the codes as they are.

Images are decoded with [stb_image](https://github.com/nothings/stb) (public domain / MIT), in `third_party/stb`.

The name set with `/nick` is saved in the `zchat` config folder: `%APPDATA%\zchat\config` on Windows,
`~/.config/zchat/config` on Linux (or `$XDG_CONFIG_HOME/zchat/config`). On start, `--name` wins over the saved
name, which wins over a random one. The color set with `/color` is saved there too.

### Updating

`/update` works when zchat was built from a git clone (with `just build` or `just install`): it remembers the
source and build folders it was built from. While you keep chatting, it fetches from the remote of the current
branch, fast-forwards to the new commits, rebuilds, replaces the zchat you are running (the one in the build
folder, or the installed copy) and restarts it with the same options. The others see you leave and come back.

Before building, it lists what's new since the zchat you are running: the subjects of the new commits, newest
first. There is no separate changelog, so commit subjects are written for the people chatting, e.g.
`Added @everyone: tags all the people in the chat`.

It needs git, CMake and the compiler used for the first build. It changes nothing when the clone has local
commits or uncommitted changes, or when the build fails: the build errors are shown and the running zchat is
kept. The remote must not ask for a password (use an SSH key or a credential helper).

## Building

Needs CMake 3.16+ and a C++23 compiler (GCC 13+, Clang 17+, or Visual Studio 2022).

With [just](https://github.com/casey/just) installed, the shortest way is:

```sh
just build          # configure + build (Release)
just run            # build and start zchat, extra args are passed on: just run -n Rex
just install        # install into ~/.local/bin (or: just install /some/prefix)
just clean          # remove the build directory
```

Or by hand:

```sh
cmake -S . -B build
cmake --build build
./build/zchat
```

To be able to just type `zchat`, install it on your `PATH`, e.g. `cmake --install build --prefix ~/.local`
(with `~/.local/bin` on your `PATH`).

On Windows, the same commands work from a Developer Command Prompt (the binary ends up in `build\Release\zchat.exe`
or `build\zchat.exe` depending on the generator). MinGW builds are linked statically, so the `.exe` can be copied
anywhere on its own.

To cross-compile the Windows version from Linux, e.g. with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```sh
cmake -S . -B build-win -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++
cmake --build build-win
```

## How it works

* Every instance binds UDP port 47474 (shared, so several instances can run on the same machine) and **broadcasts**
  its packets to `255.255.255.255` and to the broadcast address of each network interface. Packets that arrive more
  than once this way are dropped by sequence number.
* On start a peer sends `JOIN`; the others answer with `HERE`, so the newcomer learns who is around. Everybody sends
  `HERE` every 5 seconds as a heartbeat; a peer silent for 16 seconds is considered gone. Quitting sends `LEAVE`.
* A name's color is `sender id % 12`, so choosing a color means picking a new random id that maps to it. A custom
  RGB color is stored in the id itself: top 16 bits `c01d`, then the 24-bit color, then random bits chosen so that
  `id % 12` is still the closest named color for older versions. Colors are drawn with 24-bit ANSI codes, so they
  look the same in every terminal theme. On a color
  change the `LEAVE` of the old id carries the new id as its text, so the others move the peer over instead of
  showing it leave and join (older versions still show a leave and a join).
* Packets are **scrambled** before they are sent, so a packet sniffer does not show the chat in plaintext: each
  byte is XORed with a keystream (splitmix64, seeded by a key built into zchat and 4 random bytes sent with the
  packet) and added to the previous scrambled byte, so the same message looks different every time. This is
  obfuscation, not real encryption: anyone with zchat, or this code, can read the packets. Plaintext packets from
  older versions are still understood, but older versions cannot read the scrambled ones, so they don't see newer
  peers: everybody should `/update`.
* **Games** (`/game NAME`) have no server either: whoever starts a round is its referee, and sends everybody what
  happens in it as `GAME` packets, whose text starts with the game's name (`finger go <round> <words>`). In fastest
  finger the referee picks the words, and the first message with them that reaches it wins. One letter of each
  word is shown as a Cyrillic lookalike (`а` for `a`), so the words cannot be copied and pasted: typed they match,
  pasted they do not, and whoever pasted them is out of the round. If two rounds start at
  once, everybody plays the one with the lowest round number; if the referee leaves, the others give up the round
  after a while. Older versions ignore `GAME` packets, so they just see people typing funny words.
* Incoming names and messages are sanitized (control characters stripped) so nobody can mess with your terminal.
* The input line is edited in raw mode, so incoming messages are printed above what you are typing instead of
  getting mixed with it.

Wire format, one datagram per packet (fields separated by `\n`):

```
ZCHAT1 \n <J|H|M|L|P|G> \n <sender id, hex> \n <sequence number> \n <name> \n <text>
```

which is sent scrambled (see `src/cipher.hpp`):

```
ZX1 <4 byte nonce> <scrambled bytes of the packet above>
```

## Troubleshooting

* **Nobody shows up**: the firewall must allow incoming UDP on port 47474. Windows asks the first time you run
  zchat: allow it on *private* networks. On Linux, e.g. `sudo ufw allow 47474/udp`.
* Broadcasts do not cross routers, and some Wi-Fi networks (guest networks, "client isolation") block traffic
  between devices.
* **Dropping a picture does nothing** (Linux, Terminator 2.1.3): Terminator itself fails on every drop, in any
  program (`'bytes' object has no attribute 'encode'` in its log). Until it is updated, this plugin fixes it:
  `mkdir -p ~/.config/terminator/plugins && cp extras/terminator_fix_drop.py ~/.config/terminator/plugins/`,
  then restart Terminator (it needs no enabling). `/image FILE` works anyway.
