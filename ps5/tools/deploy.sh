#!/usr/bin/env bash
# Ship of Harkinian for the PlayStation 5 - upload dist/<TITLE_ID>/ to the console.
#
#   PS5_HOST=192.168.1.70 ps5/tools/deploy.sh [--all]
#
# Uses PS5_Vulkan's deploy tool (it refuses while the title runs) when its
# ps5vkctl helper answers on port 9111; otherwise plain FTP on port 2121
# (ps5/tools/ftp-deploy.py). Close the title before deploying either way.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$ps5/sce_sys/param.json")
[[ -d $root/dist/$title_id ]] || { echo "no dist/$title_id: run ps5/tools/build.sh first" >&2; exit 2; }
vulkan=${PS5_VULKAN_DIR:-$root/../PS5_Vulkan}
if [[ -n ${PS5_HOST:-} && -f $vulkan/tools/deploy-title-folder.py ]] &&
   timeout 3 bash -c "</dev/tcp/$PS5_HOST/${PS5VKCTL_PORT:-9111}" 2>/dev/null; then
    exec python3 "$vulkan/tools/deploy-title-folder.py" --always eboot.bin "$root/dist/$title_id" "$@"
fi
echo "deploy.sh: no ps5vkctl helper on ${PS5_HOST:-?}:${PS5VKCTL_PORT:-9111}; uploading over FTP" >&2
exec python3 "$ps5/tools/ftp-deploy.py" --always eboot.bin "$root/dist/$title_id" "$@"
