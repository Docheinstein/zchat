# zchat

### Rules

* **Have fun**.
* Only Vibecoding/prompting is allowed.
* No functional revert: built on top of what exists.

## What it is

A tiny chat for everyone on the same local network, in a window on Windows, macOS and Linux, or in the terminal.
Type `zchat`, get a random cool name (like *Sneaky Velociraptor*), and start chatting with whoever else is running
it. No server, no accounts, no setup. Windows and terminals chat together: they are the same zchat.

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
zchat [-p PORT] [-n NAME] [--terminal]
```

| Option             | Meaning                                                |
|--------------------|--------------------------------------------------------|
| `-p`, `--port`     | UDP port of the chat room (default `47474`)            |
| `-n`, `--name`     | pick a name for this session only (not saved)          |
| `-t`, `--terminal` | chat in the terminal instead of a window               |
| `-h`, `--help`     | show help                                              |
| `-v`, `--version`  | show the version                                       |

`zchat` opens a window; `zchat --terminal`, or where there is no display (e.g. over ssh on Linux), chats in the
terminal like before. Everything works the same in both: messages, commands, pictures, emoji, games, `/update`.

The window has your avatar and name at the top left (click them to change your name, your color with a color
picker, and your avatar by uploading an image; `/nick`, `/color` and `/avatar` change them too), then the channel (the
UDP port) and the people in the chat, each with an avatar in their color,
the messages in the middle, and the input box at the bottom: Enter sends, Up and Down go through what was sent,
typing `@` lists the people to tag, pasting the path of an image turns it into `/image`, and dropping an image file
on the window types the `/image` command for it (a size or texture can still be added before Enter), or `/file` for
any other kind of file. Files sent in the chat get a Download button. The text size
(A− / A+) and font are at the top right, and are saved with the other settings. It is the system's own web view
(WebView2 on Windows, WebKit on macOS and Linux) showing `src/ui/index.html`, built into zchat, so it is ready for
more to come: real images and emoji, more channels, avatars, fonts.

While chatting:

| Input              | Action                                                 |
|--------------------|--------------------------------------------------------|
| text + Enter       | send a message                                         |
| `/who`             | list who is in the chat                                |
| `/whoami`          | show your name                                         |
| `/nick NAME`       | change your name; it is saved and used next time too   |
| `/forget`          | forget the saved name and get a new random one         |
| `/avatar FILE`     | set your avatar, which everybody sees (a GIF plays); it is saved and used next time too; `/avatar none` removes it |
| `/color`           | show your name color and the ones to pick from         |
| `/color NAME`      | change your name color (e.g. `gold`); it is saved      |
| `/color #ff8800`   | any color as a hex code (`#f80` works too), or as RGB values: `/color 255,136,0` |
| `/color random`    | switch to a random color, and forget the saved one     |
| `/tags`            | list the tags you can use in messages                  |
| `/tags NAME`       | explain a tag, with an example (`/tag NAME` works too) |
| `/image FILE`      | send a picture: windows show it as it is, terminals draw it with characters |
| `/image SIZE FILE` | the same at another size: `small`, `medium`, `large` (default, up to 480 pixels), or a width in characters like `40` |
| `/ascii FILE`      | send a picture drawn with characters (blocks of color), for everyone |
| `/ascii SIZE FILE` | the same at another size: `small`, `medium`, `large` (default), a width like `40`, or `40x20` (up to 64x32) |
| `/ascii 30% FILE`  | the same with texture: from `0%`, blocks only (the default), to `100%`, symbols only; with a size too, in any order (`/ascii small 30% FILE`); `/image 30% FILE` does the same |
| `/addemoji NAME [SIZE] [TEXTURE%] FILE` | save a picture as an emoji, drawn as `/image` would; the same name again replaces it |
| `/emoji NAME`      | send a saved emoji; `/emoji` alone lists yours          |
| `/removeemoji NAME` | delete a saved emoji                                  |
| drop an image      | drag an image file onto the window: the line becomes `/image PATH`, to send with Enter (or add a size first) |
| `/file FILE`       | send a file of any kind (up to about 34 MB), for everyone to download |
| drop a file        | drag any other file onto the window (or a terminal): the line becomes `/file PATH`, to send with Enter |
| `/save N`          | save file N of the chat in your downloads folder; `/save` alone lists them (windows have a Download button) |
| `@NAME`            | tag someone in a message: they hear a sound (type `@` to pick from the list) |
| `@everyone`        | tag all the people in the chat: they all hear a sound  |
| `/game`            | list the games everyone in the chat can play           |
| `/game race`       | typing race: some words show up for everyone in 3 seconds, the first to type them exactly wins |
| `/game dice`       | push your luck: `/game dice roll` a die as often as you dare, each roll adds to points only you see, but a 1 loses them all; `/game dice stop` keeps them and shows them to everyone. The highest points kept in 60 seconds win; not stopped in time, and they are lost |
| `/game paint`      | a shared canvas of 16×16 squares: `/game paint c7 red` colors a square, `/game paint c7-f9 blue` a rectangle, `/game paint c7 none` empties it and `/game paint undo` takes back your last change. Without a color, squares get your brush's: `/game paint color red` picks it, and it starts as the canvas color closest to your name color. Each square painted takes a second before the next change, so the chat has to draw together |
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

