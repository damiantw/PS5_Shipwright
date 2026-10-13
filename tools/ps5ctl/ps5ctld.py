#!/workspace/ps5env/bin/python
"""ps5ctl daemon: holds one pyremoteplay session, serves JSON commands on a unix socket."""
import asyncio, json, logging, os, sys, time, threading
from pyremoteplay import RPDevice
from pyremoteplay.receiver import QueueReceiver

# ---- compat patches for PS5 fw 13.x (mirrors chiaki-ng behaviour) ----
import base64, secrets
from struct import pack_into
from pyremoteplay.session import Session
def _build_msg(self, msg_type, payload=b""):
    if payload: payload = self._cipher.encrypt(payload)
    buf = bytearray(Session.HEADER_LENGTH + len(payload))
    pack_into(f"!IHxx{len(payload)}s", buf, 0, len(payload), msg_type, payload)
    return bytes(buf)
Session._build_msg = _build_msg
def _send_hb_response(self):
    self._send(self._build_msg(Session.MessageType.HEARTBEAT_RESPONSE))
Session._send_hb_response = _send_hb_response
_orig_start = Session.start
async def _start(self, *a, **k):
    async def fallback():
        await asyncio.sleep(4)
        if self._ready_event and not self._ready_event.is_set() and not self.is_stopped and self._protocol:
            sid = (str(int(time.monotonic())) + base64.b64encode(secrets.token_bytes(48)).decode()).encode()
            logging.getLogger("ps5ctld").warning("no ctrl session id from console; using chiaki-style fallback id")
            self._session_id = sid
            for t, p in ((0x36, b"\0\0\0\0"), (0x36, b"\0\0\0\0"), (0x910, b"\0\0\0\0")):
                self._send(self._build_msg(t, p))
            self._ready_event.set()
    asyncio.get_running_loop().create_task(fallback())
    return await _orig_start(self, *a, **k)
Session.start = _start

# ---- reorder-tolerant video/audio reassembly (pyremoteplay assumes strictly in-order UDP) ----
from pyremoteplay.av import AVStream
STATS = {}
def _avs_handle(self, packet):
    STATS[self._type+'_pkts'] = STATS.get(self._type+'_pkts', 0) + 1
    st = self.__dict__.setdefault("_rx", {"frames": {}, "done": set(), "newest": -1})
    fi = packet.frame_index
    self._received = (self._received + 1) & 0xFFFF
    if fi in st["done"]: return
    fr = st["frames"].setdefault(fi, {})
    st.setdefault('meta', {})[fi] = (packet.frame_length_src, packet.frame_length_fec)
    if packet.is_fec:
        st.setdefault('fec', {}).setdefault(fi, set()).add(packet.unit_index); return
    data = packet.data[: packet.frame_size_audio] if self._type == AVStream.TYPE_AUDIO else packet.data
    fr[packet.unit_index] = data
    n = packet.frame_length_src
    if len(fr) >= n and all(i in fr for i in range(n)):
        units = [fr[i] for i in range(n)]
        STATS[self._type+'_units_done'] = STATS.get(self._type+'_units_done', 0) + 1
        del st["frames"][fi]; st["done"].add(fi)
        if len(st["done"]) > 256: st["done"] = set(sorted(st["done"])[-128:])
        if self._type == AVStream.TYPE_AUDIO:
            self._callback_done(b"".join(units))
        else:
            if self._last_complete and fi != self._last_complete + 1 and fi > self._last_complete:
                self._callback_corrupt(self._last_complete + 1, fi)
            _buf = self._header + b"".join(u[2:] for u in units)
            if os.environ.get('PS5CTL_DUMP'): open('/tmp/v.h264','ab').write(_buf)
            self._callback_done(_buf)
        self._last_complete = max(self._last_complete, fi)
    # drop stale incomplete frames
    for old in [k for k in st["frames"] if k < fi - 8]:
        if self._type == 'video': STATS.setdefault('dropped', []).append((old, st['meta'].get(old), len(st['frames'][old]), len(st.get('fec', {}).get(old, ())))); STATS['dropped'] = STATS['dropped'][-6:]
        del st["frames"][old]; self._lost += 1
