#!/usr/bin/env python3
"""PlayStation button prompts as a small O2R archive (ps5_prompts.o2r).

Replaces OoT's N64 button glyphs (dialogue font, pause-menu, ocarina, empty C slots,
file-select controls line) with Kenney's CC0 PlayStation prompts (ps5/art/prompts,
pinned from tanuki-billie/kenney-input-prompts 49f37cc). No game data: only each
texture's name and size are needed, and they are listed below (the same in every
supported ROM). Approach after mshivam019/oot64-ps5 tools/make-playstation-prompts.py
(GPL-3.0). Layout = this port's PS5 defaults: Cross=A, Circle=B, L1=L, R2=R, L2=Z,
D-pad=C buttons (the right stick is the camera), Options=Start. The file-select
line's words use Pillow's built-in font (Pillow 10.1 or later).

    ps5/tools/make-prompts.py OUT.o2r [--preview sheet.png]
"""
import argparse, struct, sys, zipfile
from pathlib import Path
from PIL import Image, ImageDraw

ART = Path(__file__).resolve().parent.parent / "art" / "prompts"
I4, IA8 = 5, 8
# texture: (format, width, height, prompt)
TEXTURES = {
    "nes_font_static/gMsgChar9FButtonATex": (I4, 16, 16, "button_cross"),
    "nes_font_static/gMsgCharA0ButtonBTex": (I4, 16, 16, "button_circle"),
    "nes_font_static/gMsgCharA1ButtonCTex": (I4, 16, 16, "dpad"),
    "nes_font_static/gMsgCharA2ButtonLTex": (I4, 16, 16, "trigger_l1"),
    "nes_font_static/gMsgCharA3ButtonRTex": (I4, 16, 16, "trigger_r2"),
    "nes_font_static/gMsgCharA4ButtonZTex": (I4, 16, 16, "trigger_l2"),
    "nes_font_static/gMsgCharA5ButtonCUpTex": (I4, 16, 16, "dpad_up"),
    "nes_font_static/gMsgCharA6ButtonCDownTex": (I4, 16, 16, "dpad_down"),
    "nes_font_static/gMsgCharA7ButtonCLeftTex": (I4, 16, 16, "dpad_left"),
    "nes_font_static/gMsgCharA8ButtonCRightTex": (I4, 16, 16, "dpad_right"),
    "nes_font_static/gMsgCharAAControlStickTex": (I4, 16, 16, "stick_l"),
    "icon_item_static/gABtnSymbolTex": (IA8, 24, 16, "button_cross"),
    "icon_item_static/gBBtnSymbolTex": (IA8, 24, 16, "button_circle"),
    "icon_item_static/gCBtnSymbolsTex": (IA8, 48, 16, ["dpad_left", "dpad_down", "dpad_right"]),
    # the pause screen's page arrows: the left one is Z (decomp calls it "L")
    "icon_item_static/gLButtonTex": (IA8, 24, 32, ("arrow_left", "trigger_l2")),
    "icon_item_static/gRButtonTex": (IA8, 24, 32, ("arrow_right", "trigger_r2")),
    "parameter_static/gOcarinaBtnIconATex": (IA8, 16, 16, "button_cross"),
    "parameter_static/gOcarinaBtnIconCUpTex": (IA8, 16, 16, "dpad_up"),
    "parameter_static/gOcarinaBtnIconCDownTex": (IA8, 16, 16, "dpad_down"),
    "parameter_static/gOcarinaBtnIconCLeftTex": (IA8, 16, 16, "dpad_left"),
    "parameter_static/gOcarinaBtnIconCRightTex": (IA8, 16, 16, "dpad_right"),
    "parameter_static/gEmptyCLeftArrowTex": (IA8, 32, 32, "dpad_left"),
    "parameter_static/gEmptyCDownArrowTex": (IA8, 32, 32, "dpad_down"),
    "parameter_static/gEmptyCRightArrowTex": (IA8, 32, 32, "dpad_right"),
    # file select's "A-Decide / B-Cancel" line
    "title_static/gFileSelControlsENGTex": (IA8, 144, 16, ("line", [("button_cross", "Decide"),
                                                                   ("button_circle", "Cancel")])),
}
BITS = {1: 32, 2: 16, 3: 4, 4: 8, 5: 4, 6: 8, 7: 4, 8: 8, 9: 16}
SCALE = 8


