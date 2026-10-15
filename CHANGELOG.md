# Changelog

What is new in zchat, newest first, a day at a time. zchat shows it too: `/changelog`, or 📜 at the top of the
window, and after an update it says once what is new since you last looked.

Each day is a `## YYYY-MM-DD` heading and each change a `* ` item under it (lines starting with spaces go on with
the item above). A change that matters to the people chatting adds an item here, in the same commit, at the top:
the first words in bold are what zchat shows when it says what is new.

## 2026-10-15

* **Unread messages on the taskbar icon**: while you look elsewhere, a red circle on zchat's button in the taskbar
  counts the new messages, until you look at zchat again. On Windows, and on Linux in the docks and panels that show
  counts (Ubuntu Dock, Dash to Dock, KDE). 🔔 → Window turns it off, or `/notify taskbar off`.
* **Emoji**: type `:rofl:` and it is sent as 🤣 (`:` and two letters list the matching ones, in a terminal too), or
  pick any of them from 😀 by the input box, searchable, by category, with the ones you used lately first.
* **Reactions**: hover on a message and react with 👍 ❤️ 😂 😮 😢 🎉, or ☺+ for any emoji; reactions show under the
  message, with how many, and a click adds yours or takes it back. `/react [@NAME] EMOJI` in a terminal. Whoever
  was away gets them too.
* **Commands are suggested as you type them**: a `/` at the start of the line lists the commands, with what they
  take and what they do, narrowed down letter by letter (in a terminal too). Up/Down choose, Tab completes, Enter
  runs one that needs nothing more (or completes it, for the rest to be typed), Escape closes the list.
* **Messages you missed come to you**: what was said while you were away, in `#general`, your channels and your
  private chats, comes from whoever has it as soon as you are back (📥), even if who said it is gone: every zchat
  keeps the last 30 days of what it can see, and they compare what they have. Messages from before show again when
  zchat starts, in every channel.
* **The Wordle day ends even if nobody is online at midnight**: whoever played it is kept and synced, and the winner
  and the coins come as soon as somebody is back.
* **Links are clickable**: `http://`, `https://` and `www.` links in messages are underlined and open in your
  browser with a click, no more retyping them letter by letter. Only web links open.
* **Changelog**: what is new in zchat, a day at a time, in `CHANGELOG.md` and in zchat itself: 📜 at the top of the
  window, `/changelog [N|all]` (in a terminal too), and after an update zchat says once what is new since you last
  looked.
* **Longer games pay more coins**, so that an hour of any game pays about the same: a round's coins are multiplied
  by about how many minutes it takes, ×2 in Bomber, ×3 in Arena and Frag, ×5 in Wordle and ×8 in Pokémon (race and
  dice ×1).
* **Updates no longer hang**: the old zchat's window closes before the new one starts (Windows took it for a hung
  window and killed it); started from Explorer, the new one opens no console for a moment; and an old executable
  still in use no longer blocks the next update.
* **Notifications set as in Discord**: 🔔 at the top right, or `/notify` and `/dnd` (in a terminal too): all
  messages, only @mentions and private messages, or nothing, for all channels or each one (right-click a channel to
  mute it). Pick the sound (six of them, and their volume), the taskbar flashing, the unread count in the title,
  how much of a message a notification shows, and whether @everyone counts.
* **Do Not Disturb**, for a while or until turned off: keeps every notification quiet, trills too.
* Windows builds with `/MP`: from 174 to 45 seconds with Visual Studio.

## 2026-10-14

* **Notification popups**: while zchat is not the window in front, messages and events (a game starting, your turn
  at blackjack, a vote, a spy request) show up in a little window in the bottom right corner of the screen, as in
  Teams and Discord, on Windows and Linux (through Xwayland under Wayland). 🔔 turns them on and off, for messages
  or the rest, and shows a test one.

## 2026-10-12

* **An icon of its own**: a Z that is a Pokéball (`extras/icons`), on the .exe and the window on Windows, and on
  the window and among the desktop's applications on Linux.

## 2026-10-09

* **The world**: 🌍 Enter World at the top (or `/world`) opens a room to walk around in with everybody, each a
  crewmate of their color with their avatar on the visor. WASD or a click walks, Enter says it in `#general` and
  over your head, and the squares on the floor open the casino, Pokémon and the games.

## 2026-10-08

* **Pokémon challenges** come up in a popup.
* **Coins start over**: everybody starts again from 50 on their first start of this zchat, since balances had been
  changed by hand; what older versions say about coins is ignored. The sealed balances get a new key, too.

## 2026-10-07

* **Sealed coins**: balances are no longer plain numbers on disk but sealed (encrypted with a checksum, per
  person); a balance changed by hand goes back to 50.
* **Auto-update**: at startup and every 5 minutes zchat checks git and, when there is a newer one, installs it and
  restarts by itself (or says once why it cannot, and `/update` does it).
* **Private chats**: `/msg NAME [MESSAGE]` (or `/dm`, `/join @NAME`), or a click on someone under People, opens a
  chat of just the two of you, listed under Private chats; `/leave` closes it until something new is said.
* **Quotes**: hover a message and click Quote, or `/quote [@NAME] MESSAGE`; the quote shows above the reply, and
  whoever is quoted hears a sound.
* **Animated Pokémon battles**: the Pokémon and their moves are animated, in a bigger window.

## 2026-10-06

* **Shareholders** in Radical Candor: a new tier after the Board, who elect Board Members; the Brutally Honest AGM
  upgrade makes them ×3.
