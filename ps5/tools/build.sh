#!/usr/bin/env bash
# Ship of Harkinian for the PlayStation 5 - build the game into dist/<TITLE_ID>/.
#
#   ps5/tools/build.sh [ninja arguments]
#
# Applies the console's patches to vendor/shipwright (tools/setup-source.sh),
# configures it with the payload SDK's CMake toolchain (once) and
# ps5/cmake/ps5-game.cmake, which gives it SDL2 with the console's drivers and
# the title's link (tools/link-title.sh). The title folder gets eboot.bin,
# sce_sys/, soh.o2r and the licences. No game data: oot.o2r is made from your
# own ROM (ps5/tools/assets.sh) and is never part of the build.
#
#   PS5_BUILD_DIR    the build tree (default build/game; the Linux VM's own disk
#                    is several times faster than the shared folder)
#   PS5_VULKAN_DIR   the PS5_Vulkan checkout (default ../PS5_Vulkan)
#   PS5_DEVTOOLS=1   a dev build with the developer tools (DEVTOOLS.md); give it its own
#                    PS5_BUILD_DIR and PS5_DIST_DIR (default build/dev, dist-dev): never release it
#
# SPDX-License-Identifier: MIT
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}" && pwd)
bash "$ps5/tools/setup-source.sh"
sdl=$(bash "$ps5/tools/setup-sdl.sh")
shaderc=$(bash "$ps5/tools/setup-shaderc.sh")
sdk=$(bash "$ps5/tools/setup-sdk.sh")
devtools=OFF
if [[ ${PS5_DEVTOOLS:-0} == 1 ]]; then
    devtools=ON
    export PS5_DIST_DIR=${PS5_DIST_DIR:-$root/dist-dev}
    build=${PS5_BUILD_DIR:-$root/build/dev}
else
    build=${PS5_BUILD_DIR:-$root/build/game}
fi
ninja=$(command -v ninja || echo "$HOME/.local/bin/ninja")
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}

if [[ ! -f $build/build.ninja ]]; then
    mkdir -p "$build"
    cmake -S "$root/vendor/shipwright" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DCMAKE_PROJECT_Ship_INCLUDE="$ps5/cmake/ps5-game.cmake" \
        -DPS5_PORT_DIR="$root" -DPS5_VULKAN_DIR="$vulkan" -DSDL_DIR="$sdl" -DSHADERC_DIR="$shaderc" \
        -DPS5_DEVTOOLS=$devtools -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=OFF > "$build/configure.log" 2>&1 \
        || { tail -60 "$build/configure.log" >&2; exit 1; }
fi
# a tree configured for the other variant would build it under this one's name
dev_cache=OFF
grep -q "^PS5_DEVTOOLS:BOOL=ON\$" "$build/CMakeCache.txt" && dev_cache=ON
[[ $dev_cache == "$devtools" ]] || {
    echo "build.sh: $build is not configured with PS5_DEVTOOLS=$devtools (use another PS5_BUILD_DIR)" >&2
    exit 2
}
"$ninja" -C "$build" "${@:-soh}"

# The title's name and art, as this tree has them (the link copies them too, but
# only when something relinks)
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")
app="${PS5_DIST_DIR:-$root/dist}/$title_id"
if [[ -d $app/sce_sys ]]; then
    cp "$ps5/sce_sys/param.json" "$ps5/sce_sys/icon0.png" "$ps5/sce_sys/pic0.dds" "$ps5/sce_sys/pic1.dds" "$app/sce_sys/"
fi
# PlayStation button prompts (Kenney CC0 glyphs, no game data); assets.sh makes them too
if [[ -d $app ]]; then
    python3 "$ps5/tools/make-prompts.py" "$app/ps5_prompts.o2r"
fi
