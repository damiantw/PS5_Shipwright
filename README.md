# Ship of Harkinian for PlayStation 5 (work in progress)

A PS5 homebrew port of [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)
(Shipwright) on [libultraship](https://github.com/Kenix3/libultraship), drawing through
libultraship's Vulkan renderer on RADV from Mihawk's
[PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan) stack.

**Status: not yet run on a console.** See [Status](#status). No ROM or game data is in
this repository or any build of it; the player supplies their own.

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

## Layout

| Path | What |
| --- | --- |
| `vendor/shipwright` | Submodule: HarbourMasters/Shipwright at an upstream revision |
| `ps5/patches/shipwright.patch` | PS5 changes to Shipwright (behind `__PROSPERO__` / `PS5`) |
| `ps5/patches/libultraship.patch` | The Vulkan backend for Shipwright's libultraship pin, plus PS5 changes |
| `ps5/patches/torch.patch`, `sdl.patch` | As in PS5_Ghostship |
| `ps5/cmake/ps5-game.cmake` | Injected after `project(Ship)`: SDL2, RADV headers, volk, shaderc, deps from source, the link |
| `ps5/tools/` | build, assets, link, deploy and run scripts |
| `ps5/sce_sys/` | Title metadata: **PPSA99320**, "Ship of Harkinian" |

## Build (Linux)

With `PS5_Vulkan`, `PS5_Mesa` and `PS5_PayloadSDK` checked out beside this folder and RADV
built (`PS5_VulkanTemplate/ps5/tools/bootstrap.sh`):

```sh
git submodule update --init --recursive
ps5/tools/build.sh        # applies ps5/patches, configures with the payload SDK, builds soh
ps5/tools/assets.sh       # soh.o2r and the extractor's yml, built with the host compiler
```

The title folder is `dist/PPSA99320/`.

## Install (once it runs)

The console needs a homebrew environment: etaHEN (or equivalent) after the exploit,
ShadowMountPlus, an FTP server (port 2121) and klogsrv (port 3232). Copy
`dist/PPSA99320/` to `/data/homebrew/PPSA99320/`, then put your own Ocarina of Time
`.z64` (a version Shipwright supports) beside `eboot.bin`, or a ready `oot.o2r` /
`oot-mq.o2r` into `/data/homebrew/PPSA99320/userdata/`.

## Status

- **Builds** (2026-10-12, on Linux): `dist/PPSA99320/eboot.bin` (about 68 MB, signed fake SELF),
  `sce_sys/`, `sce_module/libc.prx`, `soh.o2r` and `assets/` (the extractor's yml).
  Shipwright `a93fec26`, libultraship pin `62e973ae`, RADV at PS5_Mesa `47e9eb61ee7`.
- **Never run on a console.** Nothing below has been checked on hardware.
- libultraship: Shipwright's pin has no Vulkan backend. `ps5/patches/libultraship.patch`
  cherry-picks the 34 commits `62e973ae..98f9a473` from the libultraship fork network
  (KiritoDv's renderer work: GPU lighting/texgen, mipmapping, palettes, then the Vulkan
  backend). Five of them conflicted and were resolved hunk-by-hunk in favour of the picked
  commit, then fixed by hand: `G_INVAL_TEX_BY_PAL` moved to opcode 0x4C because Shipwright
  uses 0x4a for `G_SETTILESIZE_LERP`, and `ResolveResourceCached` / `mResolvedResourceCache`
  put back. **Upstream fixes on those lines may have been lost.** Check rendering first.
- Network: SDL2_net is built from source. `gethostbyname` / `gethostbyaddr` are stubs
  (`ps5/src/shims.c`), so only numeric addresses work.
- `ps5/tools/package-release.sh`, `deploy.sh` and `run.sh` are still PS5_Ghostship's and
  have not been adapted or tested.
- The icon and art are still PS5_Ghostship's placeholders.

## Licence

GPL-3.0 as built: see [LICENSING.md](LICENSING.md).
