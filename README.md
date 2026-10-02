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

In the window the chat stays a chat: what the games say (rounds starting, rolls, boards, winners, Elo and coins, the
answers to `/game`, `/casino`, `/coins` and `/shop`) goes to `#games-log`, right under `#general`, a channel of the
window's own (🎮) that gets a dot when something new is in it. Typing one of those commands in the box shows it (the
games' own buttons do not move the chat). What is typed there is said in `#general`, but for a race's words, which
stay in the log, as the others' do: they are not shown in `#general` either. In the terminal everything is in the chat,
as before.

While chatting:

| Input              | Action                                                 |
|--------------------|--------------------------------------------------------|
| text + Enter       | send a message                                         |
| `/who`             | list who is in the chat                                |
| `/channels`        | list the channels: `#general`, the public ones, and the private ones you are in |
| `/create NAME [public\|private]` | make a channel and go to it: public (the default) anybody can join, private starts with just you |
| `/join NAME`       | go to a channel, joining it if it is public; what you say, draw and send goes there |
| `/add [#CHANNEL] NAME` | add someone in the chat to a channel (the one you are in, if not named) |
| `/remove [#CHANNEL] NAME` | remove someone from a channel                     |
| `/members [#CHANNEL]` | who is in a channel                                 |
| `/leave [NAME]`    | leave a channel (not `#general`)                        |
| `/delete NAME`     | delete a channel you created; `#general` can never be removed |
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
| `/addemoji NAME [SIZE] FILE` | save a picture as an emoji, sent as `/image` sends it (a GIF plays); the same name again replaces it |
| `/addemoji NAME ascii [SIZE] [TEXTURE%] FILE` | save an emoji drawn with characters, as `/ascii` draws it (a `TEXTURE%` alone does too) |
| `/emoji NAME`      | send a saved emoji; `/emoji NAME ascii` draws a picture emoji with characters instead; `/emoji` alone lists yours |
| `/removeemoji NAME` | delete a saved emoji                                  |
| drop an image      | drag an image file onto the window: the line becomes `/image PATH`, to send with Enter (or add a size first) |
| `/file FILE`       | send a file of any kind (up to about 34 MB), for everyone to download |
| drop a file        | drag any other file onto the window (or a terminal): the line becomes `/file PATH`, to send with Enter |
| `/save N`          | save file N of the chat in your downloads folder; `/save` alone lists them (windows have a Download button) |
| `/trill [SOUND] [PICTURE]` | trill everybody else: 3 seconds to catch a STOP button running around their screen, or their window shakes, the sound (MP3, WAV) plays at full volume and the picture flies around the screen; `/trill` alone sends the built-in *Fahhh*. It costs coins (see below): 5, or 8 with your own sound or picture |
| `/stop`            | stop the trills coming at you (or playing)             |
| `/kick NAME`       | ask the rest of the chat to vote NAME out: nobody is kicked right away. The others (not NAME) have 20 seconds to answer `/kick yes` to kick them or `/kick no` to grace them (in the window, a card pops up with Kick and Grace buttons); asking counts as a vote to kick. When the time is up, or everybody voted, NAME is out if at least as many voted to kick as to grace (who does not answer does not count), and their zchat closes. It takes someone else in the chat to vote; with several votes on, `/kick yes NAME` says which; `/kick` alone shows the votes on. Asking costs 20 coins, paid when the vote starts, whatever the chat says; voting is free |
| `/spy @NAME`       | ask NAME to let you see their screen(s). Nothing is captured behind their back: NAME is asked first and has 30 seconds to answer `/spy allow`, which makes *their* zchat take a picture of each of their monitors and share them with the chat, or `/spy deny` to refuse — and doing nothing refuses too, so no screenshot is ever taken without their yes. `/spy` alone shows the requests on; with several waiting, `/spy allow NAME` says which. Asking is free: only if they accept do you pay 20 coins, and they get 10 for letting you in. A refusal, no answer, or a machine that cannot take a screenshot costs nobody anything |
| `/coins [NAME]`    | your coins, or somebody's: won in the games, spent on `/trill`, `/kick` and `/spy`. In the window they are by your name (💰), and a click on them shows `/shop` |
| `/coins top`       | who has the most coins (`/game top coins` works too). Who left is on it too |
| `/shop`            | what coins buy, and how to win them (`/coins shop` works too) |
| `/casino`          | the casino: blackjack, roulette and a horse race, for coins, at tables the whole chat shares (see below). In the window it opens the casino's own window (🎰 Casino at the top); `/casino help` lists the commands to play in the chat |
| `@NAME`            | tag someone in a message: they hear a sound (type `@` to pick from the list) |
| `@everyone`        | tag all the people in the chat: they all hear a sound  |
| `/game`            | list the games everyone in the chat can play           |
| `/game race`       | typing race: some words show up for everyone in 3 seconds, the first to type them exactly wins |
| `/game dice`       | push your luck: `/game dice roll` a die as often as you dare, each roll adds to points only you see, but a 1 loses them all; `/game dice stop` keeps them (the others see only that you stopped: the points are shown when the round ends). The highest points kept in 60 seconds win; not stopped in time, and they are lost. In the window a round opens a dice window, with Roll and Stop buttons, the players and the time left (🎲 at the top opens it again) |
| `/game paint`      | a shared canvas of 16×16 squares: `/game paint c7 red` colors a square, `/game paint c7-f9 blue` a rectangle, `/game paint c7 none` empties it and `/game paint undo` takes back your last change. Without a color, squares get your brush's: `/game paint color red` picks it, and it starts as the canvas color closest to your name color. Each square painted takes a second before the next change, so the chat has to draw together |
| `/game wordle`     | the New York Times' Wordle of the day, for the whole chat: `/game wordle WORD` guesses the five-letter word whenever you like until midnight, showing you which letters are in it (green: right place, yellow: elsewhere) while the others only see your colors. Six guesses each; `/game wordle` shows your board and how everyone is doing. At midnight, when the Times has a new word, the fewest guesses win the day (ties share it). As on the Times, one a day: your guesses are saved, so coming back later goes on from them, and once done it is done until tomorrow. Guesses must be words of the built-in list (the five-letter words of ENABLE, public domain, in `third_party/enable`); the day's word always counts. Everyone gets the word from `nytimes.com/svc/wordle/v2/DATE.json`, with `curl` |
| `/game pokemon`    | Pokémon battles as on [Pokémon Showdown](https://pokemonshowdown.com), its Random Battles of any generation: `/game pokemon challenge NAME` challenges someone in the chat to Gen 9, `/game pokemon challenge NAME gen3` to Gen 3 (`gen1` to `gen9`), and they answer `/game pokemon accept` (or `decline`). In the window, ⚔ Battle at the top picks who and which generation, and a challenge to you pops up with Accept and Decline buttons. In the window the battle opens in a window of its own, like Showdown's: the field with both sides' Pokémon (sprites from Showdown's site, when online), buttons for the moves (with a box for the generation's own: Terastallize, Mega Evolve, Ultra Burst, Z-Power, Dynamax) and to switch, the battle log and its own chat, the timer and Forfeit (⚔ at the top opens it again). In the terminal it is played with `/game pokemon move N` (`move N tera` to Terastallize, `mega`, `ultra`, `z` and `max` for the others), `/game pokemon switch N` and `/game pokemon say TEXT`. `/game pokemon timer` turns on Showdown's timer (150 seconds to choose, or lose), `/game pokemon forfeit` gives up, and anybody can `/game pokemon watch NAME` a battle (`unwatch` stops). It is Showdown's own simulator, so it needs [Node.js](https://nodejs.org) on one of the two players' computers (zchat works without it, only not the battles): the first battle installs Pokémon Showdown with npm in the config folder (about 150 MB, a minute), and `/game pokemon update` gets its newest version, with the latest random battle sets |
| `/game bomber`    | Bomberman for the whole chat, seen from above as in the classic: `/game bomber` opens a round, and everybody has 20 seconds to `/game bomber join` (whoever opened it can `/game bomber go` sooner, with 2 players at least). Up to 16 play, each from a corner or a side of a map of walls and crates that grows with them; the others can watch. Bombs blow up after 2.5 seconds in a cross, breaking crates, setting off other bombs and blowing up whoever is in the flames, their owner too; crates may hide power-ups: 💣 one more bomb at a time, 🔥 longer flames, 👟 faster legs. The last one standing wins; after 2 minutes the walls close in, from the outside, so every round ends. In the window (💣 Bomber at the top) it has a window of its own, played with the arrows or WASD and Space for a bomb; in the terminal, `/game bomber up 3` (`down`, `left`, `right`) walks, `/game bomber bomb` drops one, `/game bomber stop` stops and `/game bomber map` draws the map with characters |
| `/game help NAME`  | how to play a game, and its commands (`/help game NAME` works too) |
| `/game scores`     | who won what in this session                           |
| `/game leaderboard` | the Elo leaderboard of all the games together (`/game top` works too); `/game leaderboard NAME` is a game's own, like `/game leaderboard race`, and `/game leaderboard all` shows each of them. Who left is on them too |
| `/game elo [NAME]` | somebody's Elo ratings, in all games and in each, with where they are on the leaderboards; yours without a name |
| `/update`          | get the latest zchat from git, build it and restart (see below) |
| `/help`            | list commands, by topic; the games' own are under `/game` |
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

`/trill SOUND PICTURE` trills everybody else in the chat, like MSN's nudge, whatever channel they are looking at (a
trill in a channel only reaches its members). It takes a sound (an MP3 or WAV file, up to 2 MB), a picture (any image
`/image` takes), or one of each, in any order; paths with spaces work too, as dropped on the terminal. Each of the
others gets a little window with a STOP button, running around their screen for 3 seconds: whoever catches it (or
types `/stop`) is spared. Everybody else gets their zchat window brought to the front (shown again if it was
minimized) and shaken, the sound at full volume, and the picture, big and shaking, flying around the screen above
every window for 3 seconds (clicks go through it). Whoever sends it gets none of it, and the chat shows who sent what.
`/trill` alone sends the *Fahhh* built into zchat (`src/sounds/`). `/stop` also stops a trill already playing.

