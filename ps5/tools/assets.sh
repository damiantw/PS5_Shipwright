#!/usr/bin/env bash
# Ship of Harkinian for the PlayStation 5 - the archives the title reads, made on this machine.
#
#   ps5/tools/assets.sh              soh.o2r (SoH's own assets) and assets/ (the extractor's
#                                    yml) into dist/<TITLE_ID>/
#   ps5/tools/assets.sh ROM.z64...   ... and oot.o2r / oot-mq.o2r from your own Ocarina of Time,
#                                    into game/<TITLE_ID>/ for tools/console-files.sh
#
# Built with this machine's compiler from Shipwright's own tools-only configure
# (SOH_TOOLS_ONLY: soh-o2r-packer and soh-torch). No game data goes into dist/.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
game="$root/vendor/shipwright"
build=${PS5_BUILD_DIR:-$root/build/game}
tools="$(dirname "$build")/host-tools"
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")
app="${PS5_DIST_DIR:-$root/dist}/$title_id"
bash "$ps5/tools/setup-source.sh"

if [[ ! -f $tools/build.ninja ]]; then
    mkdir -p "$tools"
    cmake -S "$game" -B "$tools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSOH_TOOLS_ONLY=ON \
        > "$tools/configure.log" 2>&1 || { tail -40 "$tools/configure.log" >&2; exit 1; }
fi
ninja -C "$tools" GenerateSohOtr soh-torch > "$tools/build.log" 2>&1 || { tail -40 "$tools/build.log" >&2; exit 1; }
mkdir -p "$app"
cp "$tools/soh/soh.o2r" "$app/soh.o2r"
rm -rf "$app/assets"
cp -R "$game/soh/assets/yml" "$app/assets"
printf '==> %s (%s bytes)\n' "$app/soh.o2r" "$(stat -c %s "$app/soh.o2r")"

if [[ $# -ge 1 ]]; then
    mkdir -p "$root/game/$title_id"
    version=$(sed -n 's/^project(Ship VERSION \([0-9.]*\).*/\1/p' "$game/CMakeLists.txt")
    "$tools/soh-torch" --src "$game/soh/assets/yml" --dest "$root/game/$title_id" --version "$version" "$@"
    rm -f "$root/game/$title_id/torch.hash.yml"
    ls -l "$root/game/$title_id"
fi
