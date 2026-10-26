"""Which menu screen is showing? Compares a frame to reference pictures.

References live in logs/ref/<state>.png (captured with scripts/live_flips.ps1
from a known screen). Prints "<state> <distance>" for the closest one, or
"unknown <distance>" when nothing is close. Distance: mean absolute
difference of 80x60 grey thumbnails (0-255).
Usage: python scripts/screen_state.py <frame.png> [--save <state>]
  --save copies the frame to logs/ref/<state>.png.
"""
import os
import shutil
import sys

import cv2
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "logs", "ref")


def thumb(path):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    return cv2.resize(img, (80, 60), interpolation=cv2.INTER_AREA).astype(np.float32)


def main():
    frame = sys.argv[1]
    if "--save" in sys.argv:
        os.makedirs(REF, exist_ok=True)
        shutil.copy(frame, os.path.join(REF, sys.argv[sys.argv.index("--save") + 1] + ".png"))
        return
    t = thumb(frame)
    best, bd = "unknown", 1e9
    for f in os.listdir(REF):
        if f.endswith(".png"):
            d = float(np.abs(thumb(os.path.join(REF, f)) - t).mean())
            if d < bd:
                best, bd = f[:-4], d
    print(f"{best if bd < 6 else 'unknown'} {bd:.1f}")


if __name__ == "__main__":
    main()