* Radical Candor's Max hire button takes clicks again: its label no longer gets rebuilt mid-click.

## 2026-10-05

* **Radical Candor** (`/game candor`, 🗣️ Candor at the top): an idle game, ZURU style, where you hire Developers,
  HR Partners, Team Leads and on up to the Board, to make candor until JavaScript runs out of numbers.
* **Autobuy** in Radical Candor: each tier has an Auto toggle, which keeps hiring whenever candor pays for it, even
  while you are away.
* **Dates in the chat**: each line shows the time it came at; hovering on it shows the date too.

## 2026-10-02

* **Bomber** (`/game bomber`): Bomberman for the whole chat; blow up crates and each other, and the last one
  standing wins Elo and coins.
* **Arena** (`/game arena`): a top-down shooter for the whole chat, three minutes of deathmatch; each player taken
  out pays a coin, and the window shows what they made you.
* **Frag** (`/game frag`): a cartoon first-person shooter for the whole chat, three minutes of deathmatch; each
  player taken out pays a coin.
* **Casino**: blackjack, roulette and a horse race for coins, at tables the whole chat shares, in a window of its
  own (`/casino`, 🎰 Casino at the top).
* **Spy**: `/spy @NAME` asks to see someone's screens, captured only if they let you, from a popup in the corner
  like the dice window. Not answering in time lets you in too, and only `/spy deny` refuses. 10 coins when
  accepted, 5 of which go to whoever agrees.
* **Games log**: in the window, what the games say goes to `#games-log`, so the chat stays a chat.
* **What is new at startup**: zchat checks for a newer version in the background and says what is new, and that
  `/update` installs it.
* Pokémon gets a Poké Ball instead of the swords: its button at the top is Pokémon, not Battle, and its notices
  start with a red ◓, like a Poké Ball's top.

## 2026-10-01

* **Coins**: winning games pays virtual money, spent on the annoying commands: `/trill` costs 5 (8 with your own
  sound or picture) and `/kick` 20; `/coins [NAME]`, `/coins top` and `/shop`.
* **Elo ratings** for the games, overall and per game, with leaderboards: `/game leaderboard [NAME|all]`
  (or `/game top`) and `/game elo [NAME]`.
* **/kick NAME**: the rest of the chat votes someone out in 20 seconds (`/kick yes`, `/kick no`, or the Kick and
  Grace buttons); with at least as many votes to kick as to grace, their zchat closes. `/kick @NAME` works too: the
  `@` the list of people types is not part of the name (for the Pokémon challenges too).
* **Pokémon of every generation**: random battles from Gen 1 to Gen 9, with Mega Evolution, Ultra Burst, Z-Moves
  and Dynamax; ⚔ Battle in the window picks who and which generation.
* **Pokémon challenges** pop up in the window as a card with Accept and Decline buttons, and the seconds left to
  answer; `/game pokemon challenge` finds people by their plain names, so it works in a window too.
* **Cancel in Pokémon**: takes a choice back, as on Showdown, until the other player chooses too
  (`/game pokemon cancel`).
* The only move left in a Pokémon battle (Recharge, a locked Outrage, Struggle, Gen 1's sleepy Fight) can be
  clicked: it has no PP, it is not out of them.
* **/help by topic**, with the games' commands only under `/game`: `/game help NAME` (or `/help game NAME`) tells
  how to play one.
* Dropping a file on the window works on Linux, and the window builds there; the Windows version cross-compiles
  from Linux.
* The tests build again with Visual Studio.

## 2026-09-30

* **The window**: zchat gets a graphical window, with chat history, emojis, and a place to change your avatar and
  name.
* **Avatars**: `/avatar FILE` sets the picture everybody sees next to your name (a GIF plays).
* **Channels**: public ones anybody can join and private ones for who you add.
* **GIFs**: send them, and they play.
* **Files**: send any type of file, downloadable with `/save`.
* **/trill [MP3]**: everybody else hears an MP3 once at full volume (`/trill` alone plays the built-in *Fahhh*),
  with a STOP to catch, a shaking window and a flying picture.
* **/game pokemon**: Pokémon Showdown's random battles between two people in the chat, in a Showdown-like window
  anybody can watch.
* **/game wordle**: the New York Times' Wordle of the day for the whole chat, lasting until the new word at
  midnight.
* **Dice in a window**: the dice game gets its own window.
* **Better ASCII art**: pictures drawn with characters look better, and `/ascii` takes how much texture to use
  (0% blocks only, to 100% symbols only).

## 2026-09-29

* **Games**: `/game race`, a typing race (first called Fastest finger, `/game finger`) that copy and paste cannot
  win; `/game paint`, a shared 16×16 canvas everyone colors; and `/game dice`, push your luck.
* **Tagging with @** and **@everyone**, with a list of who is online to pick from and a notification sound.
* **Pictures**: send them by dropping them or with `/image`, colored, scaled, and shown as blocks of color that
  look the same in every terminal and font. Dropped pictures with quotes in their name work, and
  `extras/terminator_fix_drop.py` fixes dropping files on Terminator 2.1.3.
* **Text styles**: bold, italic, underline, strikethrough and color tags (`/tags`), and the cursor moves left and
  right in what you type, which scrolls sideways when it is long.
* **Names**: `/nick` and `/color` set your name and its color, `/forget` gets a new random one, and `/whoami`
  shows it.
* **/update** pulls, rebuilds and restarts zchat, listing what is new.
* **Scrambled packets**: messages no longer travel over the network in plaintext.

## 2026-09-25

* **First version**: a chat for everyone on the local network, with no server, no accounts and no setup.
