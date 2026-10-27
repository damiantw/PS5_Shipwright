#!/usr/bin/env bash
# Ship of Harkinian for the PlayStation 5 - name a crash address.
#
#   ps5/tools/symbolize.sh ADDR...                  against build/last-title.elf
#   ps5/tools/symbolize.sh --elf FILE ADDR...       another build's ELF (a release's symbols asset)
#   ps5/tools/symbolize.sh --dev ADDR...            the last dev build (build/last-title-dev.elf)
#   ps5/tools/symbolize.sh --crash crash-*.txt      every eboot+0x... line of a crash report
#
# ADDR is what a crash report or the console's crash record shows:
#   eboot+0x1234   an offset into the ELF (the crash report's form)
#   0x401234       an address in the running title: the image base is taken off
#                  (--base, default 0x400000, the "base:" line of a crash report)
#   0x1234         below the base: already an offset
# The ELF must be the build that crashed: compare the report's "build:" line with the
# build_id.txt beside the ELF (a release's PPSA99320-symbols-*.tar.xz has both).
#
# SPDX-License-Identifier: MIT
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
root=$(dirname "$ps5")
elf=$root/build/last-title.elf
base=0x400000
crash=
addrs=()
while (($#)); do
    case $1 in
        --elf) elf=$2; shift 2 ;;
        --dev) elf=$root/build/last-title-dev.elf; shift ;;
        --base) base=$2; shift 2 ;;
        --crash) crash=$2; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) addrs+=("$1"); shift ;;
    esac
done
[[ -f $elf ]] || { echo "symbolize.sh: no ELF $elf (build it, or --elf the release's)" >&2; exit 2; }
a2l=$(command -v llvm-addr2line || command -v llvm-addr2line-19 || command -v llvm-addr2line-18 || true)
[[ -n $a2l ]] || { echo "symbolize.sh: no llvm-addr2line (apt install llvm)" >&2; exit 2; }
if [[ -n $crash ]]; then
    report_base=$(sed -n 's/^base: *\(0x[0-9a-fA-F]*\).*/\1/p' "$crash" | head -1)
    [[ -z $report_base ]] || base=$report_base
    mapfile -t found < <(grep -o 'eboot+0x[0-9a-fA-F][0-9a-fA-F]*' "$crash" | awk '!seen[$0]++')
    addrs+=("${found[@]}")
fi
((${#addrs[@]})) || { echo "usage: symbolize.sh [--elf FILE|--dev] [--base ADDR] ADDR... | --crash FILE" >&2; exit 2; }
for a in "${addrs[@]}"; do
    if [[ $a == eboot+* ]]; then
        off=$((${a#eboot+}))
    else
        off=$((a))
        ((off >= base)) && off=$((off - base))
    fi
    printf '%s  (eboot+0x%x)\n' "$a" "$off"
    "$a2l" -f -C -i -e "$elf" "$(printf '0x%x' "$off")" | paste - - | sed 's/^/    /;s/\t/  at  /'
done
