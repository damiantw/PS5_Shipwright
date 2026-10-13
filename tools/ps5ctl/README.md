# ps5ctl: scripted PS5 Remote Play input and frame capture (headless)

Built on pyremoteplay 0.7.6 (venv /workspace/ps5env), with compatibility patches for PS5 fw 13.x, low-MTU VPN links and PyAV 19.
It reuses chiaki-ng's stored registration, so you don't need a new PIN:
`mkprofile.py` (not included in this repo: it embeds account data) turns ~/.config/Chiaki/Chiaki.conf (rp_key, rp_regist_key) into ~/.pyremoteplay/.profile.json (chmod 600).

    ./ps5ctl start                  # kills GUI chiaki (by process name), starts daemon, retries ~2 min while the console frees the slot
    ./ps5ctl status
    ./ps5ctl press CROSS --hold 200 [--measure]   # --measure: ms from press until the frame visibly changes
    ./ps5ctl seq 'RIGHT,RIGHT,WAIT500,CROSS:300' [--hold 120 --gap 250]
    ./ps5ctl down L2 / up L2        # hold a button indefinitely
    ./ps5ctl stick left 0.0 -1.0 --hold 500
    ./ps5ctl shot out.png           # latest decoded frame, 1280x720
    ./ps5ctl stop

Buttons: UP DOWN LEFT RIGHT CROSS(X) CIRCLE(O) SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 OPTIONS SHARE PS TOUCHPAD

Configuration (nothing user-specific is in the code): set `PS5_HOST` and `PS5_USER` (your PSN online ID, as keyed in ~/.pyremoteplay/.profile.json), or put them in an untracked `~/.config/ps5ctl/config.json`:

    {"host": "<console-ip>", "psn_online_id": "<online-id>", "psn_account_id": "<decimal account id>"}

(`PS5CTL_CONFIG` overrides the path; `psn_account_id`/`PSN_ACCOUNT_ID` is only used by mkprofile.py.)

Daemon log: /tmp/ps5ctld.log. Socket: /tmp/ps5ctl.sock. Env vars: PS5_HOST, PS5_RES (720p), PS5_QUALITY (low), PS5_MTU (1200), PS5CTL_LOG=DEBUG.

Patches (in ps5ctld.py unless noted):
- fallback ctrl session id: fw 13.x never sends 0x33 (chiaki-ng does the same)
- encrypt ctrl message payloads (upstream throws the ciphertext away)
- cap MTU at 1200: VPN links with MTU 1280, so at 1454 every full-size video packet was dropped
- reorder-tolerant frame reassembly, plus IDR requests while no picture is available
- site-packages: PyAV 19 flag names (receiver/__init__.py), SO_RCVBUF 8 MB (stream.py)

    ./ps5ctl wake                   # Remote Play wake from rest mode (registered credentials)

`wake` only acts when the console is in rest mode (it reports "already_on" otherwise) and
waits up to two minutes for it to answer. HDMI Device Link (CEC) is switched off on the
console, so a wake does not turn the TV on.