The sound is played with the system's own player (Windows' MCI, `afplay` on macOS, and on Linux the first of
`mpg123`, `ffplay`, `mpv` and `paplay` that is installed), from zchat's temporary folder, where each sound is saved
the first time it comes, by name: sent again, the saved one plays. The picture is never saved: it is shown from
memory. While the sound plays, the speakers are turned up to 100% and unmuted (Windows' Core Audio, `osascript` on
macOS, `pactl` on Linux), then put back as they were once it is over (once the last one is over, when several play
at once). The popups are Windows' own, and GTK's in the Linux window (not tested yet); elsewhere (macOS, Linux
terminals) the chat says who is trilling you, with `/stop`, and shows the picture in the chat instead. Versions from
before trills show nothing; the ones with sound-only trills play the sound of trills without a picture.

### Coins

Winning games is not only for the leaderboards: every round of a game with a winner (the same rounds that change Elo
ratings: race, dice, wordle, pokemon and bomber) pays its players coins, which buy the annoying commands. Each player gets 2
for playing, and 4 more for each player they beat (2 for each they tied with): the winner of a duel gets 6 and the
loser 2, and the winner of a race of five 18. Everybody starts with 50, enough to trill and kick right away, and the
chat shows what each player of a round won, with their new balance.

| Costs | What                                                    |
|------:|---------------------------------------------------------|
|     5 | `/trill`: *Fahhh* at everybody else                     |
|     8 | `/trill SOUND PICTURE`: your own sound, picture, or both |
|    20 | `/kick NAME`: paid when the vote starts, whatever the chat says (voting is free) |
|    20 | `/spy @NAME`: ask to see their screen(s); paid only if they `/spy allow` (free otherwise), and they get 10 |

