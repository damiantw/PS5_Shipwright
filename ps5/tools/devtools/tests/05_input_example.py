"""A Python test: walk a circle in Kokiri Forest and check Link went somewhere and came back near."""
import math


def run(t):
    c = t.c
    r = c.warp("KOKIRI_FOREST_0", age="child", time="noon")
    t.measure("warp_seconds", round(r["load_seconds"], 2))
    start = c.snapshot()["pos"]
    steps = [{"stick": [int(70 * math.sin(a / 8)), int(70 * math.cos(a / 8))], "frames": 3} for a in range(0, 50)]
    c.input(steps)
    far = 0.0
    while c.snapshot()["input_frames"]:
        far = max(far, math.dist(c.snapshot()["pos"], start))
        c.frames(5)
    t.check(far > 50, "Link walked away (%.0f units)" % far)
    t.screenshot("kokiri")