def icon(name, size, fill=0.9):
    image = Image.open(ART / f"playstation_{name}.png").convert("RGBA")
    image = image.crop(image.getbbox())
    image.thumbnail((max(1, int(size[0] * fill)), max(1, int(size[1] * fill))), Image.Resampling.LANCZOS)
    out = Image.new("RGBA", size)
    out.alpha_composite(image, ((size[0] - image.width) // 2, (size[1] - image.height) // 2))
    return out


def arrow(side, prompt, size):
    # a grey arrow plate (like the N64 one) with the PlayStation trigger on it
    w, h = size
    out = Image.new("RGBA", size)
    tip = w * 0.28
    pts = [(tip, 0), (w, 0), (w, h), (tip, h), (0, h / 2)] if side == "arrow_left" else \
          [(0, 0), (w - tip, 0), (w, h / 2), (w - tip, h), (0, h)]
    ImageDraw.Draw(out).polygon(pts, fill=(150, 150, 150, 230))
    glyph = icon(prompt, (int(w * 0.62), int(h * 0.62)), 1.0)
    x = int(tip * 0.8) if side == "arrow_left" else int((w - tip) - glyph.width) + int(tip * 0.2)
    out.alpha_composite(glyph, (max(0, x), (h - glyph.height) // 2))
    return out


def line(entries, size):
    # glyph + word pairs, centred; white, the game tints it like the original line
    from PIL import ImageFont
    w, h = size
    font = ImageFont.load_default(size=int(h * 0.8))
    draw = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    gap, pad = int(h * 0.25), int(h * 0.9)
    widths = [h + gap + int(draw.textlength(word, font=font)) for _, word in entries]
    x = (w - (sum(widths) + pad * (len(entries) - 1))) // 2
    out = Image.new("RGBA", size)
    for (glyph, word), width in zip(entries, widths):
        out.alpha_composite(icon(glyph, (h, h), 0.95), (x, 0))
        ImageDraw.Draw(out).text((x + h + gap, h // 2), word, font=font, fill="white", anchor="lm")
        x += width + pad
    return out


def render(spec, size):
    if isinstance(spec, list):
        out = Image.new("RGBA", size)
        cell = (size[0] // len(spec), size[1])
        for i, name in enumerate(spec):
            out.alpha_composite(icon(name, cell), (i * cell[0], 0))
        return out
    if isinstance(spec, tuple) and spec[0] == "line":
        return line(spec[1], size)
    if isinstance(spec, tuple):
        return arrow(spec[0], spec[1], size)
    return icon(spec, size)


def resource(image, kind, w, h):
    header = bytearray(64)
    struct.pack_into("<BBHIIQ", header, 0, 0, 1, 0, 0x4F544558, 1, 0)  # little-endian, Texture v1
    data = image.tobytes()
    # replacement pixels are RGBA32; HByteScale scales bytes, not texels
    return bytes(header) + struct.pack("<IIIIffI", kind, image.width, image.height, 1,
                                       image.width / w * 32 / BITS[kind], image.height / h, len(data)) + data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", type=Path)
    ap.add_argument("--preview", type=Path)
    args = ap.parse_args()
    previews = []
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as out:
        for name, (kind, w, h, spec) in TEXTURES.items():
            image = render(spec, (w * SCALE, h * SCALE))
            info = zipfile.ZipInfo("alt/textures/" + name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            out.writestr(info, resource(image, kind, w, h))
            previews.append((name.rsplit("/", 1)[-1], image))
        info = zipfile.ZipInfo("ps5-prompts/LICENSE.txt", (1980, 1, 1, 0, 0, 0))
        out.writestr(info, (ART / "LICENSE.txt").read_bytes())
    if args.preview:
        sheet = Image.new("RGB", (900, ((len(previews) + 2) // 3) * 120), "#454545")
        for i, (name, image) in enumerate(previews):
            image = image.copy(); image.thumbnail((280, 88))
            x, y = i % 3 * 300, i // 3 * 120
            sheet.paste(image, (x, y), image)
            ImageDraw.Draw(sheet).text((x, y + 92), name, fill="white")
        sheet.save(args.preview)
    print(f"==> {args.output} ({args.output.stat().st_size} bytes, {len(previews)} textures)")


if __name__ == "__main__":
    sys.exit(main())
