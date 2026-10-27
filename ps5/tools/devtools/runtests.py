#!/usr/bin/env python3
"""Ship of Harkinian for the PlayStation 5 - scripted tests against a dev build (DEVTOOLS.md).

    runtests.py [--url URL] [--out DIR] [TEST...]     default: every test in tests/

A test is YAML (a list of steps) or Python (def run(t): ...). Each test gets a fresh
connection; screenshots and a results.json go to --out (default
/workspace/devtools/results/<time>). Exit status 1 when any test fails.

YAML steps (one key each):
    - require_state: [title, file_select, play]      # the game must be in one of these
    - wait: {state: play, loaded: true, timeout: 60}  # the server's wait command
    - wait_any_state: {states: [title, file_select], timeout: 180}
    - load: {slot: 0, max_seconds: 30}
    - save: {slot: 0}
    - warp: {entrance: LOST_WOODS_BRIDGE_EAST_EXIT, age: child, time: noon, max_seconds: 10}
    - press: {buttons: [A], frames: 2}
    - stick: {x: 0, y: 80, frames: 40}
    - input: {steps: [...]}
    - sleep: 2                      # seconds of game time (the server's clock)
    - frames: 20                    # game frames
    - perf_reset: {}
    - assert_perf: {max_ms: 20, over_20ms: 0, min_fps: 55}
    - assert_state: {scene: 0x5b, loaded: true, "pos[1]": {gt: -500}}
    - assert_moved: {min: 50}       # Link moved at least this far since the last mark
    - mark: {}                      # remember Link's position for assert_moved
    - clear_text: {max_presses: 40} # press A until the stick moves Link (cutscenes, text)
    - cvar: {name: gFoo, value: 1}
    - console: "reload"
    - screenshot: bridge-east       # <out>/<test>-bridge-east.png
    - log_tail: 20                  # copy the game log's end into the results

SPDX-License-Identifier: MIT
"""
import argparse, glob, importlib.util, json, math, os, sys, time, traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yaml  # python3-yaml
from sohdev import Client, DEFAULT_URL, DevError

HERE = os.path.dirname(os.path.abspath(__file__))


class Failed(AssertionError):
    pass


class T:
    """What a test step (or a Python test) works with."""

    def __init__(self, client, name, out):
        self.c, self.name, self.out = client, name, out
        self.notes, self.shots, self.measures = [], [], {}
        self.mark_pos = None

    def note(self, msg):
        print("    " + msg)
        self.notes.append(msg)

    def check(self, cond, msg):
        if not cond:
            raise Failed(msg)
        self.note("ok: " + msg)

    def screenshot(self, label, max_width=1280):
        path = os.path.join(self.out, "%s-%s.png" % (self.name, label))
        r = self.c.screenshot(path, max_width=max_width)
        self.shots.append(path)
        self.note("screenshot %s (%dx%d)" % (path, r["width"], r["height"]))
        return path

    def measure(self, key, value):
        self.measures[key] = value
        text = "%s = %s" % (key, value)
        self.note(text if len(text) < 400 else text[:400] + " ...")


def compare(actual, spec, what):
    if isinstance(spec, dict):
        for op, v in spec.items():
            ok = {"gt": actual > v, "ge": actual >= v, "lt": actual < v, "le": actual <= v,
                  "eq": actual == v, "ne": actual != v}[op]
            if not ok:
                raise Failed("%s = %r, wanted %s %r" % (what, actual, op, v))
    elif isinstance(spec, list):
        if actual not in spec:
            raise Failed("%s = %r, wanted one of %r" % (what, actual, spec))
    elif actual != spec:
        raise Failed("%s = %r, wanted %r" % (what, actual, spec))


def lookup(obj, path):
    for part in path.replace("]", "").replace("[", ".").split("."):
        obj = obj[int(part)] if isinstance(obj, list) else obj[part]
    return obj


