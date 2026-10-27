#!/usr/bin/env python3
"""Ship of Harkinian for the PlayStation 5 - the debug server's client (DEVTOOLS.md).

A dev build (PS5_DEVTOOLS=1 ps5/tools/build.sh) with /data/soh/devtools.enable on the
console answers one JSON object per line, over whichever transport gets through:

    tcp://192.168.1.70:9200      the title listens (default)
    reverse://0.0.0.0:9201       the title dials out to this machine (connect=IP:9201
                                 in devtools.enable); we listen and wait for it
    mailbox://192.168.1.70:2121  files in /data/soh/devtools/mailbox over the console's FTP

    from sohdev import Client
    c = Client()                                   # SOHDEV_URL, else tcp://192.168.1.70:9200
    c.warp("LOST_WOODS_BRIDGE_EAST_EXIT", age="child", time="noon")   # waits for the scene
    c.press("A"); c.stick(0, 80, frames=40)        # game frames (20 a second)
    print(c.snapshot()["pos"]); c.screenshot("bridge.png")

The command line: sohdev.py --help.

SPDX-License-Identifier: MIT
"""
import argparse, base64, ftplib, io, json, os, socket, struct, subprocess, sys, time, uuid, zlib

DEFAULT_URL = os.environ.get("SOHDEV_URL", "tcp://%s:9200" % os.environ.get("SOHDEV_HOST", "192.168.1.70"))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
BUTTONS = ["A", "B", "Z", "START", "DUP", "DDOWN", "DLEFT", "DRIGHT", "L", "R", "CUP", "CDOWN", "CLEFT", "CRIGHT"]


class DevError(Exception):
    def __init__(self, reply):
        self.reply = reply
        super().__init__(reply.get("error", "error"))


