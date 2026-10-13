#!/usr/bin/env bash
# Ship of Harkinian for the PlayStation 5 - the release archive for the PS5 Homebrew Catalog.
#
#   ps5/tools/release.sh            build dist/PPSA99320.zip from dist/PPSA99320/
#   ps5/tools/release.sh --no-build pack what build.sh last made
#
# The zip holds one folder, PPSA99320/, at its top level, as the console takes it:
# eboot.bin, sce_sys/ (param.json and the art, exactly ps5/sce_sys/), sce_module/,
# soh.o2r (Ship of Harkinian's own fonts, shaders and textures, shipped in every
# upstream release), assets/ (the extractor's recipes, no game data) and licenses/.
# No ROM and no game archive (oot.o2r, oot-mq.o2r) or other Nintendo data: the
# script refuses if one is in the folder, and checks the zip afterwards.
# It prints the zip's sha256 for catalog/PPSA99320.json.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
param="$ps5/sce_sys/param.json"
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["contentVersion"])' "$param")
[[ $version =~ ^[0-9]{2}\.[0-9]{3}\.[0-9]{3}$ ]] || { echo "contentVersion $version is not NN.NNN.NNN" >&2; exit 2; }
[[ ${1:-} == --no-build ]] || "$ps5/tools/build.sh"
app="$root/dist/$title_id"
for file in eboot.bin sce_sys/param.json sce_sys/icon0.png sce_sys/pic0.dds sce_sys/pic1.dds sce_module/libc.prx \
        soh.o2r assets/config.yml; do
    [[ -e $app/$file ]] || { echo "missing $app/$file: run ps5/tools/build.sh" >&2; exit 2; }
done
for file in param.json icon0.png pic0.dds pic1.dds; do
    cmp -s "$ps5/sce_sys/$file" "$app/sce_sys/$file" || { echo "dist's sce_sys/$file is not ps5/sce_sys/$file: run build.sh" >&2; exit 2; }
done
# never game data, saves or settings
forbidden='\.(z64|n64|v64|rom)$|(^|/)oot(-mq)?\.o2r$|(^|/)mq\.o2r$|\.otr$|(^|/)(userdata|Save|mods|radv-shader-cache)(/|$)|shipofharkinian\.json$|env\.txt$'
stray=$(cd "$app" && find . -mindepth 1 | sed 's|^\./||' | grep -E -i "$forbidden" | head -1 || true)
[[ -z $stray ]] || { echo "refusing: $stray is game data, a save or a setting" >&2; exit 2; }
cp "$root/licenses/GPL-3.0.txt" "$root/licenses/MIT.txt" "$root/licenses/Zlib.txt" "$app/licenses/"
out="$root/dist/$title_id.zip"
rm -f "$out"
python3 - "$root/dist" "$title_id" "$out" <<'PY'
import os, sys, zipfile
dist, title, out = sys.argv[1:4]
skip = {".DS_Store"}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
    for base, dirs, files in os.walk(os.path.join(dist, title)):
        dirs[:] = sorted(d for d in dirs if d not in ("userdata", "radv-shader-cache"))
        for name in sorted(files):
            if name in skip:
                continue
            path = os.path.join(base, name)
            info = zipfile.ZipInfo(os.path.relpath(path, dist), (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, open(path, "rb").read())
PY
# the archive itself: one top-level folder, nothing forbidden
python3 - "$out" "$title_id" "$forbidden" <<'PY'
import re, sys, zipfile
out, title, forbidden = sys.argv[1], sys.argv[2], re.compile(sys.argv[3], re.I)
names = zipfile.ZipFile(out).namelist()
tops = {n.split("/")[0] for n in names}
assert tops == {title}, f"top level is {sorted(tops)}, not {title}/"
bad = [n for n in names if forbidden.search(n.rstrip("/"))]
assert not bad, f"forbidden entries: {bad[:5]}"
for need in ("eboot.bin", "sce_sys/param.json", "sce_sys/icon0.png", "soh.o2r"):
    assert f"{title}/{need}" in names, f"{need} missing"
print(f"checked: {len(names)} entries under {title}/, no ROM or game archive")
PY
printf '==> %s (%s bytes, contentVersion %s)\n' "$out" "$(stat -c %s "$out")" "$version"
printf 'sha256 %s\n' "$(sha256sum "$out" | cut -d' ' -f1)"