Without enough coins, the command is not sent, and zchat says what it costs and how many you have. The prices are in
`src/coins.hpp`, with what a round pays.

### Casino

Coins can also be bet at the casino, at three tables the whole chat shares: whoever bets first at a free table deals
that round (shuffles, spins the wheel, starts the race), the others can bet too for a while, and everybody sees
everybody's bets. Each table can be played alone too: the button to deal, spin or start (or its command) goes right
away once everybody who bet pressed it. Bets are from 1 to 500 coins. In the window, 🎰 Casino at the top opens the
casino's window: a lobby, and each table, with chips to pick the bet and buttons for everything.

| Table | Commands | What pays |
|-------|----------|-----------|
| 🃏 Blackjack | `/casino blackjack bet N` sits down (up to seven players), `deal`, then `hit`, `stand`, `double` or `split` (`bj` for short) | everybody plays their hand against the dealer at the same time; the dealer draws to 16 and stands on all 17s. A win pays 1 to 1, a blackjack 3 to 2, a push gives the bet back |
| 🎡 Roulette | `/casino roulette bet N WHAT`, as many as you like, then `spin` | a European wheel, 0 to 36. WHAT is a number (pays 35 to 1), `red`, `black`, `odd`, `even`, `low`, `high` (1 to 1), `1st`, `2nd`, `3rd` dozen or `col1`, `col2`, `col3` (2 to 1) |
| 🏇 Horse race | `/casino horses open` shows the six horses and their odds, `/casino horses bet N HORSE` (its number or name), then `go` | the winner's odds times the bet: about 9 in 10 of what is bet comes back, on average |

