#!/usr/bin/env python3
"""Display roll calibration: frames of level-reference lines tilted by a sweep of angles.

Each angle is shown for a few seconds with its value in large figures; whoever wears the headset
reports which value makes the lines look level (with both eyes, and each eye alone). Angles are
counterclockwise as the viewer sees them, so the level-looking value is the correction to apply.
Writes frame_NNNN_L.png / _R.png like Blender's stereo output, for tools/pack_anim.py.

Usage: make_calibration_frames.py <out dir> [--from -14] [--to 14] [--step 2] [--seconds 3] [--fps 1]
Needs: pillow
"""
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "brand", "fonts", "Montserrat-Light-merged.ttf")
SIZE = 900        # same as the boot animation's per-eye render, centred on the lens
SS = 3            # supersampling for smooth lines


def frame(angle, prefix=""):
    """Level lines and the label, all turned together: nothing in view stays level as a false cue."""
    big = SIZE * SS
    img = Image.new("RGB", (big, big), "black")
    d = ImageDraw.Draw(img)
    c = big / 2
    for k, (half, width, colour) in {0: (380, 6, "white"), -1: (300, 3, "#FFB070"), 1: (300, 3, "#FFB070"),
                                     -2: (220, 3, "#B85CFF"), 2: (220, 3, "#B85CFF")}.items():
        y = c + k * 70 * SS
        d.line([(c - half * SS, y), (c + half * SS, y)], fill=colour, width=width * SS)
    label = prefix + (f"{angle:+d}°" if angle else "0°")
    d.text((c, c - 230 * SS), label, fill="white", font=ImageFont.truetype(FONT, 110 * SS), anchor="mm")
    # PIL turns positive angles counterclockwise, as the viewer sees the image
    img = img.rotate(angle, resample=Image.BICUBIC, center=(c, c))
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--from", dest="start", type=int, default=-14)
    p.add_argument("--to", dest="stop", type=int, default=14)
    p.add_argument("--step", type=int, default=2)
    p.add_argument("--seconds", type=int, default=3)
    p.add_argument("--fps", type=int, default=1)
    p.add_argument("--per-eye", action="store_true",
                   help="sweep the left eye alone (right eye black), then the right eye alone")
    args = p.parse_args()
    os.makedirs(args.out, exist_ok=True)
    black = Image.new("RGB", (SIZE, SIZE), "black")
    sweeps = ("L", "R") if args.per_eye else ("",)
    n = 1
    for eye in sweeps:
        for angle in range(args.start, args.stop + 1, args.step):
            img = frame(angle, prefix=f"{eye} " if eye else "")
            for _ in range(args.seconds * args.fps):
                for side in "LR":
                    shown = img if eye in ("", side) else black
                    shown.save(os.path.join(args.out, f"frame_{n:04d}_{side}.png"))
                n += 1
    print(f"wrote {n - 1} frames per eye ({'per eye, ' if args.per_eye else ''}"
          f"{args.start}..{args.stop} step {args.step}, {args.seconds} s each)")


if __name__ == "__main__":
    main()
