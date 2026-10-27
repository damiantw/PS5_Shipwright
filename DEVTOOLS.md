# Developer tools (dev builds only)

Tools for testing the port from another machine: a **debug server** inside the game
(input injection, warps, state, screenshots, frame timing), a **crash handler**, **symbols**
for crash addresses, and a **Python client and test runner** (`ps5/tools/devtools/`).

The debug server exists only in a *dev build*. The release (store/catalog) zip has none of
its code: `release.sh` refuses a dev build, and CI checks the release ELF has no
`ps5_devtools_` symbols. The crash handler is in every build (it writes a file, no network).

## Quick start (for testers)

```sh
# 1. build the dev variant (its own build tree and dist folder) and its assets
PS5_DEVTOOLS=1 ps5/tools/build.sh                         # build/dev -> dist-dev/PPSA99320
PS5_DIST_DIR=dist-dev ps5/tools/assets.sh                 # soh.o2r etc. (the tools from build/game)
# 2. install it over the title (back up the console's install first; see "Restoring")
PS5_HOST=192.168.1.70 PS5_DIST_DIR=dist-dev ps5/tools/deploy.sh
# 3. switch the server on: an empty file is enough
echo > devtools.enable && curl -sT devtools.enable ftp://192.168.1.70:2121/data/soh/
# 4. launch the game, then from the box:
cd ps5/tools/devtools
./sohdev.py ping
./sohdev.py snap                                          # state, scene, room, Link, frame
./sohdev.py load 0                                        # load save slot 1 (slots are 0..2)
./sohdev.py warp LOST_WOODS_BRIDGE_EAST_EXIT --age child --time noon
./sohdev.py press A 2                                     # buttons for N game frames
./sohdev.py stick 0 80 40                                 # stick forward 40 game frames (2 s)
./sohdev.py shot /tmp/now.png                             # screenshot to the box
./sohdev.py perf --reset                                  # frame times since the last reset
./runtests.py                                             # every test in tests/
./runtests.py tests/03_lw_bridge.yaml --out /tmp/bridge   # one test, screenshots in /tmp/bridge
```

Notes from the first runs: a screenshot takes ~6 s (4K readback, scaled to 1280) and its
readback stalls a frame, so take screenshots outside perf windows. The first visit to the
Lost Woods bridge with save 1 plays Saria's farewell: tests use `clear_text` (press A until
Link moves) first.

Frames are **game frames** (20 a second, also with 60 fps frame interpolation). Sticks
take -80..80 (or -1.0..1.0); y up is forward. Buttons: A B Z START L R DUP DDOWN DLEFT
DRIGHT CUP CDOWN CLEFT CRIGHT. Entrances and scenes take a number or the name from
`soh/include/tables/{entrance,scene}_table.h` (`ENTR_`/`SCENE_` optional).

In Python:

```python
from sohdev import Client
c = Client()                       # SOHDEV_URL, default tcp://192.168.1.70:9200
c.load(0)                          # waits until the game is in play
r = c.warp("LOST_WOODS_BRIDGE_EAST_EXIT", age="child", time="noon")
print(r["load_seconds"], c.snapshot()["pos"])
c.perf_reset(); c.stick(0, 80, frames=60); print(c.perf()["ms_max"])
c.screenshot("bridge.png")
```

### Transports

All three start by default; each step is logged with its errno to klog and to
`/data/soh/devtools/status.txt` (fetch it over FTP). Tested on a PS5 with the title
launched normally from the home screen (registered through ShadowMountPlus), 2026-10-27:

| URL | how | result |
|---|---|---|
| `tcp://192.168.1.70:9200` | the game listens | **works**: socket, bind(0.0.0.0:9200), listen and accept all succeed; one command costs one network round trip |
| `reverse://0.0.0.0:9201` | the game dials out to `connect=` in devtools.enable; the client listens | connect() from the title works (a closed port on a LAN host gives errno 61 ECONNREFUSED at once); a Tailscale address (100.x) is unroutable from the console (errno 60 ETIMEDOUT after 75 s): the listener must be on the console's LAN |
| `mailbox://192.168.1.70:2121` | request files over the console's FTP server | **works**: ~1.3 s a command |

Mailbox: a client writes `/data/soh/devtools/mailbox/in/NAME.tmp`, renames it to
`NAME.req` (request lines); the game answers in `out/NAME.resp` and deletes the request.

### devtools.enable

`/data/soh/devtools.enable` switches the server on in a dev build (without it nothing
listens, dials or polls). Optional lines:

```
port=9200                        # listening port
token=SECRET                     # clients must send {"cmd":"auth","token":"SECRET"} first
connect=192.168.1.16:9201        # also dial out to this LAN address (reverse transport)
transport=listen,connect,mailbox # which transports start (default: all)
```

### Restoring

The dev build replaces the installed title. To go back: deploy the release folder
again (`PS5_HOST=... ps5/tools/deploy.sh`, or the release zip), then delete
`DEVTOOLS_BUILD.txt` and `eboot.sym` from the install folder, and `devtools.enable`
from `/data/soh`. Back up `/data/soh` before tests that save.

## Protocol