AVStream.handle = _avs_handle

# VPN links (e.g. MTU 1280): big video packets get dropped unless we cap the negotiated MTU
_orig_ss = Session._start_stream
def _start_stream(self, test=True, mtu=None, rtt=None):
    if not test:
        mtu = min(mtu or 1454, int(os.environ.get("PS5_MTU", "1200")))
        logging.getLogger("ps5ctld").info("forcing stream MTU %s", mtu)
    return _orig_ss(self, test=test, mtu=mtu, rtt=rtt)
Session._start_stream = _start_stream

SOCK = os.environ.get("PS5CTL_SOCK", "/tmp/ps5ctl.sock")
def _cfg():
    import json, pathlib
    p = pathlib.Path(os.environ.get("PS5CTL_CONFIG", pathlib.Path.home() / ".config/ps5ctl/config.json"))
    try:
        return json.loads(p.read_text())
    except FileNotFoundError:
        return {}
_C = _cfg()
HOST = os.environ.get("PS5_HOST") or _C.get("host")
USER = os.environ.get("PS5_USER") or _C.get("psn_online_id")
if not HOST or not USER:
    sys.exit("ps5ctld: set PS5_HOST and PS5_USER (PSN online ID in ~/.pyremoteplay/.profile.json), or host/psn_online_id in ~/.config/ps5ctl/config.json")
logging.basicConfig(level=getattr(logging, os.environ.get("PS5CTL_LOG","INFO")), format="%(asctime)s %(levelname)s %(name)s %(message)s")
log = logging.getLogger("ps5ctld")
ALIAS = {"X": "CROSS", "O": "CIRCLE", "TRI": "TRIANGLE", "SQ": "SQUARE", "START": "OPTIONS", "CREATE": "SHARE"}

class Rx(QueueReceiver):
    def __init__(self):
        super().__init__(max_video_frames=3, max_audio_frames=1)
        self.nframes = 0; self.last_t = 0
    def handle_video(self, frame):
        super().handle_video(frame); self.nframes += 1; self.last_t = time.time()
    def handle_audio(self, frame):
        pass

