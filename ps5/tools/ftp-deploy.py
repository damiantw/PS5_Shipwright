#!/usr/bin/env python3
# Ship of Harkinian for the PlayStation 5 - upload dist/<TITLE_ID>/ over plain FTP.
#
#   ps5/tools/ftp-deploy.py DIST_DIR [--all] [--always NAME ...] [--only PREFIX ...]
#
# Needs only the console's FTP server (PS5_HOST, PS5_FTP_PORT default 2121,
# PS5_FTP_USER default anonymous, PS5_FTP_PASS). Each file goes up under a
# hidden .name.upload and is renamed over its destination once complete;
# eboot.bin and sce_sys/param.json go last. Files whose size already matches
# are skipped unless --all or named with --always. Nothing is ever deleted
# on the console except the file being replaced, and the data folder
# (/data/soh-ps5) is never touched.
#
# SPDX-License-Identifier: MIT
import argparse, os, sys
from ftplib import FTP, error_perm, error_reply
from pathlib import Path

LAST = ("eboot.bin", "sce_sys/param.json")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("dist")
    p.add_argument("--all", action="store_true")
    p.add_argument("--always", action="append", default=[])
    p.add_argument("--only", action="append", default=[],
                   help="upload only paths under this prefix (eboot.bin, sce_sys, ...); repeatable")
    a = p.parse_args()
    host = os.environ.get("PS5_HOST")
    if not host:
        sys.exit("ftp-deploy: set PS5_HOST (the console's address)")
    dist = Path(a.dist).resolve()
    title = dist.name
    base = f"/data/homebrew/{title}"
    ftp = FTP()
    ftp.connect(host, int(os.environ.get("PS5_FTP_PORT", "2121")), timeout=60)
    ftp.login(os.environ.get("PS5_FTP_USER", "anonymous"), os.environ.get("PS5_FTP_PASS", ""))
    files = sorted(str(f.relative_to(dist)) for f in dist.rglob("*") if f.is_file())
    if a.only:
        files = [f for f in files if any(f == o or f.startswith(o.rstrip("/") + "/") for o in a.only)]
    files.sort(key=lambda f: f in LAST)
    made = set()
    listings = {}

    def remote_size(path):
        # one LIST a folder, not a SIZE a file: a release has some 20000 files
        folder, name = path.rsplit("/", 1)
        if folder not in listings:
            entries = {}
            lines = []
            try:
                ftp.retrlines(f"LIST {folder}", lines.append)
            except (error_perm, error_reply):
                pass
            for line in lines:
                parts = line.split(None, 8)
                if len(parts) == 9 and not line.startswith("d"):
                    entries[parts[8]] = int(parts[4])
            listings[folder] = entries
        return listings[folder].get(name)

    for rel in files:
        local = dist / rel
        remote = f"{base}/{rel}"
        size = remote_size(remote)
        if size == local.stat().st_size and not a.all and rel not in a.always:
            continue
        folder, name = remote.rsplit("/", 1)
        cur = ""
        for part in [x for x in folder.split("/") if x]:
            cur += "/" + part
            if cur not in made:
                try:
                    ftp.mkd(cur)
                except error_perm:
                    pass
                made.add(cur)
        tmp = f"{folder}/.{name}.upload"
        with open(local, "rb") as src:
            ftp.storbinary(f"STOR {tmp}", src, blocksize=1 << 20)
        try:
            ftp.sendcmd(f"DELE {remote}")
        except (error_perm, error_reply):
            pass
        ftp.rename(tmp, remote)
        if ftp.size(remote) != local.stat().st_size:
            sys.exit(f"ftp-deploy: {remote}: size mismatch after upload")
        print(f"uploaded {rel} ({local.stat().st_size} bytes)")
    ftp.quit()


if __name__ == "__main__":
    main()
