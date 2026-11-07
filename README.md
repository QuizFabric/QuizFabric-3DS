# QuizFabric 3DS

A Nintendo 3DS client for [QuizFabric](https://github.com/lucgei231/PythonServer) —
a Kahoot-style quiz server. Join live games, play solo quizzes, host your own,
download Kahoot quizzes, or play completely offline from an SD-card cache.

The **question shows on the top screen**; the **answer tiles / text box and the
SUBMIT button live on the touch screen**, over an animated pink→purple gradient.

```
┌─ TOP (400x240) ──────────────────┐   ┌─ BOTTOM (320x240) ──────────┐
│  Q3  Minecraft Quiz     1250 pts │   │ ┌────────┐ ┌────────┐       │
│                                  │   │ │ Bedrock│ │ Chest  │       │
│   Which block is craftable?      │   │ └────────┘ └────────┘       │
│                                  │   │ ┌────────┐ ┌────────┐       │
│  ███████████░░░░░░  620 points   │   │ │ Barrier│ │ Cmd Blk│       │
└──────────────────────────────────┘   │ └────────┘ └────────┘       │
                                       │ ┌─────── SUBMIT ─────────┐  │
                                       │ └────────────────────────┘  │
                                       └─────────────────────────────┘
```

## Features

- **Stereoscopic 3D** — push the 3D slider up and the whole top screen goes 3D
  with layered depth: the animated background sits deep, glass panels float
  above it, text sits at the screen, and question images/banners pop out
  slightly. The depth **scales with how high the slider is**; slider down is
  plain 2D.
- **Live games** — join with a 4-digit PIN, name and avatar; multiple choice,
  type-in, slider and puzzle questions; animated GIF questions (full length,
  streamed from the SD card so they don't eat RAM); emoji everywhere; toasts;
  leaderboards.
- **Solo mode** — browse quizzes with Kahoot-style cover banners (2-per-row
  grid), Y to search, circle pad or D-pad to move, hold up/down to fast-scroll.
- **Host + control panel** — host a quiz from the 3DS, then drive it from any
  3DS with the control code: START/NEXT, REVEAL, autoplay (auto-advance 5 s
  after each leaderboard) and kick players.
- **Kahoot download** — search Kahoot or paste an ID/link; the server converts
  the quiz (questions AND images) into a QuizFabric quiz that becomes yours.
- **SD cache + offline** — X pre-caches a quiz's photos, X+SELECT caches every
  quiz (missing files only); Y on the setup screen toggles offline mode; play,
  host and browse cached quizzes with no internet; delete cached quizzes from
  the offline menu.
- **Delete** — remove quizzes you uploaded or stole, right from the setup menu.

## Building

### 1. Install devkitPro

Get the installer from <https://github.com/devkitPro/installer/releases> and
**tick the "3DS Development" package group**. That provides devkitARM, libctru,
citro3d and citro2d.

If you already have devkitPro but not the 3DS packages:

```
<devkitPro>\msys2\usr\bin\bash.exe -l -c "pacman -Sy --needed 3ds-dev"
```

Installed somewhere other than `C:\devkitPro`? Point the scripts at it:

```
setx DEVKITPRO_WIN "E:\devkitPro"
```

### 2. Build

Double-click one of these (or run from a terminal):

| Script | What it does |
|---|---|
| `check_env.bat` | Reports which tools/libraries are present and which are missing |
| `build_3dsx.bat` | Builds `QuizFabric3DS.3dsx` (+ `.smdh`) for the Homebrew Launcher |
| `build_cia.bat` | Builds `QuizFabric3DS.cia` for installing with FBI |
| `build_prod.bat` | Builds the **production** `QuizFabric3DSProd.cia` — fixed server `play.lucasgeitgey.com:80`, no server field on screen |
| `build_all.bat` | Both; skips the CIA (with a note) if its tools are missing |
| `clean.bat` | Removes `build/` and all output files |
| `tests\run_tests.bat` | Runs the protocol tests on your PC (via devkitPro's MSYS2) |

The scripts drive devkitPro's own MSYS2 bash, because devkitPro's `DEVKITPRO`
env var is an MSYS path (`/opt/devkitpro`) that plain `cmd.exe` can't resolve.

Prefer doing it by hand? From the devkitPro MSYS2 shell:

```sh
cd /e/3DSTHINGY
make              # dev: .3dsx + .cia, server set on screen
make PROD=1       # production: fixed server, own TID (CTR-P-QZFP)
```

### CIA extras

`.cia` packaging needs two tools beyond the compiler:

- **makerom** — usually comes with devkitPro's `3dstools`. If not:
  <https://github.com/3DSGuy/Project_CTR/releases>
- **bannertool** — *not* shipped by devkitPro:
  <https://github.com/Steveice10/bannertool/releases>

Drop either `.exe` into `tools\` in this folder and the build scripts will find
it automatically. `build_cia.bat` checks for both up front and prints these
links rather than failing halfway through.

### Installing on the 3DS

- **3DSX** — copy `QuizFabric3DS.3dsx` to `/3ds/` on your SD card, launch from
  the Homebrew Launcher.
- **CIA** — copy `QuizFabric3DS.cia` (or `QuizFabric3DSProd.cia`) anywhere on
  the SD card, install with FBI.

---

## Using it

### Join a live game

1. Someone hosts a quiz on the server, which shows a 4-digit **game PIN**.
2. On the 3DS, tap each row on the touch screen to set:
   - **SERVER** — `host:port` of the QuizFabric machine (default port `5710`)
   - **NAME** — must be unique in the game; the server rejects duplicates
   - **GAME PIN** — the 4-digit code
   - **AVATAR** — tap to cycle; the list is pulled from the server the first
     time you tap it, so the names match the PNGs the host screen renders
3. Tap **JOIN GAME**.

Settings are saved to `sdmc:/3ds/quizfabric/config.txt`, so you only type the
server address once.

### Solo / Host / Kahoot

- **SOLO** opens the quiz list (covers, search, pre-cache). Pick one and play
  the full quiz locally scored.
- **HOST** opens the same list; picking one creates a game and shows the game
  PIN plus a **control code**. Enter that code under **CONTROL** on any 3DS to
  drive the game remotely.
- **KAHOOT** is the downloader: type a search query (or paste a Kahoot ID or
  link), pick a result, and the server imports it as your own quiz. You can
  rename it before saving.

### Offline mode

- Press **Y** on the setup screen to toggle offline mode. The app also forces
  offline automatically if it can't reach the internet at boot (15 s check);
  press **Y** again to re-check and come back online.
- Pre-cache quizzes first: **X** caches the selected quiz's photos, **X+SELECT**
  caches every quiz (only missing files — existing cache is never wiped).
- Offline: play or host cached quizzes with banners, and delete cached quizzes
  from the offline menu.

### Controls

| Input | Action |
|---|---|
| Touch | Everything — pick answers, edit fields, submit |
| D-Pad / Circle Pad | Move the answer selection / browse quiz lists / scroll the leaderboard |
| A | Multiple choice: submit — Type-in: open the keyboard |
| X | Submit (questions) — pre-cache the selected quiz (quiz lists) |
| Y | Toggle offline mode (setup) — search quizzes (quiz lists) |
| B | Leave the game / back |
| L / R | Jump 5 results on the Kahoot search page |
| X+SELECT | Pre-cache every quiz (only missing files) |
| START | Quit (from the setup or error screen) |

### Scoring

The server awards `max(0, 1000 - seconds × 50)`, so points reach zero at 20
seconds. The top screen shows that window draining, along with the points
currently on offer.

---

## How it talks to the server

QuizFabric uses Flask-SocketIO, and its `join.html` loads socket.io **4.0.1** —
Socket.IO protocol **v5** over Engine.IO protocol **v4**.

Rather than the usual HTTP long-poll handshake and upgrade dance, this client
opens the WebSocket transport directly, which Engine.IO permits:

```
GET /socket.io/?EIO=4&transport=websocket
```

Then:

| Direction | Packet | Meaning |
|---|---|---|
| ← | `0{"sid":…,"pingInterval":25000,…}` | Engine.IO open |
| → | `40` | connect to the default namespace |
| ← | `40{"sid":…}` | namespace connected |
| → | `42["join_game",{"code","name","avatar"}]` | join |
| ← | `42["joined",{"code","quiz_name"}]` | accepted |
| ← / → | `2` / `3` | heartbeat ping / pong |

Events consumed: `joined`, `error`, `new_question`, `answer_revealed`,
`answers_revealed`, `leaderboard`, `final_leaderboard`, `player_joined`,
`player_left`, `answer_received`, `show_popup`.

Events emitted: `join_game`, `submit_answer` (and the control-panel events:
`control_join`, `start_round`, `reveal_answers`, `kick_player`).

For multiple choice, `answer` is the **integer index** into the `answers` array
(the server checks `answer in correct_indices`). For type-in questions it's a
**string**, compared case-insensitively after trimming.

### Layout

| File | Role |
|---|---|
| `source/main.c` | State machine, screen drawing, input, question images |
| `source/ui.c` | Gradient background, text measuring/wrapping, tiles, 3D parallax |
| `source/ws.c` | Non-blocking TCP + WebSocket (RFC 6455) |
| `source/sio.c` | Engine.IO / Socket.IO framing |
| `source/json.c` | In-place JSON parser |
| `source/solo.c` / `source/host.c` | Quiz lists (covers, search), solo play, hosting |
| `source/control.c` | Host control panel: live answers, autoplay, next, kick |
| `source/steal.c` | Kahoot search and download |
| `source/precache.c` / `source/cachedel.c` | SD-card cache and cache deletion |
| `source/thumb.c` | Banner thumbnails (animated on screen only) |
| `source/delete.c` | Delete your server-side quizzes |
| `assets/` | Icon, banner, banner audio, `app.rsf` / `app_prod.rsf` (CIA metadata) |
| `tests/` | Host-side protocol tests |

`ws.c` runs the connection setup as a per-frame state machine rather than a
blocking call, so a wrong IP shows a timeout instead of freezing the UI.

---

## Known limitations

- **No TLS.** The client speaks plain `ws://`. QuizFabric serves HTTP on port
  5710, so this matches; it is not suitable for a server behind HTTPS.
- **The WebSocket `Sec-WebSocket-Accept` header isn't verified.** Doing so needs
  SHA-1, which isn't worth pulling in for a LAN game client. The client checks
  for HTTP `101` and proceeds.
- **Avatars aren't drawn**, only their names — the client fetches the list but
  not the PNGs.

---

## Testing

`json.c` and `sio.c` have no 3DS dependencies, so the protocol layer — the part
most likely to be subtly wrong — is covered by tests you can run on your PC:

```
tests\run_tests.bat
```

These parse real QuizFabric payloads (multiple-choice and type-in questions,
leaderboards with `null` fields and players who left, escaped and Unicode
strings, namespaced/ack-id packets) and check malformed input fails cleanly
rather than crashing.

---

## Related

- [QuizFabric DS](https://github.com/QuizFabric/QuizFabric-DS) — the Nintendo
  DS / DSi client (no GIFs, low-res images to save RAM)
- [QuizFabric server](https://github.com/lucgei231/PythonServer) — the Flask /
  Socket.IO server

## License

[CC-BY-SA 4.0](LICENSE)