The house wins in the long run, as in any casino: the casino is where coins go.

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

The words `/game wordle` accepts are those of five letters of ENABLE (public domain), in `third_party/enable`.

Pokémon battles are those of [Pokémon Showdown](https://github.com/smogon/pokemon-showdown)'s simulator (MIT), which zchat installs with npm the first time it is needed; the sprites shown in the battle window come from `play.pokemonshowdown.com`. Pokémon is © Nintendo, Game Freak and Creatures: this is a game among friends.

The name set with `/nick` is saved in the `zchat` config folder: `%APPDATA%\zchat\config` on Windows,
`~/.config/zchat/config` on Linux (or `$XDG_CONFIG_HOME/zchat/config`). On start, `--name` wins over the saved
name, which wins over a random one. The color set with `/color` is saved there too.

The last 50 chat messages, sent and received, are kept in the `history` file of the config folder, with the time
they were sent at, and shown again when zchat starts, before the welcome (with their date when they are not from
today). Pictures, joins and other notices are not kept. The file is plain text, one message per line: delete it to
forget them.

Emoji are saved in the `emoji` folder of the config folder (`%APPDATA%\zchat\emoji` on Windows), one file each, as
it is sent: `NAME.pic` for a real picture, `NAME.art` for one drawn with characters. So `/emoji NAME` sends it right
away and the same every time, even if the image file is gone. Names are letters, digits, `-` and `_`, up to 32, and are not case-sensitive.

### Channels

Everybody is always in `#general`, which older versions see too. Other channels are made with `/create`: a public
one anybody can `/join` (everyone is told it was made), a private one starts with just you, and only its members see
it, and only people added to it can join. Anybody in a channel can `/add` or `/remove` people (by the name they have in
the chat: they must be online); only whoever created it can `/delete` it. `/join` shows only that channel (the window
empties, a terminal goes on below a line) with what was said there since zchat started; something new in another
channel is told about once, and in the window its name gets a dot. In the window the channels are on the left: a click
joins one (or its Join button), `×` leaves one, and `+` makes one; the current channel's Members button, at the
top, lists who is in it, to remove them or add who is not, and Leave and Delete (for its creator) are beside it. Channels you know are kept in `channels` in the config folder.

Private is not secret: like everything in zchat, their packets reach every computer on the network, only scrambled,
and zchat just does not show them to whoever is not in the channel. Older versions do not know channels, and see
only `#general`.

To run zchat with another config folder (another name, avatar, channels...), for example a second person on one
computer, set `ZCHAT_CONFIG_DIR` to it.

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
  terminal only (and `zchat` then chats in the terminal, with no window): install them and run CMake again.
  `-DZCHAT_GUI=OFF` does the same on purpose.

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

The window is built too (`-DZCHAT_GUI=OFF` leaves it out, for the terminal version).

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
* **Kicks** (`/kick NAME`) travel as `GAME` packets too (`kick vote`, `kick ballot`, `kick voted`, `kick result`):
  whoever asks counts the votes, says each one it counts (so voters send theirs again until it is counted), and sends
  the result a few times, as the one kicked has to get it; their own zchat then closes. Older versions ignore it.
* **Spying** (`/spy @NAME`) rides on `GAME` packets too (`spy ask`, `spy done`). The asker only sends a request; the
  picture is taken by the *target's* own zchat, and only after they answer `/spy allow` — no machine ever reaches into
  another to grab its screen, and no answer (or `/spy deny`) means no screenshot at all. On a yes, the target captures
  each of its monitors with the system's screenshot tool (`screencapture`, ImageMagick/`grim`/…, or a PowerShell
  snippet — see `src/capture.cpp`) and sends them as ordinary `IMAGE` packets, then a `spy done shot`. Nobody pays
  until that yes: on a shot the asker is charged 20 coins and the one who agreed gets 10; a refusal, no answer or a
  failed capture costs nobody anything. Older versions ignore it.
