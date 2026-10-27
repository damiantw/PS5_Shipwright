#!/usr/bin/env bash
# Ship of Harkinian for the PlayStation 5 - the dev build's zip (never a release).
#
#   PS5_DEVTOOLS=1 ps5/tools/build.sh && PS5_DIST_DIR=dist-dev ps5/tools/assets.sh
#   ps5/tools/package-dev.sh        dist-dev/PPSA99320-dev.zip from dist-dev/PPSA99320/
#
# The same layout as the release zip (one PPSA99320/ folder), plus eboot.sym and
# DEVTOOLS_BUILD.txt. Installed like the release, it replaces it; the debug server
# still only starts with /data/soh/devtools.enable on the console (DEVTOOLS.md).
# No ROM or game archive goes in, as for the release.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")
dist=${PS5_DIST_DIR:-$root/dist-dev}
app=$dist/$title_id
[[ -f $app/eboot.bin && -f $app/DEVTOOLS_BUILD.txt ]] ||
    { echo "no dev build in $app: PS5_DEVTOOLS=1 ps5/tools/build.sh" >&2; exit 2; }
cp "$root/licenses/GPL-3.0.txt" "$root/licenses/MIT.txt" "$root/licenses/Zlib.txt" "$app/licenses/" 2>/dev/null || true
out=$dist/$title_id-dev.zip
rm -f "$out"
python3 - "$dist" "$title_id" "$out" <<'PY'
import os, re, sys, zipfile
dist, title, out = sys.argv[1:4]
bad = re.compile(r'\.(z64|n64|v64|rom|otr)$|(^|/)oot(-mq)?\.o2r$|(^|/)mq\.o2r$|shipofharkinian\.json$|env\.txt$', re.I)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
    for base, dirs, files in os.walk(os.path.join(dist, title)):
        dirs[:] = sorted(d for d in dirs if d not in ("userdata", "radv-shader-cache"))
        for name in sorted(files):
            path = os.path.join(base, name)
            rel = os.path.relpath(path, dist)
            if name == ".DS_Store":
                continue
            assert not bad.search(rel), f"refusing: {rel} is game data or a setting"
            info = zipfile.ZipInfo(rel, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, open(path, "rb").read())
PY
printf '==> %s (%s bytes; a dev build, not for release)\n' "$out" "$(stat -c %s "$out")"
