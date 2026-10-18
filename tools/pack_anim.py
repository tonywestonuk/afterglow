#!/usr/bin/env python3
"""Pack rendered stereo frames into afterglow's boot animation format (see system/init/bootanim.c).

Input: Blender's stereo output, frame_NNNN_L.png / frame_NNNN_R.png, each eye rendered with its
camera centred on that eye's lens. The header records each lens centre, which the player rotates
that eye's image around to cancel the headset's display roll. Both eyes are cropped to one
rectangle covering everything visible in any frame, then stored as zstd frames: keyframes at the
start and at the loop point, bytewise differences from the previous frame everywhere else.

Usage: pack_anim.py <frames dir> <output .anim> [--fps 36] [--loop-start 109]
Needs: numpy, pillow, zstandard
"""
import argparse
import glob
import os
import struct

import numpy as np
import zstandard
from PIL import Image

# Lens centres on the Quest 1's 2880x1600 screen as the viewer sees it (left eye first): where Meta's
# splash logo appears. The bootloader draws the splash upright into the framebuffer at (851, 734) and
# (2027, 734), and the panels show the framebuffer rotated 180° (the player compensates).
LENS_CENTRES = ((852, 865), (2028, 865))


def load(path, threshold):
    rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8).copy()
    rgb[rgb.max(axis=2) <= threshold] = 0   # near-black render noise compresses badly and isn't seen
    return rgb


def main():
    p = argparse.ArgumentParser()
    p.add_argument("frames_dir")
    p.add_argument("output")
    p.add_argument("--fps", type=int, default=36)
    p.add_argument("--loop-start", type=int, default=109, help="1-based frame the loop returns to")
    p.add_argument("--threshold", type=int, default=2, help="pixels at or below this are black")
    p.add_argument("--level", type=int, default=19, help="zstd level")
    args = p.parse_args()

    left = sorted(glob.glob(os.path.join(args.frames_dir, "frame_*_L.png")))
    right = sorted(glob.glob(os.path.join(args.frames_dir, "frame_*_R.png")))
    assert left and len(left) == len(right), "need matching _L and _R frames"
    frames = len(left)
    size = Image.open(left[0]).size[0]

    # Crop: union of every non-black pixel in every frame of both eyes, padded, even-sized
    lo, hi = np.array([size, size]), np.array([0, 0])
    for path in left + right:
        ys, xs = np.nonzero(load(path, args.threshold).max(axis=2))
        if len(xs):
            lo = np.minimum(lo, [xs.min(), ys.min()])
            hi = np.maximum(hi, [xs.max() + 1, ys.max() + 1])
    lo = np.maximum(lo - 4, 0)
    hi = np.minimum(hi + 4, size)
    hi += (hi - lo) % 2
    x0, y0, x1, y1 = int(lo[0]), int(lo[1]), int(hi[0]), int(hi[1])
    width, height = x1 - x0, y1 - y0
    print(f"{frames} frames per eye, rendered {size}x{size}, cropped to {width}x{height} at ({x0},{y0})")

    cctx = zstandard.ZstdCompressor(level=args.level)
    keyframes = {0, args.loop_start - 1}
    blobs, index = [], []
    prev = [None, None]
    for f in range(frames):
        for eye, paths in enumerate((left, right)):
            cur = load(paths[f], args.threshold)[y0:y1, x0:x1]
            key = f in keyframes
            raw = cur if key else cur - prev[eye]   # uint8 arithmetic wraps mod 256
            blobs.append(cctx.compress(np.ascontiguousarray(raw).tobytes()))
            index.append(1 if key else 0)
            prev[eye] = cur

    eye_x = [cx - size // 2 + x0 for cx, _ in LENS_CENTRES]
    eye_y = [cy - size // 2 + y0 for _, cy in LENS_CENTRES]
    centre_x = [cx for cx, _ in LENS_CENTRES]
    centre_y = [cy for _, cy in LENS_CENTRES]
    header = struct.pack("<8s5I8i", b"AGLANIM1", args.fps, frames, args.loop_start - 1, width, height,
                         *eye_x, *eye_y, *centre_x, *centre_y)
    offset = len(header) + len(blobs) * 12
    table = b""
    for blob, flags in zip(blobs, index):
        table += struct.pack("<3I", offset, len(blob), flags)
        offset += len(blob)
    with open(args.output, "wb") as out:
        out.write(header + table + b"".join(blobs))
    total = os.path.getsize(args.output)
    print(f"eyes at {list(zip(eye_x, eye_y))}; {total / 1e6:.1f} MB "
          f"({total / len(blobs) / 1e3:.0f} KB per eye-frame, raw {width * height * 3 / 1e3:.0f} KB)")


if __name__ == "__main__":
    main()