* **Games** (`/game NAME`) have no server either: whoever starts a round is its referee, and sends everybody what
  happens in it as `GAME` packets, whose text starts with the game's name (`race go <round> <words>`). In the race
  the referee picks the words, and the first message with them that reaches it wins. One letter of each
  word is shown as a Cyrillic lookalike (`а` for `a`), so the words cannot be copied and pasted: typed they match,
  pasted they do not, and whoever pasted them is out of the round. If two rounds start at
  once, everybody plays the one with the lowest round number; if the referee leaves, the others give up the round
  after a while. In dice everyone rolls their own die, and tells the others when they join, stop (with their
  points, which zchat shows only when the round ends; older versions show them right away, and miss newer ones
  stopping, so everybody should update) or roll a 1; every zchat ends the round when time is up, or when all the players are done and nobody
  can join anymore (after 15 seconds), and shows who won. In paint every zchat keeps its own copy of the canvas: each change of a
  square carries a clock and a tag from the painter's id, and a square keeps the newest change, so changes lost or
  heard out of order do not matter. Every 15 seconds each zchat sends a hash of its canvas, and whoever has a
  different one sends the changes it has, so newcomers and whoever missed a packet catch up. Older versions ignore `GAME` packets, so they just see people typing funny words.
* **Elo ratings** have no server to keep them either: every round of a game with a winner (race, dice, wordle,
  pokemon, bomber; not paint) is rated, in that game and in a general rating of all games together. Everybody starts at 1500;
  each pair of players of a round counts as a game, won by whoever finished better (half each for a tie), and with
  more players each pair counts less, so a race of five moves ratings about as much as a duel. K is 40 for the first
  10 games (marked `?`, still settling) and 20 after. In a race whoever typed something while the words were up took
  part; in dice the points kept rank the players, those who lost them all tied last; in wordle the guesses, those who
  did not find the word tied last; in bomber whoever lasted longer, those blown up at once tied. Each zchat keeps its user's own ratings, in `elo` in the config, and works out how
  they change from the results it sees, with the others' ratings as it last heard them; then it tells everybody
  (`elo ratings <user> <ratings>`), and also every minute and to whoever comes. The others keep what they hear in the
  `ratings` file of the config folder, by the user id that stays the same through changes of name and color, so the
  leaderboards show who left too. Everybody works out a round's new ratings to show them at once, but what a player's
  own zchat says is what counts. Who was not online for a round is not rated for it (a Wordle day is rated at
  midnight, by whoever is there). People with older versions are rated by name, for the session only.
* **Coins** are kept like the ratings: each zchat keeps its user's own, in `coins` in the config, adds what they win
  in the rounds it sees and takes what they spend, and tells everybody (`elo coins <user> <coins>`) along with the
  ratings. The others keep what they hear in the `coins` file of the config folder, by user id, for `/coins top`;
  they work out what a round pays everybody too, to show it at once. With no server, nothing stops someone from
  editing their own config to be rich, as with the ratings: it is a game among colleagues. Older versions ignore the
  coins, and trill and kick for free.
