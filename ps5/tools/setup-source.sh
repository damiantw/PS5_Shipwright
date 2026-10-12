#!/usr/bin/env bash
# Ship of Harkinian for the PlayStation 5 - the game's sources, with the console's changes.
#
#   ps5/tools/setup-source.sh          apply ps5/patches/{shipwright,libultraship,torch}.patch
#   ps5/tools/setup-source.sh --save   write the trees' changes back to the patches
#
# vendor/ghostship is Ghostship at an upstream revision, with libultraship and
# Torch as its own submodules. What the console needs changed is kept here as
# patches, not as forks. A patch already applied is left alone, so this can run
# before every build.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
game="$root/vendor/shipwright"
trees=(shipwright:"$game" libultraship:"$game/libultraship" torch:"$game/torch")
[[ -f $game/libultraship/CMakeLists.txt ]] || git -C "$root" submodule update --init --recursive vendor/shipwright

if [[ ${1:-} == --save ]]; then
    for entry in "${trees[@]}"; do
        # new files too (the Vulkan renderer is new to Shipwright's libultraship pin)
        git -C "${entry#*:}" add -N -- . ':!properties.h' ':!soh/properties.h' ':!soh/assets/custom/shaders' ':!*.rej' 2>/dev/null || true
        # generated at configure time, never the console's change
        git -C "${entry#*:}" diff --ignore-submodules -- . ':!properties.h' ':!src/port/build.c' > "$root/ps5/patches/${entry%%:*}.patch"
    done
    exit 0
fi
for entry in "${trees[@]}"; do
    name=${entry%%:*} source=${entry#*:}
    patch="$root/ps5/patches/$name.patch"
    [[ -s $patch ]] || continue
    if git -C "$source" apply --reverse --check "$patch" 2>/dev/null; then
        continue # applied already
    fi
    git -C "$source" apply --check "$patch" || {
        echo "setup-source.sh: $name.patch does not apply to $source (a different upstream revision, or edits not yet saved with --save?)" >&2
        exit 2
    }
    git -C "$source" apply --whitespace=nowarn "$patch"
    echo "setup-source.sh: applied $name.patch" >&2
done
