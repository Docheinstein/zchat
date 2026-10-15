# Changelog

What is new in zchat, newest first, a day at a time. zchat shows it too: `/changelog`, or 📜 at the top of the
window, and after an update it says once what is new since you last looked.

Each day is a `## YYYY-MM-DD` heading and each change a `* ` item under it (lines starting with spaces go on with
the item above). A change that matters to the people chatting adds an item here, in the same commit, at the top:
the first words in bold are what zchat shows when it says what is new.

## 2026-10-15

* **Links are clickable**: `http://`, `https://` and `www.` links in messages are underlined and open in your
  browser with a click, no more retyping them letter by letter. Only web links open.
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

* Pokémon challenges come up in a popup.
* **Coins start over**: everybody starts again from 50 on their first start of this zchat, since balances had been
  changed by hand; what older versions say about coins is ignored.

## 2026-10-07

* **Sealed coins**: balances are no longer plain numbers on disk but sealed (encrypted with a checksum, per
  person); a balance changed by hand goes back to 50.
* **Auto-update**: at startup and every 5 minutes zchat checks git and, when there is a newer one, installs it and
  restarts by itself (or says once why it cannot, and `/update` does it).
* **Private chats**: `/msg NAME [MESSAGE]` (or `/dm`, `/join @NAME`), or a click on someone under People, opens a
  chat of just the two of you, listed under Private chats; `/leave` closes it until something new is said.
* **Quotes**: hover a message and click Quote, or `/quote [@NAME] MESSAGE`; the quote shows above the reply, and
  whoever is quoted hears a sound.
* Pokémon battles are animated, moves too, in a bigger window.

## 2026-10-06

* **Radical Candor** (`/game candor`, 🗣️ Candor at the top): an idle game, ZURU style, with Auto toggles that keep
  hiring while you are away, and Shareholders, a tier after the Board who elect Board Members (×3 with the
  Brutally Honest AGM upgrade). The Max hire button takes clicks again.
* Each line in the chat shows the time it came at; hovering on it shows the date too.

## 2026-10-02

* **New games for the whole chat**: Bomber (`/game bomber`, Bomberman), Arena (`/game arena`, a top-down shooter)
  and Frag (`/game frag`, a cartoon first-person shooter), three-minute deathmatches where each player taken out
  pays a coin.
* **Casino**: blackjack, roulette and a horse race for coins, at tables the whole chat shares, in a window of its
  own (`/casino`, 🎰 Casino at the top).
* **Spy**: `/spy @NAME` asks to see someone's screens, captured only if they allow it from a popup in the corner;
  10 coins when accepted, 5 of which go to whoever agrees.
* **Games log**: in the window, what the games say goes to `#games-log`, so the chat stays a chat.
* At startup zchat checks for a newer version in the background and says what is new.
* Pokémon gets a Poké Ball: on its button at the top, and a red ◓ in front of its notices.

## 2026-10-01

* **Coins**: winning games pays virtual money, spent on the annoying commands: `/trill` costs 5 (8 with your own
  sound or picture) and `/kick` 20; `/coins [NAME]`, `/coins top` and `/shop`.
* **Elo ratings** for the games, overall and per game, with leaderboards: `/game leaderboard [NAME|all]`
  (or `/game top`) and `/game elo [NAME]`.
* **/kick NAME**: the rest of the chat votes someone out in 20 seconds (`/kick yes`, `/kick no`, or the Kick and
  Grace buttons); with at least as many votes to kick as to grace, their zchat closes.
* **Pokémon of every generation**: random battles from Gen 1 to Gen 9, with Mega Evolution, Ultra Burst, Z-Moves
  and Dynamax; challenges pop up as a card with Accept and Decline; Cancel takes a choice back, as on Showdown.
* `/help` is grouped by topic, and `/game help NAME` tells how to play each game.
* Dropping a file on the window works on Linux, and the window builds there; the Windows version cross-compiles
  from Linux.

## 2026-09-30

* **The window**: zchat gets a graphical window, with avatars (`/avatar`, GIFs play), channels, chat history,
  emojis and GIFs.
* **Files**: send any type of file, downloadable with `/save`.
* **/trill [MP3]**: everybody hears an MP3 once (`/trill` alone plays the built-in *Fahhh*), with a STOP to catch,
  a shaking window and a flying picture.
* **/game pokemon**: Pokémon Showdown's random battles between two people in the chat, in a Showdown-like window
  anybody can watch.
* **/game wordle**: the New York Times' Wordle of the day for the whole chat, lasting until the new word at
  midnight.
* The dice game gets a window, and pictures look better as ASCII art.

## 2026-09-29

* **Games**: `/game race` (a typing race that copy and paste cannot win), `/game paint` (a shared 16×16 canvas) and
  `/game dice` (push your luck).
* **Tagging with @** and **@everyone**, with a list of who is online and a notification sound.
* **Pictures**: send them by dropping them or with `/image`, shown as blocks of color, scaled, the same in every
  terminal.
* Bold, italic, underline and strikethrough; nicknames and name colors; `/whoami`.
* **/update** pulls, rebuilds and restarts zchat, listing what is new.
* Messages no longer travel over the network in plaintext.

## 2026-09-25

* First version: a chat for everyone on the local network, with no server, no accounts and no setup.
