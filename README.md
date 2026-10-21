# QuizFabric 3DS

A Nintendo 3DS client for joining live [QuizFabric](../PythonServer/PythonServer2/1.0) quizzes —
the 3DS equivalent of opening `/join` in a browser.

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

---

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
| `build_all.bat` | Both; skips the CIA (with a note) if its tools are missing |
| `clean.bat` | Removes `build/` and all output files |
| `tests\run_tests.bat` | Runs the protocol tests on your PC (via devkitPro's MSYS2) |

The scripts drive devkitPro's own MSYS2 bash, because devkitPro's `DEVKITPRO`
env var is an MSYS path (`/opt/devkitpro`) that plain `cmd.exe` can't resolve.

Prefer doing it by hand? From the devkitPro MSYS2 shell:

```sh
cd /e/3DSTHINGY
make 3dsx     # or: make cia, or: make all
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
- **CIA** — copy `QuizFabric3DS.cia` anywhere on the SD card, install with FBI.

---

## Using it

1. Someone hosts a quiz on the server (`/host/<quiz name>`), which shows a
   4-digit **game PIN**.
2. On the 3DS, tap each row on the touch screen to set:
   - **SERVER** — `host:port` of the QuizFabric machine (default port `5710`)
   - **NAME** — must be unique in the game; the server rejects duplicates
   - **GAME PIN** — the 4-digit code
   - **AVATAR** — tap to cycle; the list is pulled from the server the first
     time you tap it, so the names match the PNGs the host screen renders
3. Tap **JOIN GAME**.

Settings are saved to `sdmc:/3ds/quizfabric/config.txt`, so you only type the
server address once.

### Host control panel (from the 3DS)

The host screen shows a 4-digit **control code**. Tap the **CONTROL** row in
the setup menu and enter it to drive the game from the 3DS:

- **NEXT / START** — advance to the next question
- **REVEAL** — reveal answers early (only while a question is open)
- **AUTO** — toggle autoplay: auto-advance 5 seconds after the leaderboard
- **KICK** — tap a player to select them (gold highlight), then KICK

The touch screen shows the player list (with who has answered) and a live
feed of answers as they come in; B exits back to setup.

### Controls

| Input | Action |
|---|---|
| Touch | Everything — pick answers, edit fields, submit |
| D-Pad | Move the answer selection / scroll the leaderboard |
| A | Multiple choice: submit · Type-in: open the keyboard |
| X | Submit |
| B | Leave the game / back to setup |
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
| ← | `2` / → `3` | heartbeat ping / pong |

Events consumed: `joined`, `error`, `new_question`, `answer_revealed`,
`answers_revealed`, `leaderboard`, `final_leaderboard`, `player_joined`,
`player_left`, `answer_received`, `show_popup`.

Events emitted: `join_game`, `submit_answer`.

For multiple choice, `answer` is the **integer index** into the `answers` array
(the server checks `answer in correct_indices`). For type-in questions it's a
**string**, compared case-insensitively after trimming.

### Layout

| File | Role |
|---|---|
| `source/main.c` | State machine, screen drawing, input |
| `source/ui.c` | Gradient background, text measuring/wrapping, tiles |
| `source/ws.c` | Non-blocking TCP + WebSocket (RFC 6455) |
| `source/sio.c` | Engine.IO / Socket.IO framing |
| `source/json.c` | In-place JSON parser |
| `source/control.c` | Host control panel: live answers, autoplay, next, kick |
| `assets/` | Icon, banner, banner audio, `app.rsf` (CIA metadata) |
| `tests/` | Host-side protocol tests |

`ws.c` runs the connection setup as a per-frame state machine rather than a
blocking call, so a wrong IP shows a timeout instead of freezing the UI.

---

## Known limitations

- **Images load over the slow Wi-Fi in the background.** The server serves a
  pre-converted `.565` copy of each question PNG (`convert_stardew_images.py`
  on the QuizFabric machine generates them); the 3DS downloads it while you
  answer and shows the picture once it arrives, with a "Downloading image..."
  spinner on the top screen in the meantime.
- **Avatars aren't drawn**, only their names — the client fetches the list but
  not the PNGs.
- **No TLS.** The client speaks plain `ws://`. QuizFabric serves HTTP on port
  5710, so this matches; it is not suitable for a server behind HTTPS.
- **The WebSocket `Sec-WebSocket-Accept` header isn't verified.** Doing so needs
  SHA-1, which isn't worth pulling in for a LAN game client. The client checks
  for HTTP `101` and proceeds.

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
