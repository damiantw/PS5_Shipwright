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

1. Download `PPSA99320.zip` from the [Releases](https://github.com/damiantw/PS5_Shipwright/releases) page.
2. Unzip it. It contains one folder, `PPSA99320`.
3. Copy that folder to `/data/homebrew/PPSA99320` on the console (so `eboot.bin` ends up at
   `/data/homebrew/PPSA99320/eboot.bin`), using whatever you like:
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
console with a progress bar (about 30 seconds). The result, `oot.o2r` (and `oot-mq.o2r`), is
saved in `/data/soh/` and the game starts. Later launches skip this step. You can delete the
ROM afterwards.

Extensions are matched in any case (`OOT.Z64` works) and spaces in the name are fine. If
several ROMs of the same version are present, the first is converted and the others are
skipped.

If no supported ROM is found, the game shows a "Game files needed" message listing every file
it found in `/data/soh/` that it could not use, with the reason (for example "is 8.0 MB; an
OoT ROM is 32, 54 or 64 MB (truncated?)" or "checksum not recognised; damaged or another
version"). Press Cross to close it (the first press may only wake the controller; press again).
The same reasons are written to `/data/soh/logs/Ship of Harkinian.log` as `PS5: ROM rejected:`
lines.

**Alternative:** make `oot.o2r` on a PC with desktop Ship of Harkinian (or this repo's
`ps5/tools/assets.sh`) and copy it to `/data/soh/`.

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

| Button | Game |
| --- | --- |
| Left stick | move |
| Right stick | camera (free look); aims in first-person view |
| Cross / Circle | A / B |
| D-pad | C buttons (up: first-person view / Navi) |
| L2 / R2 / L1 | Z (target) / R (shield) / L |
| Options | Start |

The right stick turns the camera (Ship of Harkinian's Free Look, slightly faster than its
default) and the C buttons are on the D-pad; an existing configuration is switched to this
once, on the first start of this version. Change it under **Settings > Controls** (camera
speed, invert) or remap everything in the controller bindings.

Button prompts in the game (dialogue, the HUD's C buttons, pause menu, ocarina, file select)
show PlayStation buttons: Cross, Circle, L1/L2/R2 and the D-pad. They also win over a texture
pack's buttons. Turn them off under **Settings > General > PlayStation Button Prompts**
(applies after a restart).

**Settings menu:**

| Button | Action |
| --- | --- |
| Touchpad click | open / close the menu |
| D-pad / left stick | move between items, across the tab row, sidebar and page (amber frame shows focus) |
| Cross | select / enter |
| Circle | back; closes the menu when nothing is selected |
| L1 / R1 | previous / next top tab |
| L2 / R2 | previous / next sidebar page |

The menu is sized for a TV (scale options 1.5x to 3.0x, "Large" by default), shows PS5
button names, and hides PC-only options.

## Updating

Download the new `PPSA99320.zip` from Releases and copy its `PPSA99320` folder over
the old one in `/data/homebrew/` (close the game first). Your saves, settings and `oot.o2r`
in `/data/soh/` stay as they are.

## Mods and texture packs

Ship of Harkinian's mods work on the PS5 as on a PC.

- Put `.o2r` (or older `.otr`) mod files in **`/data/soh/mods/`** (the game creates the
  folder on first start). Custom music (`.ootrs`) goes there too.
- A new mod is enabled automatically the next time the game starts. To turn mods on or off or
  change their order, open the menu (touchpad click) > **Settings > Mod Menu**. Priority is
  top to bottom: a mod higher in the list overrides the ones below it. Changes apply after a
  restart (close the game from the PS5 Switcher and start it again).
- Texture packs that keep their files under `alt/` (most HD packs, for example OoT Reloaded)
  work straight away: **Enhancements > Graphics > Mods > Use Alternate Assets** is on by
  default. If a pack's textures don't show, check that option.
- Large HD packs take longer to load scenes, but play at 60 fps.

## Performance

The game renders at 3840x2160 and runs at 60 fps by default (Settings > Graphics > Frame
Interpolation; 20 is the original N64 rate). Shaders are compiled the first time each one is
needed and kept in `/data/soh/cache/`. On first launch, and after each update, a short
"Preparing Shaders" screen (about 2 seconds) compiles the common ones on the console from a
list of shader keys that ships with the game (`shader_keys.txt`; it contains no compiled
code). Shaders not on the list are still compiled when first needed, so a rarely seen
effect can stutter once and not afterwards.

## Known issues

- The Select Item page of the pause menu shows the game world behind the item grid instead
  of the dark background.
- The button prompts follow the default layout above; after remapping buttons the in-game
  glyphs still show the default buttons.
- The HUD's A and B buttons keep the game's action words (no glyph is drawn on them in the
  original game either).
- The sun's lens flare and the glow around some lights (torches, fairies) are never drawn:
  the depth test that decides whether they're hidden runs before the frame is rendered.
- Texture packs that replace whole scenes (e.g. Djipi's 3DS Experience, archive 30 "Hyrule
  Field") can slow Hyrule Field down: that archive draws the field's full mesh once per
  section, up to about seven times a frame. Disable just that archive if it's too slow.
- A crash at the Lost Woods bridge (01.000.000, reported on a PS5 Pro) came from an animation
  missing from `oot.o2r`. The game now carries on and names the missing file in the log; if
  you see "could not be loaded" there, delete `/data/soh/oot.o2r` and start the game again
  to rebuild it from your ROM.
- Not reproduced yet, reports welcome with the log: a bright flash every few seconds in
  Kokiri Forest after starting the game, and the castle courtyard guards' timing.

## Troubleshooting

- **"Game files needed" at start:** put a supported ROM (or `oot.o2r`) in `/data/soh/` and
  start again. The message lists each ROM it rejected and why; a truncated copy or one with
  an unknown checksum needs a fresh dump.
- **Saves or settings seem to reset:** the game probably couldn't use `/data/soh` and fell
  back to its own folder. Make sure it's launched through ShadowMountPlus (or similar).
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

## Releasing

- The version and title id come from `ps5/sce_sys/param.json` (`titleId` PPSA99320,
  `contentVersion` as `NN.NNN.NNN`, starting at `01.000.000`). The build copies that file into
  the title unchanged.
- `bash ps5/tools/release.sh` builds and writes `dist/PPSA99320.zip` (`--no-build` packs the
  last build). The zip has one `PPSA99320/` folder at the top (eboot.bin, sce_sys/,
  sce_module/, soh.o2r, assets.zip with the extractor's recipes, licenses/), with fixed
  timestamps. The script refuses ROMs, `oot*.o2r`, saves and settings, checks the finished zip
  and prints its sha256.
- `soh.o2r` is included on purpose. It holds Ship of Harkinian's own fonts, shaders and
  textures (no Nintendo data), as every upstream SoH release does, and the game can't start
  without it.
- GitHub Actions (`.github/workflows/release.yml`) does the same in CI with the pinned PS5
  toolchain (cached between runs):
  1. Bump `contentVersion`, commit and push.
  2. Push a tag equal to it (`git tag 01.000.001 && git push origin 01.000.001`). The workflow
     checks that the tag matches, builds, verifies there is no ROM or `oot*.o2r` in the zip,
     and publishes a release with `PPSA99320.zip` and `PPSA99320.zip.sha256`.
  3. Running it by hand (Actions > release > Run workflow) builds and checks without
     publishing anything.
  Every run also uploads `catalog/PPSA99320.json` filled in with that build's sha256 as an
  artifact; submitting it anywhere is up to you.

## Licence

GPL-3.0. See [LICENSING.md](LICENSING.md).
