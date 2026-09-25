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
| `-n`, `--name`     | pick your own name instead of a random one             |
| `-h`, `--help`     | show help                                              |
| `-v`, `--version`  | show the version                                       |

While chatting:

| Input              | Action                                                 |
|--------------------|--------------------------------------------------------|
| text + Enter       | send a message                                         |
| `/who`             | list who is in the chat                                |
| `/help`            | list commands                                          |
| `/quit`, Ctrl+C, Ctrl+D | leave                                             |
| `//text`           | send a message that starts with `/`                    |
| Ctrl+U / Ctrl+W    | clear the line / delete the last word                  |

People using a different `--port` are in a different room.

## Building

Needs CMake 3.16+ and a C++23 compiler (GCC 13+, Clang 17+, or Visual Studio 2022).

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