`/image` sends a real picture, the file as it is (PNG, JPEG, GIF and BMP files up to about 36 MB), so it keeps all
its quality. Windows show it at most 480 pixels wide and tall (never bigger than it is; a size makes it smaller),
and a click opens it big, at its full size. Terminals, which cannot show pictures, draw it with characters, as
`/ascii` would. Other kinds of file (TGA, PSD, PNM) and bigger ones are sent as a JPEG of good quality instead, up to
1280 pixels. Versions from before real pictures show nothing.

Animated GIFs play in windows at their own speed and full quality, as any GIF does, since the file is sent as it is.
One too big for that is sent as JPEG frames, all of them, as big and good as fits; one too long to decode (over 64
million pixels in all its frames) is then sent as its first frame, and zchat says so. Terminals draw the first frame.

A picture bigger than one packet (about 45 KB) is downloaded by each of the others straight from the sender, over
TCP, as fast as the network goes: broadcasts are no good for big data (Wi-Fi sends them at its slowest, and never
again when they are lost). Whoever cannot download it (e.g. a firewall in the way) gets it in pieces instead, and
asks again for the ones lost, so it still arrives whole, only slower. Versions from before this only see pictures of
up to about 45 KB.

`/file` sends a file of any kind, as it is, up to about 34 MB, the same way as a big picture. Whoever gets it sees its
name and size: windows with a Download button, terminals with the `/save N` command that saves it. It is saved in
the downloads folder (or the home folder, without one), with ` (2)`, ` (3)`... in the name when one of that name is
there already. Until then, received files are kept in a temporary folder of zchat's own, deleted when it quits. As
with pictures, dropping a file on a terminal types its full path, which then sends it. Names are cleaned up, so a
file can never be saved outside the downloads folder. Versions from before files show nothing.

`/ascii` sends a picture as colored ASCII art, at most 64 characters wide and 32 lines tall: brighter parts are drawn
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
fonts, to fill exactly their part of the character. To keep lines crisp, each character also tries its two main
colors (so a white stays white next to a black outline instead of both turning grey), and candidates are compared
by the absolute difference of their colors, which does not favor averaging over sharpness.

A picture can also get texture, with `/image N% FILE`: its dark parts are then drawn as text, with ASCII symbols
shown as themselves, on black (`. : - + = * % # @`, and `# W @` in bold, which have more ink), the one with enough
ink for their light, in the color that keeps it. At `0%`, the default, there is none. As it grows, more and more of
the dark range becomes text, from the darkest up: at about `15%` the shadows of a drawing (characters that are mostly
black, with a few lit bits), then also the smooth dim parts of a photo, and at `100%` all that symbols can show,
up to the brightness of `@` in bold. What is brighter always keeps its blocks, which draw corners and thin lines
that symbols cannot, and so does the bright side of an edge. How dark a character is is how dark it looks (a deep
red is dark), and one that symbols could not show bright enough stays blocks. In a picture that is dark all over,
less of it becomes text. In the art sent, `"` before a symbol makes it texture (shown as itself, not as a block),
and `!` switches bold on and off for texture symbols.

Each picture comes with its own palette, the 61 colors that represent it best (found with k-means), in a first
row: `$` followed by 3 characters per color (one per channel, 64 levels each). The color codes of the drawing
refer to it, so skin, the white of an eye or a sky get their own colors instead of the closest of fixed hues.

A line with a lot of detail that would be too long for a packet keeps the blocks of its clearest details only, and
at worst is drawn with one color per character, or as the plain ASCII art. Versions from before this show the codes
as they are, and may not receive the biggest pictures at all (they took at most 8 KB).