* **The casino's tables** (`/casino`) are `GAME` packets too, `blackjack ...`, `roulette ...` and `horses ...`:
  whoever bets first at a free table is that round's referee. At blackjack it shuffles six decks, deals, and sends
  the whole table (`blackjack state <round> <table>`, everybody's cards, the dealer's second one hidden until it
  plays) on every change and every two seconds; the players send their bets and moves (`blackjack act <round>
  <hand> <cards> hit`), again until the table shows them, and a move sent twice is made once. At roulette and the
  races everybody sends their bets to everybody, and the referee sends where the ball stops (`roulette spin <round>
  <number>`) or the whole race (`horses race <round> <order> <track>`: the order of finish, and where each horse is at
  twenty checkpoints, which every window plays out). Each zchat takes its user's bets from their coins and pays
  their winnings itself from what the referee says, so coins are kept as always (see above); a bet the referee never
  took, or a round whose referee left, is given back. Two tables opened at the same time become the lowest numbered
  one. The arithmetic (cards, payouts, odds, how the horses run) is in `src/casino.cpp`, with its tests.
* **Pokémon battles** are run by one of the two players' zchat, the referee: the challenger's if it has Node.js, or else the
  other's. It runs Pokémon Showdown's simulator with node and `src/pokemon/bridge.js` (built into zchat, written next to
  the simulator), which prints what the battle says in three streams: what everyone sees, and what each player sees
  (their own team, their choices). The referee numbers the messages of each stream and sends them in parts that fit
  in a `GAME` packet; each player acknowledges theirs, and is sent again what they did not, and those watching ask for
  what they missed, as every second or two the referee tells how many messages each stream has. The players' choices
  go to the referee until it says it has them. A player's stream travels like everything else, so another zchat could
  read it: battles are for fun. If the referee goes silent the battle ends with no winner; a player silent for a
  minute loses. `src/pokemon_state.cpp` turns Showdown's protocol into what the window and the terminal show.
* **Bomber** is played in real time, so whoever opens a round is its referee all along: it runs the game, 20 steps a second, and after each step sends everybody the whole map (a character per tile) and where every player is, in one `GAME` packet (`bomber s <round> <step> ...`). The players only send the key they hold and how many bombs they asked for (`bomber in`), when it changes and every 300 ms, as packets get lost; windows draw the players between their last two places, so they glide. Who joins is told every second while the round is open (`bomber open`), and who plays every second while it is on (`bomber roster`), so whoever comes in the middle can watch. Where each finished (`bomber end`) is sent a few times, and rated by everybody. While a round is on, the chat ticks the games 50 times a second instead of 5. If the referee goes silent for 6 seconds the round is off, unrated.
* **Avatars** are square pictures (a still image cropped to its middle, 256 pixels; a GIF as it is up to 2 MB, or
  else as 128-pixel frames), saved in the config folder as `avatar`. Heartbeats (`JOIN` and `HERE`) carry
  `avatar <hash> <bytes> <TCP port>`, and whoever does not have that avatar yet downloads it from the sender with
  `AVATAR <hash>`, as for big pictures; so newcomers get everyone's within a heartbeat, and a new one shows right
  away. Terminals do not show avatars.
* **Channels** have no server either. Everybody has a user id that does not change (`user` in the config; the sender
  id changes with the color), sent in heartbeats. A channel is its name, a version, public or private, its creator,
  and its members' user ids; a change makes a new version, sent as a `CHANNEL STATE` packet (and, now and then, by
  its members, for whoever missed it), and the newest one wins (the same version: the one from the higher user id).
  Everybody checks who made it: a member (or joining a public one), and the creator alone to delete it. What is said
  in a channel goes as `CHANNEL MESSAGE` packets (`<channel> m` or `<channel> a`, then the message or the drawing);
  pictures and files as usual, with `channel <channel>` as their first line. Only members show them.
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
ZCHAT1 \n <J|H|M|L|P|G|I|O|K|R|N|C> \n <sender id, hex> \n <sequence number> \n <name> \n <text>
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