def step(t, kind, arg):
    c = t.c
    arg = {} if arg is None else arg
    if kind == "require_state":
        s = c.snapshot()["state"]
        t.check(s in arg, "state %s is one of %s" % (s, arg))
    elif kind == "wait":
        r = c.wait(**arg)
        t.note("waited %.2f s (%d frames)" % (r["elapsed"], r["frames"]))
    elif kind == "wait_any_state":
        start = time.time()
        s = c.wait_until(lambda s: s["state"] in arg["states"], timeout=arg.get("timeout", 60))
        t.measure("seconds_to_" + s["state"], round(time.time() - start, 2))
    elif kind == "load":
        r = c.load(arg.get("slot", 0), timeout=arg.get("timeout", 60))
        t.measure("load_seconds", round(r["load_seconds"], 2))
        if "max_seconds" in arg:
            t.check(r["load_seconds"] <= arg["max_seconds"], "loaded in %.2f s <= %s s" % (r["load_seconds"], arg["max_seconds"]))
    elif kind == "save":
        r = c.save(arg.get("slot"))
        t.note("saved slot %d (scene 0x%x)" % (r["slot"], r["scene"]))
    elif kind == "warp":
        a = dict(arg)
        limit = a.pop("max_seconds", None)
        r = c.warp(**a)
        t.measure("warp_%s_seconds" % r["entrance_name"], round(r["load_seconds"], 2))
        t.note("in %s (scene 0x%x), Link at %s" % (r["scene_name"], r["scene"], r["snapshot"]["pos"]))
        if limit is not None:
            t.check(r["load_seconds"] <= limit, "scene loaded in %.2f s <= %s s" % (r["load_seconds"], limit))
    elif kind == "press":
        c.press(arg["buttons"], arg.get("frames", 2), arg.get("release", 2))
    elif kind == "stick":
        c.stick(arg.get("x", 0), arg.get("y", 0), arg["frames"], arg.get("buttons", ()))
    elif kind == "input":
        c.input(arg["steps"])
        c.wait(input_idle=True, timeout=arg.get("timeout", 120))
    elif kind == "sleep":
        c.sleep(float(arg))
    elif kind == "frames":
        c.frames(int(arg))
    elif kind == "perf_reset":
        c.perf_reset()
    elif kind == "assert_perf":
        p = c.perf()
        t.measure("perf", {k: p[k] for k in ("seconds", "frames", "fps", "ms_avg", "ms_max", "over_20ms", "over_33ms", "rings")})
        if "max_ms" in arg:
            t.check(p["ms_max"] <= arg["max_ms"], "longest frame %.2f ms <= %s ms" % (p["ms_max"], arg["max_ms"]))
        if "over_20ms" in arg:
            t.check(p["over_20ms"] <= arg["over_20ms"], "%d frames over 20 ms <= %s" % (p["over_20ms"], arg["over_20ms"]))
        if "min_fps" in arg:
            t.check(p["fps"] >= arg["min_fps"], "%.1f fps >= %s" % (p["fps"], arg["min_fps"]))
    elif kind == "assert_state":
        s = c.snapshot()
        for k, v in arg.items():
            compare(lookup(s, k), v, k)
            t.note("ok: %s = %r" % (k, lookup(s, k)))
    elif kind == "clear_text":
        for i in range(arg.get("max_presses", 40)):
            p0 = c.snapshot()["pos"]
            c.stick(0, 60, 6)
            c.wait(input_idle=True, timeout=10)
            if math.dist(c.snapshot()["pos"], p0) > 15:
                t.note("Link moves (after %d A presses)" % i)
                break
            c.press(["A"], 2, 8)
            c.wait(input_idle=True, timeout=10)
        else:
            raise Failed("Link still does not move after %d A presses" % arg.get("max_presses", 40))
    elif kind == "mark":
        t.mark_pos = c.snapshot()["pos"]
    elif kind == "assert_moved":
        p = c.snapshot()["pos"]
        d = math.dist(p, t.mark_pos)
        t.check(d >= arg.get("min", 1), "Link moved %.1f units (>= %s)" % (d, arg.get("min", 1)))
    elif kind == "cvar":
        c.cvar(**arg)
    elif kind == "console":
        r = c.console(arg)
        t.note("console %r -> %s" % (arg, r["result"]))
    elif kind == "screenshot":
        t.screenshot(str(arg))
    elif kind == "log_tail":
        t.measure("log_tail", c.log(int(arg)))
    else:
        raise ValueError("unknown step %r" % kind)


def run_test(path, url, out):
    name = os.path.splitext(os.path.basename(path))[0]
    print("== %s" % name)
    result = {"test": name, "file": path, "ok": False}
    start = time.time()
    t = None
    try:
        with Client(url) as c:
            t = T(c, name, out)
            if path.endswith(".py"):
                spec = importlib.util.spec_from_file_location("test_" + name, path)
                mod = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(mod)
                mod.run(t)
            else:
                doc = yaml.safe_load(open(path))
                for s in doc["steps"]:
                    (kind, arg), = s.items() if isinstance(s, dict) else ((s, None),)
                    step(t, kind, arg)
        result["ok"] = True
    except (Failed, DevError, TimeoutError, ConnectionError, OSError, KeyError, ValueError) as e:
        result["error"] = "%s: %s" % (type(e).__name__, e)
        if not isinstance(e, Failed):
            result["traceback"] = traceback.format_exc()
        print("    FAIL: " + result["error"])
        if t is not None:
            try:  # what the screen showed when it failed
                t.screenshot("failure")
            except Exception:
                pass
    result["seconds"] = round(time.time() - start, 1)
    if t is not None:
        result.update(notes=t.notes, screenshots=t.shots, measures=t.measures)
    print("   %s (%.1f s)" % ("PASS" if result["ok"] else "FAIL", result["seconds"]))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--out", default=os.path.join("/workspace/devtools/results", time.strftime("%Y%m%d-%H%M%S")))
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    tests = a.tests or sorted(glob.glob(os.path.join(HERE, "tests", "*.yaml")) + glob.glob(os.path.join(HERE, "tests", "*.py")))
    os.makedirs(a.out, exist_ok=True)
    results = [run_test(p, a.url, a.out) for p in tests]
    json.dump(results, open(os.path.join(a.out, "results.json"), "w"), indent=2)
    failed = [r["test"] for r in results if not r["ok"]]
    print("%d passed, %d failed%s; results in %s" % (len(results) - len(failed), len(failed),
                                                    " (%s)" % ", ".join(failed) if failed else "", a.out))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
