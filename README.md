# Ship of Harkinian for PlayStation 5

A PS5 homebrew port of [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)
(Shipwright) on [libultraship](https://github.com/Kenix3/libultraship). It draws through
libultraship's Vulkan renderer on RADV, using Mihawk's
[PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan) stack.

**Status:** boots, plays and saves on a retail PS5 with a homebrew environment
(tested 2026-10-12). See [Known issues](#known-issues).

**No ROM or game data is in this repository or in any build of it.** You supply your own
Ocarina of Time ROM. Never commit a ROM (`*.z64`, `*.n64`, `*.v64`) or an `.o2r` made from
one; `.gitignore` blocks them.

## Credits

- **[griches/PS5_Ghostship](https://github.com/griches/PS5_Ghostship)** (Gary Riches): this
  repository's layout, the console layer (`ps5/`: SDL2 video/audio/pad drivers, shims,
  start-up, link recipe, packaging) and the libultraship Vulkan changes for the PS5 all
  come from that port of Ghostship.
- **[mihawk-99](https://github.com/mihawk-99)** (Mihawk): PS5_Vulkan, PS5_Mesa (RADV on the
  PS5), PS5_PayloadSDK and PS5_VulkanTemplate, which everything here builds on.
- [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) (John Törnblom), HarbourMasters,
  Kenix3 and the libultraship contributors.
- The Vulkan backend comes from libultraship commit `98f9a473` ("Implemented vulkan backend",
  KiritoDv), cherry-picked onto Shipwright's libultraship pin, plus PS5_Ghostship's PS5 hunks.

## Requirements

**Build machine (Linux, x86-64):**
- clang/LLVM 18+, cmake, ninja, python3, git, plus `ffmpeg` and ImageMagick for the title art
- [PS5_PayloadSDK](https://github.com/mihawk-99), [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)
  and PS5_Mesa, checked out **beside** this folder, with RADV built
  (`PS5_VulkanTemplate/ps5/tools/bootstrap.sh`)

**Console:**
- a PS5 running a homebrew environment (etaHEN or equivalent, after the exploit)
- ShadowMountPlus, which registers `/data/homebrew/<TITLE_ID>` as a title and mounts `/data` into its sandbox
- an FTP server on port 2121
- optional: klogsrv on port 3232 for logs, and PS5_Vulkan's control helper on port 9111

## Layout

| Path | What |
| --- | --- |
| `vendor/shipwright` | Submodule: HarbourMasters/Shipwright at an upstream revision |
| `ps5/patches/*.patch` | All PS5 changes to Shipwright, libultraship, torch and SDL. Vendor trees are never committed. |
| `ps5/src/` | Console layer: start-up, data folder, shims, SDL drivers |
| `ps5/cmake/ps5-game.cmake` | Injected after `project(Ship)`: SDL2, RADV, volk, shaderc, the link |
| `ps5/tools/` | setup, build, assets, deploy and run scripts |
| `ps5/sce_sys/`, `ps5/art/` | Title metadata (**PPSA99320**), icon and background with their sources |
| `tools/ps5ctl/` | Headless Remote Play controller used for testing on hardware |

## Building

```sh
git submodule update --init --recursive
bash ps5/tools/build.sh     # setup-source.sh applies ps5/patches; then configures and builds soh
bash ps5/tools/assets.sh    # soh.o2r and the extractor's yml (host compiler)
```

The output is `dist/PPSA99320/`, containing `eboot.bin` (about 68 MB), `sce_sys/`, `sce_module/`,
`soh.o2r` and `assets/`.

When you change files under `vendor/` (Shipwright, libultraship, torch), save them back into
the patches before committing:

```sh
bash ps5/tools/setup-source.sh --save
```

### Making oot.o2r from your own ROM

```sh
bash ps5/tools/assets.sh /path/to/your/OoT.z64 [/path/to/your/OoT-MQ.z64]
```

This writes `oot.o2r` (and `oot-mq.o2r`) to `game/PPSA99320/`, which is git-ignored. Use a ROM
version that Shipwright supports (see Shipwright's docs for the accepted hashes). Copy the
`.o2r` to the console's data folder (below). Do not commit or distribute it.

## Installing / deploying

```sh
PS5_HOST=192.168.1.70 bash ps5/tools/deploy.sh
```

`deploy.sh` uses PS5_Vulkan's title-folder helper if the console answers on port 9111.
Otherwise it falls back to plain FTP (`ps5/tools/ftp-deploy.py`, port 2121):
- Only changed files are uploaded, judged by size; `eboot.bin` is always uploaded.
- Each file goes to a temporary name and is then renamed into place.
- `eboot.bin` and `param.json` go last.

The first full upload includes about 20,000 small yml files and takes a long time over FTP.
For later updates, use:

```sh
python3 ps5/tools/ftp-deploy.py --only eboot.bin --only sce_sys dist/PPSA99320
```

The title lives at `/data/homebrew/PPSA99320/`. ShadowMountPlus shows it as "Ship of Harkinian".
Close the game before you overwrite `eboot.bin`.

## Data folder

Saves, settings, logs and game archives live **outside** the title folder, at:

```
/data/soh/
  oot.o2r, oot-mq.o2r     your archives (copy them here)
  shipofharkinian.json    all settings (CVars, window/backend)
  imgui.ini               menu window layout
  Save/                   global.sav, file1.sav ... (game saves)
  logs/                   Ship of Harkinian.log
  mods/                   .o2r / .otr mods
  .migrated               marker: old /app0/userdata contents were copied once
```

Why `/data/soh`:
- Reinstalling or replacing `/data/homebrew/PPSA99320` (a new build, a fresh FTP upload, a
  ShadowMountPlus re-scan) never touches your saves or settings.
- ShadowMountPlus mounts `/data` into the title's sandbox, so the path is real and writable
  from inside the game.

Details:
- At start-up the game checks that the folder can be created and written. If it can't, the
  game falls back to `/app0/userdata` (the old location inside the title folder).
- On first run it copies an existing `/app0/userdata` across once. It never overwrites files.
- To use another folder, add `PS5_DATA_DIR=/data/whatever` to `env.txt` in the title folder.

Persistence was checked on hardware: save in game, close, redeploy a new `eboot.bin`,
relaunch, load. The file and the rupee count survived.

## Controls

The game uses the DualSense with SoH's default N64 mapping. Cross is A, Circle/Square is B,
Options is Start, and the right stick is the C buttons.

**Menu (SoH's ImGui settings menu):**

| Button | Action |
| --- | --- |
| **Touchpad click** | open / close the menu (the same binding as PS5_Ghostship) |
| D-pad / left stick | move focus (an amber frame marks the focused item) |
| Cross | activate / enter a panel |
| **Circle** | back out; closes the menu when nothing is active |
| L1 / R1 | switch tabs (ImGui) |

Controller navigation is on by default.

**Console-friendly menu (in `ps5/patches`, behind `__PROSPERO__`):**
- TV-sized scale. The fonts are rasterised at 3x so text stays sharp; the scale options are
  1.5 / 2.0 / 2.5 / 3.0, with "Large" as the default.
- Larger padding and spacing.
- A high-contrast amber focus highlight.
- Search lives in the sidebar, so the header tabs fit at TV scale.
- PS5 button names (Cross, Circle, Square, Triangle, L1/R1/L2/R2, Options, Touchpad) in
  the binding UI.
- Desktop-only items are hidden: fullscreen toggles, "Open App Files Folder", the mouse-cursor
  option and the controller-navigation toggle.
- Existing configs are migrated once (`gSettings.PS5.MenuDefaults`).

## Testing with ps5ctl

`tools/ps5ctl/` drives the console over Remote Play from a Linux box, with no GUI. See its
README; it needs pyremoteplay and an existing registration.

```sh
tools/ps5ctl/ps5ctl start            # connect (only while the console is on)
tools/ps5ctl/ps5ctl press TOUCHPAD   # open the SoH menu
tools/ps5ctl/ps5ctl stick left 0 -1 --hold 2000
tools/ps5ctl/ps5ctl shot shot.png
tools/ps5ctl/ps5ctl wake             # wake from rest mode; does nothing if already on
```

The Remote Play video lags several seconds (up to about 40 s while the game runs). Wait
before you screenshot, and check each step before sending the next input.

HDMI Device Link (CEC) is switched off on the test console, so `wake` does not turn on the TV.

## Known issues

- The PS5 system UI (PS button, control center) is very sluggish while the game runs.
- The home-screen tile can keep showing an old icon after `sce_sys` changes; the shell caches
  title art. A console restart, or re-registering through ShadowMountPlus, refreshes it.
- Graphics menu: "Renderer API" shows OpenGL although the backend is Vulkan (see
  `Window.Backend` in the json), and "Allow multi-windows" is a desktop option. Leave both
  alone.
- In the menu, moving focus from the sidebar into the right-hand panel with the d-pad is
  awkward (it is a separate child window).
- Pad input can reach the game while the menu is open on the title screen (a Cross press can
  start file select).
- The sun / lens-flare depth fix (empty `GetPixelDepth` outside a frame) has not been checked
  in a scene with the sun visible.
- Network: `gethostbyname` is a stub, so use numeric addresses.
- `package-release.sh` and `run.sh` are still PS5_Ghostship's and are untested.

## Troubleshooting

- **Crash at boot:** capture klog (`nc <console> 3232`). Crash addresses are the ELF address
  plus 0x400000, so subtract that and run `llvm-addr2line -e build/game/soh/soh.elf`.
- **"oot.o2r not found" or the game exits at once:** put `oot.o2r` in `/data/soh/`. Check that
  klog shows `data folder: /data/soh`.
- **Settings or saves "reset":** check whether klog reports the fallback `/app0/userdata`. That
  means `/data/soh` wasn't writable (for example, ShadowMountPlus isn't running).
- **The menu won't open:** click the touchpad itself (not a swipe). klog logs
  `PS5 touch pad down/up`.
- **build.sh fails on missing vendor trees:** run `git submodule update --init --recursive`.
  Check that PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK are beside this folder.
- **Deploy hangs at 9111:** that helper is optional, and `deploy.sh` falls back to FTP.
  `PS5_HOST` and `PS5_FTP_PORT` override the defaults.

## Licence

GPL-3.0 as built: see [LICENSING.md](LICENSING.md).
