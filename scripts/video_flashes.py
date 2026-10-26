"""Find single-frame glitches ("flashes") in a screen recording of the game.

For each frame: a 64x48 thumbnail. A frame is a flash when it differs from
both neighbours much more than they differ from each other (one frame that
does not belong). Saves each flash with its neighbours side by side, plus a
summary. KNOWN_ISSUES #46.
Usage: python scripts/video_flashes.py <video.mp4 | frame folder> <outdir> [--max 40] [--tag t]
"""
import os
import sys

import cv2
import numpy as np


def main():
    src, out = sys.argv[1], sys.argv[2]
    mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 40
    os.makedirs(out, exist_ok=True)
    thumbs, frames = [], []
    if os.path.isdir(src):
        # a folder of frames from scripts/live_flips.ps1 (<tag>_<n>.png)
        files = sorted((f for f in os.listdir(src) if f.endswith(".png") and "_" in f
                        and f.rsplit("_", 1)[1][:-4].isdigit()),
                       key=lambda f: int(f.rsplit("_", 1)[1][:-4]))
        prefix = sys.argv[sys.argv.index("--tag") + 1] + "_" if "--tag" in sys.argv else ""
        files = [f for f in files if f.startswith(prefix)]
        fps = 60.0
        src_iter = (cv2.imread(os.path.join(src, f)) for f in files)
    else:
        cap = cv2.VideoCapture(src)
        fps = cap.get(cv2.CAP_PROP_FPS) or 60.0
        def read_all():
            while True:
                ok, f = cap.read()
                if not ok:
                    return
                yield f
        src_iter = read_all()
    for f in src_iter:
        if f is None:
            break
        thumbs.append(cv2.resize(f, (64, 48), interpolation=cv2.INTER_AREA).astype(np.float32))
        frames.append(cv2.resize(f, (480, 270), interpolation=cv2.INTER_AREA))
    n = len(thumbs)
    print(f"{n} frames at {fps:.1f} fps ({n / fps:.1f} s)")
    scores = []
    for i in range(1, n - 1):
        a = np.abs(thumbs[i] - thumbs[i - 1]).mean()
        b = np.abs(thumbs[i] - thumbs[i + 1]).mean()
        c = np.abs(thumbs[i - 1] - thumbs[i + 1]).mean()
        s = min(a, b) - c
        if s > 6 and min(a, b) > 2.5 * (c + 1):
            scores.append((s, i))
    scores.sort(reverse=True)
    print(f"{len(scores)} flash frames")
    stack = np.stack(thumbs)
    for s, i in scores[:mx]:
        # the most similar frame elsewhere in the run: is the flash an old
        # frame shown again?
        d = np.abs(stack - thumbs[i]).mean(axis=(1, 2, 3))
        d[max(0, i - 5):i + 6] = 1e9
        j = int(d.argmin())
        row = np.hstack([frames[i - 1], frames[i], frames[i + 1], frames[j]])
        name = os.path.join(out, f"flash_{i:06d}_{i / fps:07.2f}s.png")
        cv2.imwrite(name, row)
        print(f"  frame {i} t={i / fps:.2f}s score {s:.1f}; most like frame {j} "
              f"(t={j / fps:.2f}s, distance {d[j]:.1f} vs neighbours "
              f"{np.abs(thumbs[i] - thumbs[i - 1]).mean():.1f})")


if __name__ == "__main__":
    main()
