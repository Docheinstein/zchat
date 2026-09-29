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
| `/nick NAME`       | change your name; it is saved and used next time too   |
| `/forget`          | forget the saved name and get a new random one         |
| `/color`           | show your name color and the ones to pick from         |
| `/color NAME`      | change your name color (e.g. `gold`); it is saved      |
| `/color #ff8800`   | any color as a hex code (`#f80` works too), or as RGB values: `/color 255,136,0` |
| `/color random`    | switch to a random color, and forget the saved one     |
| `/tags`            | list the tags you can use in messages                  |
| `/tags NAME`       | explain a tag, with an example (`/tag NAME` works too) |
| `/image FILE`      | send a picture, drawn with colored characters (ASCII art) |
| `/image SIZE FILE` | the same at another size: `small`, `medium`, `large` (default), a width like `40`, or `40x20` (up to 64x32) |
| drop an image      | drag an image file onto the window, then press Enter: same as `/image` |
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

```
this is a <color=red>test</color>, <color=#ff8800>orange <color=0,200,255>blue</color> orange again</color>
this is <bold>important</bold>, <italic>so <bold>very</bold> nice</italic>, <b=3>urgent!</b>
read <u>this</u> first, the meeting is on <s>Monday</s> Tuesday
```

Anything that is not a valid tag is shown as typed. Tags are applied by whoever receives the message, so older
zchat versions show them as plain text.

Pictures are sent as colored ASCII art, at most 64 characters wide and 32 lines tall: brighter parts are drawn
with more ink, in the color of the picture (it looks best on a dark terminal; without colors, it is black and
white). PNG, JPEG, GIF, BMP, TGA, PSD and PNM files can be sent. Dropping a file on a terminal types its path, so a
line holding nothing but the path of an image file sends the picture, however the terminal writes it: plain, in
quotes, with `\ ` escapes, or as a `file://` URL. Older zchat versions don't show pictures.

Images are decoded with [stb_image](https://github.com/nothings/stb) (public domain / MIT), in `third_party/stb`.

The name set with `/nick` is saved in the `zchat` config folder: `%APPDATA%\zchat\config` on Windows,
`~/.config/zchat/config` on Linux (or `$XDG_CONFIG_HOME/zchat/config`). On start, `--name` wins over the saved
name, which wins over a random one. The color set with `/color` is saved there too.

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
* Incoming names and messages are sanitized (control characters stripped) so nobody can mess with your terminal.
* The input line is edited in raw mode, so incoming messages are printed above what you are typing instead of
  getting mixed with it.

Wire format, one datagram per packet (fields separated by `\n`):

```
ZCHAT1 \n <J|H|M|L> \n <sender id, hex> \n <sequence number> \n <name> \n <text>
```

## Troubleshooting

* **Nobody shows up**: the firewall must allow incoming UDP on port 47474. Windows asks the first time you run
  zchat: allow it on *private* networks. On Linux, e.g. `sudo ufw allow 47474/udp`.
* Broadcasts do not cross routers, and some Wi-Fi networks (guest networks, "client isolation") block traffic
  between devices.
