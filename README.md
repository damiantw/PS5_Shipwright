# Ship of Harkinian for PlayStation 5

Play **Ship of Harkinian** (Shipwright), the PC port of *The Legend of Zelda: Ocarina of Time*,
on a jailbroken PS5. This is a homebrew port: the game runs natively on the console with
Vulkan graphics, the DualSense, saves and Ship of Harkinian's full settings menu.

**No game data is included.** You need your own, legally dumped Ocarina of Time ROM. The
game turns it into its data file on the console the first time you start it.

## Credits

- [HarbourMasters](https://github.com/HarbourMasters/Shipwright): Ship of Harkinian, and
  Kenix3 and the [libultraship](https://github.com/Kenix3/libultraship) contributors.
- [griches/PS5_Ghostship](https://github.com/griches/PS5_Ghostship) (Gary Riches): the PS5
  console layer and the libultraship Vulkan changes this port is built on.
- [mihawk-99](https://github.com/mihawk-99) (Mihawk): PS5_Vulkan, PS5_Mesa (RADV on the
  PS5) and PS5_PayloadSDK.
- [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) (John Törnblom).

## Requirements

- A jailbroken PS5 running a homebrew environment (etaHEN or similar).
- ShadowMountPlus or a similar tool that registers homebrew in
  `/data/homebrew/` as a title, so **PPSA99320** ("Ship of Harkinian") appears on the home screen.
- Your own Ocarina of Time ROM (`.z64`, `.n64` or `.v64`), in a version Ship of Harkinian
  supports. A Master Quest ROM is optional.

## Install

**PS5 Homebrew Catalog (recommended, once listed):** find "Ship of Harkinian" in the
catalog and install it.

**Manual install:**

1. Download `PPSA99320.zip` from the [Releases](https://github.com/damiantw/PS5_Shipwright/releases) page.
2. Unzip it. It contains one folder, `PPSA99320`.
3. Copy that folder to `/data/homebrew/PPSA99320` on the console, using whatever you like:
   a file manager on the console, a USB drive, or an FTP client if your homebrew setup runs
   an FTP server.
4. Let ShadowMountPlus (or your tool) register it. "Ship of Harkinian" appears on the home screen.

## First run

1. Copy your ROM into **`/data/soh/`** on the console (create the folder if needed). The file
   name doesn't matter; `.z64`, `.n64` and `.v64` (any byte order) are accepted. Add a Master
   Quest ROM too if you have one.
2. Start Ship of Harkinian.

When there is no `oot.o2r` or `oot-mq.o2r` in `/data/soh/`, the game looks for ROMs there,
checks each one against the versions Ship of Harkinian supports, and converts it on the
console with a progress bar (a few minutes). The result, `oot.o2r` (and `oot-mq.o2r`), is
saved in `/data/soh/` and the game starts. Later launches skip this step. You can delete the
ROM afterwards.

If no supported ROM is found, the game shows a "Game files needed" message and exits. A ROM
that was found but didn't match a supported version is named in the log (`/data/soh/logs/`).

**Alternative:** make `oot.o2r` on a PC with desktop Ship of Harkinian (or this repo's
`ps5/tools/assets.sh`) and copy it to `/data/soh/`.

Never share your ROM or an `.o2r` made from it.

## Data folder

Everything that belongs to you lives in `/data/soh/`, outside the game folder, so updating
or reinstalling the game never touches it:

```
/data/soh/
  oot.o2r, oot-mq.o2r     game data made from your ROM
  shipofharkinian.json    settings
  imgui.ini               menu layout
  Save/                   your save files
  logs/                   Ship of Harkinian.log
  mods/                   .o2r / .otr mods
```

- Back up `Save/` (and `shipofharkinian.json`) to keep your progress and settings.
- If `/data/soh` can't be created or written (for example, the game isn't running through
  ShadowMountPlus), the game falls back to `userdata/` inside its own folder.
- To use another folder, create `env.txt` in the game folder
  (`/data/homebrew/PPSA99320/env.txt`) with a line such as `PS5_DATA_DIR=/data/mysoh`.

## Controls

The DualSense uses Ship of Harkinian's default N64 layout: Cross is A, Circle/Square is B,
Options is Start and the right stick is the C buttons. You can remap everything in the menu.

**Settings menu:**

| Button | Action |
| --- | --- |
| Touchpad click | open / close the menu |
| D-pad / left stick | move between items (amber frame shows focus) |
| Cross | select / enter |
| Circle | back; closes the menu when nothing is selected |
| L1 / R1 | switch tabs |

The menu is sized for a TV (scale options 1.5x to 3.0x, "Large" by default), shows PS5
button names, and hides PC-only options.

## Updating

Install the new version from the Homebrew Catalog, or copy the new `PPSA99320` folder over
the old one in `/data/homebrew/` (close the game first). Your saves, settings and `oot.o2r`
in `/data/soh/` stay as they are.

## Known issues

- The PS5 system UI (PS button, control center) is very sluggish while the game runs.
- The home-screen icon can keep showing an old picture after an update. Restart the console
  or re-register the title in ShadowMountPlus.
- Graphics menu: "Renderer API" says OpenGL though the game uses Vulkan, and "Allow
  multi-windows" is a PC option. Leave both alone.
- In the menu, moving from the sidebar into the right-hand panel with the d-pad is awkward.
- On the title screen, a button press can reach the game while the menu is open (Cross may
  open file select).

## Troubleshooting

- **"Game files needed" at start:** put a supported ROM (or `oot.o2r`) in `/data/soh/` and
  start again. Check `/data/soh/logs/Ship of Harkinian.log` for a ROM that was rejected.
- **Saves or settings seem to reset:** the game probably couldn't use `/data/soh` and fell
  back to its own folder. Make sure it's launched through ShadowMountPlus (or similar).
- **The menu won't open:** click the touchpad (press it down; a swipe does nothing).
- **Anything else:** the log is `/data/soh/logs/Ship of Harkinian.log`. Include it when you
  open an [issue](https://github.com/damiantw/PS5_Shipwright/issues).

## Building from source

For developers. You need a Linux x86-64 machine with clang/LLVM 18+, cmake, ninja, python3,
git, ffmpeg and ImageMagick, plus [PS5_PayloadSDK](https://github.com/mihawk-99),
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) and PS5_Mesa checked out beside this
folder, with RADV built.

```sh
git submodule update --init --recursive
bash ps5/tools/build.sh     # applies ps5/patches, configures and builds
bash ps5/tools/assets.sh    # builds soh.o2r
```

The output is `dist/PPSA99320/`. All PS5 changes to Shipwright, libultraship, torch and SDL
are kept as patches in `ps5/patches/`; after editing files under `vendor/`, save them back with
`bash ps5/tools/setup-source.sh --save`. The console-specific code is in `ps5/src/`.

Never commit a ROM or an `.o2r` made from one; `.gitignore` blocks them.

## Licence

GPL-3.0. See [LICENSING.md](LICENSING.md).