# ------------------------------------------------------------------ transports
class LineSocket:
    def __init__(self, sock, timeout):
        self.sock = sock
        self.sock.settimeout(timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def request(self, line):
        self.sock.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise ConnectionError("the console closed the connection (did the game crash?)")
            self.buf += chunk
        out, self.buf = self.buf.split(b"\n", 1)
        return out.decode()

    def close(self):
        self.sock.close()


def tcp_transport(host, port, timeout):
    return LineSocket(socket.create_connection((host, port), timeout=10), timeout)


def reverse_transport(host, port, timeout, accept_timeout=120):
    """We listen; the title connects to us (connect=OUR_IP:PORT in devtools.enable)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host or "0.0.0.0", port))
    srv.listen(1)
    srv.settimeout(accept_timeout)
    try:
        conn, peer = srv.accept()
    finally:
        srv.close()
    print("sohdev: the console connected from %s:%d" % peer, file=sys.stderr)
    return LineSocket(conn, timeout)


class Mailbox:
    """Request files over the console's FTP server: in/NAME.req -> out/NAME.resp."""

    def __init__(self, host, port, timeout, data_dir="/data/soh"):
        self.host, self.port, self.timeout = host, port, timeout
        self.base = data_dir + "/devtools/mailbox"
        self.ftp = None

    def _ftp(self):
        if self.ftp is None:
            self.ftp = ftplib.FTP()
            self.ftp.connect(self.host, self.port, timeout=15)
            self.ftp.login()
            self.ftp.set_pasv(True)
        return self.ftp

    def request(self, line):
        name = "%d-%s" % (time.time() * 1000, uuid.uuid4().hex[:6])
        ftp = self._ftp()
        tmp, req = "%s/in/%s.tmp" % (self.base, name), "%s/in/%s.req" % (self.base, name)
        ftp.storbinary("STOR " + tmp, io.BytesIO(line.encode() + b"\n"))
        try:
            ftp.rename(tmp, req)
        except ftplib.all_errors:
            ftp.storbinary("STOR " + req, io.BytesIO(line.encode() + b"\n"))
        resp = "%s/out/%s.resp" % (self.base, name)
        start = time.time()
        while time.time() - start < self.timeout:
            buf = io.BytesIO()
            try:
                ftp.retrbinary("RETR " + resp, buf.write)
                try:
                    ftp.delete(resp)
                except ftplib.all_errors:
                    pass
                return buf.getvalue().decode().strip().splitlines()[-1]
            except ftplib.error_perm:
                time.sleep(0.1)
        raise TimeoutError("no reply in %s after %d s (is a dev build running with devtools.enable?)"
                           % (resp, self.timeout))

    def close(self):
        if self.ftp:
            try:
                self.ftp.quit()
            except ftplib.all_errors:
                pass


def open_transport(url, timeout):
    scheme, _, rest = url.partition("://")
    if not rest:
        scheme, rest = "tcp", url
    host, _, port = rest.rpartition(":")
    if not host:
        host, port = rest, ""
    if scheme == "tcp":
        return tcp_transport(host, int(port or 9200), timeout)
    if scheme == "reverse":
        return reverse_transport(host, int(port or 9201), timeout)
    if scheme == "mailbox":
        return Mailbox(host, int(port or 2121), timeout)
    raise ValueError("transport %r: tcp://, reverse:// or mailbox://" % url)


# ------------------------------------------------------------------ the client

def fix_png(data):
    """Dev builds up to 9620b4cd wrote IEND with a CRC of 0; mend it so viewers take the file."""
    iend = b"\x00\x00\x00\x00IEND"
    if data.endswith(iend + b"\x00\x00\x00\x00"):
        data = data[:-4] + struct.pack(">I", zlib.crc32(b"IEND"))
    return data

class Client:
    def __init__(self, url=DEFAULT_URL, token=None, timeout=120):
        self.url = url
        self.t = open_transport(url, timeout)
        self.next_id = 1
        self._symbols = None
        token = token or os.environ.get("SOHDEV_TOKEN")
        if token:
            self.call("auth", token=token)

    def close(self):
        self.t.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def call(self, cmd, **args):
        req = {"id": self.next_id, "cmd": cmd}
        req.update({k: v for k, v in args.items() if v is not None})
        self.next_id += 1
        reply = json.loads(self.t.request(json.dumps(req)))
        if not reply.get("ok"):
            raise DevError(reply)
        return reply

    # reading
    def ping(self):
        return self.call("ping")

    def version(self):
        return self.call("version")

    def snapshot(self):
        """Taken once a game frame, without waiting for the game thread."""
        return self.call("snapshot")

    def state(self, actors=True):
        return self.call("state", actors=actors)

    def flags(self, scene=None):
        return self.call("flags", scene=scene)

    def log(self, lines=50):
        return self.call("log", lines=lines)["lines"]

    def perf(self, reset=False, recent=False):
        return self.call("perf", reset=reset, recent=recent)

    def perf_reset(self):
        return self.call("perf", reset=True)

    # waiting
    def wait(self, timeout=30, **cond):
        """On the console: state=, scene=, room=, entrance=, loaded=, state_frames=,
        input_idle=, frames= (game frames), seconds=. All must hold."""
        return self.call("wait", timeout=timeout, **cond)

    def frames(self, n):
        return self.wait(frames=n, timeout=max(30, n / 10))

    def sleep(self, seconds):
        return self.wait(seconds=seconds, timeout=seconds + 30)

    def wait_until(self, pred, timeout=30, poll=0.05):
        """Any condition on the snapshot, polled from here."""
        start = time.time()
        while True:
            s = self.snapshot()
            if pred(s):
                return s
            if time.time() - start > timeout:
                raise TimeoutError("condition not met in %.0f s; last snapshot %s" % (timeout, s))
            time.sleep(poll)

    def wait_loaded(self, scene=None, timeout=60):
        cond = {"loaded": True, "state_frames": 3}
        if scene is not None:
            cond["scene"] = scene
        return self.wait(timeout=timeout, **cond)

    # input
    def input(self, steps, clear=False):
        """steps: [{buttons: [..] or a mask, stick: [x, y], rstick: [x, y], frames: n,
        mode: or|override}]. Sticks -80..80 (or -1.0..1.0), y up positive; game frames."""
        return self.call("input", steps=steps, clear=clear)

    def press(self, buttons, frames=2, release=2, wait=True):
        buttons = [buttons] if isinstance(buttons, str) else list(buttons)
        r = self.input([{"buttons": buttons, "frames": frames}, {"buttons": [], "frames": release}])
        if wait:
            self.wait(input_idle=True, timeout=30 + (frames + release) / 10)
        return r

    def stick(self, x, y, frames, buttons=(), wait=True):
        r = self.input([{"stick": [x, y], "buttons": list(buttons), "frames": frames}])
        if wait:
            self.wait(input_idle=True, timeout=30 + frames / 10)
        return r

    def input_clear(self):
        return self.call("input_clear")

    # changing the game
    def warp(self, entrance=None, scene=None, spawn=None, age=None, wait=True, timeout=60, **kw):
        """entrance: a number or a name (ENTR_ optional); or scene (number or name, SCENE_
        optional) and spawn. age: child|adult. time=: 0..65535, HH:MM, day, noon, evening,
        night, midnight. With wait, returns once the new scene runs, with load_seconds."""
        start = time.time()
        r = self.call("warp", entrance=entrance, scene=scene, spawn=spawn, age=age, time=kw.get("time"))
        if wait:
            w = self.wait_loaded(scene=r["scene"], timeout=timeout)
            r["load_seconds"] = time.time() - start
            r["snapshot"] = w["snapshot"]
        return r

    def cvar(self, name, value=None, type=None, save=False):
        return self.call("cvar", name=name, value=value, type=type, save=save)

    def cvar_clear(self, name):
        return self.call("cvar", name=name, clear=True)

    def save(self, slot=None):
        return self.call("save", slot=slot)

    def load(self, slot=0, wait=True, timeout=60):
        start = time.time()
        r = self.call("load", slot=slot)
        if wait:
            self.wait(state="play", loaded=True, state_frames=3, timeout=timeout)
            r["load_seconds"] = time.time() - start
        return r

    def console(self, line):
        return self.call("console", line=line)

    # memory
    def symbols(self, elf=None):
        """name -> (ELF offset, size), from the dev build's linked ELF (llvm-nm)."""
        if self._symbols is None:
            elf = elf or os.path.join(ROOT, "build", "last-title-dev.elf")
            nm = os.path.join(ROOT, ".deps/native/ps5-payload-sdk/bin/llvm-nm")
            if not os.path.exists(nm):
                nm = "llvm-nm"
            out = subprocess.run([nm, "--defined-only", "-S", elf], capture_output=True, text=True, check=True).stdout
            syms = {}
            for line in out.splitlines():
                p = line.split()
                if len(p) == 4:
                    syms[p[3]] = (int(p[0], 16), int(p[1], 16))
                elif len(p) == 3:
                    syms.setdefault(p[2], (int(p[0], 16), 0))
            self._symbols = syms
        return self._symbols

    def _where(self, addr=None, symbol=None, global_=None, offset=0):
        if symbol is not None:
            return {"eboot": self.symbols()[symbol][0], "offset": offset}
        if global_ is not None:
            return {"global": global_, "offset": offset}
        return {"addr": addr, "offset": offset}

    def mem_read(self, addr=None, length=16, symbol=None, global_=None, offset=0):
        r = self.call("mem_read", len=length, **self._where(addr, symbol, global_, offset))
        return bytes.fromhex(r["hex"])

    def mem_write(self, data, addr=None, symbol=None, global_=None, offset=0):
        return self.call("mem_write", hex=bytes(data).hex(), **self._where(addr, symbol, global_, offset))

    # pictures
    def screenshot(self, path=None, max_width=1280, on_console=None):
        """The frame on screen as a PNG: path saves it here, on_console names a file in
        /data/soh/devtools/ on the console. max_width 0 is full size (3840)."""
        r = self.call("screenshot", max_width=max_width, name=on_console, inline=path is not None)
        if path:
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            with open(path, "wb") as f:
                f.write(fix_png(base64.b64decode(r.pop("png_base64"))))
            r["local_path"] = path
        return r

    def crash(self):
        """Tests the crash handler: the game dies (a write to address 8)."""
        try:
            self.call("crash", confirm=True)
        except (ConnectionError, OSError, TimeoutError):
            pass


# ------------------------------------------------------------------ command line
def _num(s):
    try:
        return int(s, 0)
    except (TypeError, ValueError):
        return s


def main(argv=None):
    ap = argparse.ArgumentParser(description="drive a dev build's debug server (DEVTOOLS.md)")
    ap.add_argument("--url", default=DEFAULT_URL, help="tcp://HOST:9200, reverse://0.0.0.0:9201 or mailbox://HOST:2121")
    ap.add_argument("--token")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for n in ("ping", "version", "snap", "globals", "input-clear"):
        sub.add_parser(n)
    p = sub.add_parser("state"); p.add_argument("--no-actors", action="store_true")
    p = sub.add_parser("flags"); p.add_argument("--scene", type=_num)
    p = sub.add_parser("warp", help="ENTRANCE (number or name) or --scene S [--spawn N]")
    p.add_argument("entrance", nargs="?", type=_num); p.add_argument("--scene", type=_num); p.add_argument("--spawn", type=int)
    p.add_argument("--age", choices=["child", "adult"]); p.add_argument("--time"); p.add_argument("--no-wait", action="store_true")
    p = sub.add_parser("press", help="BUTTON[,BUTTON] [FRAMES]"); p.add_argument("buttons"); p.add_argument("frames", nargs="?", type=int, default=2)
    p = sub.add_parser("stick", help="X Y FRAMES (-80..80, y up)"); p.add_argument("x", type=int); p.add_argument("y", type=int); p.add_argument("frames", type=int)
    p = sub.add_parser("input", help="a JSON list of steps"); p.add_argument("steps")
    p = sub.add_parser("shot", help="a screenshot to a local PNG"); p.add_argument("path"); p.add_argument("--max-width", type=int, default=1280)
    p = sub.add_parser("perf"); p.add_argument("--reset", action="store_true"); p.add_argument("--recent", action="store_true")
    p = sub.add_parser("cvar"); p.add_argument("name"); p.add_argument("value", nargs="?"); p.add_argument("--save", action="store_true")
    p = sub.add_parser("mem", help="ADDR|SYMBOL [LEN]"); p.add_argument("where"); p.add_argument("len", nargs="?", type=int, default=64)
    p = sub.add_parser("save"); p.add_argument("slot", nargs="?", type=int)
    p = sub.add_parser("load"); p.add_argument("slot", type=int)
    p = sub.add_parser("console", help="a SoH console command line"); p.add_argument("line")
    p = sub.add_parser("wait", help="key=value ... (state=play loaded=true scene=0x5b frames=20)")
    p.add_argument("cond", nargs="*"); p.add_argument("--timeout", type=float, default=30)
    p = sub.add_parser("log"); p.add_argument("lines", nargs="?", type=int, default=40)
    p = sub.add_parser("raw", help="a JSON request"); p.add_argument("json")
    sub.add_parser("crash", help="crash the game on purpose (tests the crash handler)")
    a = ap.parse_args(argv)
    c = Client(a.url, a.token)
    cmd, out = a.cmd, None
    if cmd == "ping": out = c.ping()
    elif cmd == "version": out = c.version()
    elif cmd == "snap": out = c.snapshot()
    elif cmd == "globals": out = c.call("globals")
    elif cmd == "input-clear": out = c.input_clear()
    elif cmd == "state": out = c.state(actors=not a.no_actors)
    elif cmd == "flags": out = c.flags(a.scene)
    elif cmd == "warp":
        out = c.warp(a.entrance, scene=a.scene, spawn=a.spawn, age=a.age, time=_num(a.time), wait=not a.no_wait)
    elif cmd == "press": out = c.press(a.buttons.split(","), a.frames)
    elif cmd == "stick": out = c.stick(a.x, a.y, a.frames)
    elif cmd == "input": out = c.input(json.loads(a.steps))
    elif cmd == "shot": out = c.screenshot(a.path, a.max_width)
    elif cmd == "perf": out = c.perf(a.reset, a.recent)
    elif cmd == "cvar":
        v = a.value
        if v is not None:
            try:
                v = json.loads(v)
            except ValueError:
                pass
        out = c.cvar(a.name, v, save=a.save)
    elif cmd == "mem":
        w = _num(a.where)
        data = c.mem_read(addr=w, length=a.len) if isinstance(w, int) else c.mem_read(symbol=w, length=a.len)
        for i in range(0, len(data), 16):
            print("%04x  %s" % (i, data[i:i + 16].hex(" ")))
        return 0
    elif cmd == "save": out = c.save(a.slot)
    elif cmd == "load": out = c.load(a.slot)
    elif cmd == "console": out = c.console(a.line)
    elif cmd == "wait":
        cond = {}
        for kv in a.cond:
            k, v = kv.split("=", 1)
            cond[k] = {"true": True, "false": False}.get(v.lower(), _num(v))
        out = c.wait(timeout=a.timeout, **cond)
    elif cmd == "log":
        print("\n".join(c.log(a.lines)))
        return 0
    elif cmd == "raw":
        req = json.loads(a.json)
        out = c.call(req.pop("cmd"), **req)
    elif cmd == "crash":
        c.crash()
        print("crash requested: see /data/soh/logs/crash-*.txt")
        return 0
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except DevError as e:
        print("error: %s" % json.dumps(e.reply), file=sys.stderr)
        sys.exit(1)