TCP (or a mailbox file): one JSON object per line. Request `{"id": 1, "cmd": "...", ...}`;
reply `{"id": 1, "ok": true, ...}` or `{"id": 1, "ok": false, "error": "..."}`. Commands
that touch the game run on the game thread at the start of its next frame (a reply
within 10 s, or an error).

| cmd | arguments | reply |
|---|---|---|
| `ping` | | `game_frame` |
| `version` | | `build` (build_id.txt), `content_version`, `image_base`, `protocol`, `port` |
| `snapshot` | | per-frame state, without waiting: `frame`, `state` (logo, title, file_select, play, ...), `state_frames`, `scene`, `room`, `entrance`, `loaded`, `pos`, `yaw`, `age`, `health`, `input_frames` |
| `state` | `actors` (true) | the snapshot, scene/entrance names, Link (pos, rot, speed, state flags), save basics, the actor list (id, name, category, params, room, pos) |
| `flags` | `scene` | eventChkInf, itemGetInf, infTable, eventInf, the scene's live flags, a scene's saved flags |
| `input` | `steps: [{buttons, stick: [x,y], rstick, frames, mode: or\|override}]`, `clear` | queued for player 1: each step for its frames, OR'd with the pad or replacing it |
| `input_clear` | | |
| `warp` | `entrance` or `scene` + `spawn`; `age` child/adult; `time` 0..65535, HH:MM, day, noon, evening, night, midnight | as SoH's `entrance` console command (instant); needs play |
| `wait` | conditions: `state`, `scene`, `room`, `entrance`, `loaded`, `state_frames`, `input_idle`, `frames`, `seconds`; `timeout` | when all hold on a frame after the request; else `error: timeout` with the snapshot |
| `cvar` | `name`; `value` (`type` int/float/string), `clear`, `save` | `type`, `value` |
| `console` | `line` | SoH console command: `result`, `output` |
| `save` | `slot` (default the loaded one) | as the pause menu saves |
| `load` | `slot` 0..2 | as the file select loads; from play, the title or the file select |
| `screenshot` | `max_width` (1280; 0 full), `inline` (true), `name` or `save` | `png_base64`; `path` in /data/soh/devtools/ |
| `perf` | `reset`, `recent` | since the last reset: `fps`, `ms_avg`, `ms_max`, `over_17/20/33/50ms`, `histogram`, `rings` (vertex/uniform peak and size), `recent_ms` |
| `mem_read` | `addr`, `eboot` (ELF offset) or `global` (gSaveContext, gPlayState, gGameState, player), `offset`, `len` ≤ 65536 | `hex`; refused where the kernel map says unreadable |
| `mem_write` | as mem_read, `hex` | refused where not writable |
| `globals` | | the addresses `global` takes |
| `log` | `lines` | the end of the game's log |
| `crash` | `confirm: true` | crashes the game (tests the crash handler) |

## Security

- Release builds: no server code at all (compiled out; checked in CI and by release.sh).
- Dev builds: nothing listens, dials or reads the mailbox unless `/data/soh/devtools.enable`
  exists. The listener answers only private addresses (10/8, 172.16/12, 192.168/16,
  100.64/10, 169.254/16, loopback); `token=` adds a shared secret. The protocol can read and
  write the game's memory: keep dev builds on a trusted network, and delete
  `devtools.enable` when done.

## Crash handler (every build)

SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT write `/data/soh/logs/crash-YYYYMMDD-HHMMSS.txt`
(UTC): the signal and fault address, every register, a frame-pointer backtrace, text
addresses found on the stack, the build ID and version, the end of the game's log, and
the game's state (game state, scene, room, entrance, Link's position). Then the console's
own crash handling goes on as before. Frames are printed as `eboot+0xOFFSET` and named when
an `eboot.sym` of the same build is at hand: a dev build ships it in the title folder; for
a release, put the release's `eboot.sym` (from the symbols asset) in `/data/soh/`.

## Symbols

CI attaches `PPSA99320-symbols-VERSION.tar.xz` to each release (not in the zip): the
unstripped linked ELF (`llvm-pie.elf`), `eboot.sym` and `build_id.txt`. To name an address:

```sh
ps5/tools/symbolize.sh --elf llvm-pie.elf eboot+0x1a2b3c     # or an address, 0x5a2b3c
ps5/tools/symbolize.sh --elf llvm-pie.elf --crash crash-20261027-190102.txt
ps5/tools/symbolize.sh 0x5a2b3c                               # this machine's last build
```

Check the crash report's `build:` line against `build_id.txt` first.

## Build details

- `PS5_DEVTOOLS=1 ps5/tools/build.sh`: CMake `-DPS5_DEVTOOLS=ON` in `build/dev`, output
  `dist-dev/PPSA99320/` with `DEVTOOLS_BUILD.txt` and `eboot.sym`; `build/last-title-dev.elf`.
  build.sh refuses a build tree configured for the other variant.
- `ps5/tools/package-dev.sh`: `dist-dev/PPSA99320-dev.zip` (CI artifact `PPSA99320-dev-*`,
  never published).
- Code: `ps5/src/devtools/` (server and game side), `ps5/src/crash.c`, `crash_context.c`;
  hooks under `#ifdef PS5_DEVTOOLS` in graph.c (game frame), padmgr.c (input), OTRGlobals.cpp
  (start) and gfx_vulkan.cpp (frame timing); screenshots reuse `devloop_shot.c`'s readback.