async def main():
    dev = RPDevice(HOST)
    st = dev.get_status(); log.info("status %s", st)
    if st.get("status-code") != 200:
        log.error("console not on/reachable"); sys.exit(2)
    # console needs a while to release a previous session; retry up to ~2 min
    for attempt in range(12):
        rx = Rx()
        sess = dev.create_session(USER, receiver=rx, resolution=os.environ.get("PS5_RES", "720p"), fps="low", quality=os.environ.get("PS5_QUALITY", "low"), codec="h264")
        try:
            ok = bool(sess) and await dev.connect()
        except Exception as e:
            ok = False; sess.error = repr(e)[:120]
        if ok: break
        log.warning("attempt %d failed: %s; retrying in 10s", attempt + 1, getattr(sess, "error", None))
        dev.disconnect(); await asyncio.sleep(10)
    else:
        log.error("session start failed"); sys.exit(3)
    t0 = time.time()
    while not sess.is_ready and time.time() - t0 < 20: await asyncio.sleep(0.1)
    if not sess.is_ready: log.error("session not ready: %s", sess.error); sys.exit(4)
    ctl = dev.controller; ctl.start()
    from pyremoteplay.takion_pb2 import TakionMessage
    def send_idr():
        m = TakionMessage(); m.type = TakionMessage.PayloadType.IDRREQUEST
        sess.stream.send_data(m.SerializeToString(), 1, 2, True)
    async def idr_keeper():
        # request a keyframe whenever we have no decodable picture (start-up, loss)
        while not sess.is_stopped:
            if rx.nframes == 0 or time.time() - rx.last_t > 2.0:
                try: send_idr(); log.info("IDR requested (frames=%d)", rx.nframes)
                except Exception as e: log.warning("idr req failed %r", e)
            await asyncio.sleep(1.5)
    asyncio.get_running_loop().create_task(idr_keeper())
    log.info("session ready")
    lock = asyncio.Lock()

    def thumb():
        f = rx.get_latest_video_frame()
        return None if f is None else f.to_image().convert("L").resize((160, 90))
    async def tap(btn, hold_ms, measure=False):
        from PIL import ImageChops, ImageStat
        ref = thumb() if measure else None
        b = ALIAS.get(btn.upper(), btn.upper())
        ctl.button(b, "press"); t = time.time()
        if ref is not None:
            lat = None
            while time.time() - t < 3:
                await asyncio.sleep(0.01)
                cur = thumb()
                if cur is not None and ImageStat.Stat(ImageChops.difference(ref, cur)).mean[0] > 2.0:
                    lat = round((time.time() - t) * 1000); break
            rem = hold_ms / 1000 - (time.time() - t)
            if rem > 0: await asyncio.sleep(rem)
            ctl.button(b, "release")
            return {"button": b, "held_ms": round((time.time() - t) * 1000, 1), "press_to_visible_change_ms": lat}
        await asyncio.sleep(hold_ms / 1000)
        ctl.button(b, "release")
        return {"button": b, "held_ms": round((time.time() - t) * 1000, 1)}

    def shot(path):
        f = rx.get_latest_video_frame()
        if f is None: return {"error": "no frame yet"}
        f.to_image().save(path)
        return {"path": os.path.abspath(path), "frame_age_ms": round((time.time() - rx.last_t) * 1000), "frames_total": rx.nframes, "size": [f.width, f.height]}

    async def handle(r, w):
        try:
            req = json.loads(await r.readline()); op = req.get("op"); res = {}
            async with lock:
                if op == "press":
                    res = await tap(req["button"], req.get("hold", 120), req.get("measure", False))
                elif op == "seq":
                    res = {"steps": []}
                    for item in req["items"]:
                        if item.upper().startswith("WAIT"):
                            await asyncio.sleep(int(item[4:].strip(":")) / 1000); continue
                        name, _, h = item.partition(":")
                        res["steps"].append(await tap(name.strip(), int(h) if h else req.get("hold", 120)))
                        await asyncio.sleep(req.get("gap", 250) / 1000)
                elif op in ("down", "up"):
                    b = ALIAS.get(req["button"].upper(), req["button"].upper())
                    ctl.button(b, "press" if op == "down" else "release"); res = {"button": b, op: True}
                elif op == "stick":
                    ctl.stick(req["stick"], point=(float(req["x"]), float(req["y"])))
                    await asyncio.sleep(req.get("hold", 200) / 1000)
                    ctl.stick(req["stick"], point=(0.0, 0.0)); res = {"ok": True}
                elif op == "shot":
                    res = shot(req["path"])
                elif op == "status":
                    res = {"stats": STATS, "ready": sess.is_ready, "frames": rx.nframes, "last_frame_age_ms": round((time.time() - rx.last_t) * 1000)}
                elif op == "quit":
                    res = {"bye": True}; w.write((json.dumps(res) + "\n").encode()); await w.drain()
                    dev.disconnect(); os._exit(0)
                else:
                    res = {"error": f"bad op {op}"}
        except Exception as e:
            res = {"error": repr(e)}
        w.write((json.dumps(res) + "\n").encode()); await w.drain(); w.close()

    if os.path.exists(SOCK): os.unlink(SOCK)
    srv = await asyncio.start_unix_server(handle, SOCK)
    while not sess.is_stopped: await asyncio.sleep(0.5)
    log.error("session stopped: %s", sess.error); os.unlink(SOCK); os._exit(5)

asyncio.run(main())