Images are decoded with [stb_image](https://github.com/nothings/stb) (public domain / MIT), in `third_party/stb`.

The name set with `/nick` is saved in the `zchat` config folder: `%APPDATA%\zchat\config` on Windows,
`~/.config/zchat/config` on Linux (or `$XDG_CONFIG_HOME/zchat/config`). On start, `--name` wins over the saved
name, which wins over a random one. The color set with `/color` is saved there too.

The last 50 chat messages, sent and received, are kept in the `history` file of the config folder, with the time
they were sent at, and shown again when zchat starts, before the welcome (with their date when they are not from
today). Pictures, joins and other notices are not kept. The file is plain text, one message per line: delete it to
forget them.

Emoji are saved in the `emoji` folder of the config folder (`%APPDATA%\zchat\emoji` on Windows), one `NAME.art`
file each: the picture already drawn, as it is sent, so `/emoji NAME` sends it right away and the same every time,
even if the image file is gone. Names are letters, digits, `-` and `_`, up to 32, and are not case-sensitive.

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

Needs CMake 3.16+ and a C++23 compiler (GCC 13+, Clang 17+, or Visual Studio 2022), and an internet connection the
first time: CMake downloads the [webview](https://github.com/webview/webview) library for the window (and, on
Windows, Microsoft's WebView2 SDK).

For the window:

* **Windows** (10 and 11): nothing more, WebView2 comes with Windows.
* **macOS**: nothing more, WebKit comes with macOS.
* **Linux**: GTK and WebKitGTK, e.g. `sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev` (Debian, Ubuntu) or
  `sudo dnf install gtk3-devel webkit2gtk4.1-devel` (Fedora). Without them, CMake says so and builds zchat for the
  terminal only. `-DZCHAT_GUI=OFF` does the same on purpose.

With [just](https://github.com/casey/just) installed, the shortest way is:

```sh
just build          # configure + build (Release)
just run            # build and start zchat, extra args are passed on: just run -n Rex
just run-terminal   # the same, in the terminal
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

(The window has only been built with Visual Studio so far: if a MinGW build of it fails, add `-DZCHAT_GUI=OFF` for
the terminal version.)

## How it works

* Every instance binds UDP port 47474 (shared, so several instances can run on the same machine) and **broadcasts**
  its packets to `255.255.255.255` and to the broadcast address of each network interface. Packets that arrive more
  than once this way are dropped by sequence number. The pieces of big pictures are sent once per network instead
  (to the broadcast address of each interface only), since they are a lot of data.
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
  happens in it as `GAME` packets, whose text starts with the game's name (`race go <round> <words>`). In the race
  the referee picks the words, and the first message with them that reaches it wins. One letter of each
  word is shown as a Cyrillic lookalike (`а` for `a`), so the words cannot be copied and pasted: typed they match,
  pasted they do not, and whoever pasted them is out of the round. If two rounds start at
  once, everybody plays the one with the lowest round number; if the referee leaves, the others give up the round
  after a while. In dice everyone rolls their own die, and tells the others when they join, stop (with their
  points) or roll a 1; every zchat ends the round when time is up, or when all the players are done and nobody
  can join anymore (after 15 seconds), and shows who won. In paint every zchat keeps its own copy of the canvas: each change of a
  square carries a clock and a tag from the painter's id, and a square keeps the newest change, so changes lost or
  heard out of order do not matter. Every 15 seconds each zchat sends a hash of its canvas, and whoever has a
  different one sends the changes it has, so newcomers and whoever missed a packet catch up. Older versions ignore `GAME` packets, so they just see people typing funny words.
* **Avatars** are square pictures (a still image cropped to its middle, 256 pixels; a GIF as it is up to 2 MB, or
  else as 128-pixel frames), saved in the config folder as `avatar`. Heartbeats (`JOIN` and `HERE`) carry
  `avatar <hash> <bytes> <TCP port>`, and whoever does not have that avatar yet downloads it from the sender with
  `AVATAR <hash>`, as for big pictures; so newcomers get everyone's within a heartbeat, and a new one shows right
  away. Terminals do not show avatars.
* **Pictures** bigger than a packet are offered with an `OFFER` packet (`<picture id> <bytes> <pieces> <TCP port>`).
  Every zchat listens on a TCP port the system picks; whoever gets an offer connects to the address it came from,
  sends `GET <picture id>`, and reads the picture (scrambled like packets) until the sender closes the connection.
  If that fails, it asks for the picture in 8 KB pieces instead, with a `RESEND` packet (`<sender id> <picture id>
  <index> ...`), and gets them as `CHUNK` packets (`<picture id> <index> <count>`, then a piece of the picture's
  text); it asks again for the ones missing when they stop coming, waiting twice as long each time it gets nothing.
  The sender keeps its last pictures for two minutes. A picture whose pieces stop coming for 20 seconds is given up
  on, and zchat says so.
* Incoming names and messages are sanitized (control characters stripped) so nobody can mess with your terminal.
* The input line is edited in raw mode, so incoming messages are printed above what you are typing instead of
  getting mixed with it.

Wire format, one datagram per packet (fields separated by `\n`):

```
ZCHAT1 \n <J|H|M|L|P|G|I|O|K|R> \n <sender id, hex> \n <sequence number> \n <name> \n <text>
```

which is sent scrambled (see `src/cipher.hpp`):

```
ZX1 <4 byte nonce> <scrambled bytes of the packet above>
```

## Troubleshooting

* **Nobody shows up**: the firewall must allow incoming UDP on port 47474. Windows asks the first time you run
  zchat: allow it on *private* networks. On Linux, e.g. `sudo ufw allow 47474/udp`.
* **Big pictures arrive slowly**: they are downloaded over TCP, on a port the system picks, so the firewall must let
  zchat itself accept connections (the rule Windows makes when you allow zchat does); without that they still
  arrive, in pieces, much more slowly.
* Broadcasts do not cross routers, and some Wi-Fi networks (guest networks, "client isolation") block traffic
  between devices.
* **Dropping a picture does nothing** (Linux, Terminator 2.1.3): Terminator itself fails on every drop, in any
  program (`'bytes' object has no attribute 'encode'` in its log). Until it is updated, this plugin fixes it:
  `mkdir -p ~/.config/terminator/plugins && cp extras/terminator_fix_drop.py ~/.config/terminator/plugins/`,
  then restart Terminator (it needs no enabling). `/image FILE` works anyway.
