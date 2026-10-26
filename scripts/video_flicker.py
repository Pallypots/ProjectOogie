"""Project Oogie: find flicker in a screen recording.

For every frame: mean brightness and the mean absolute difference to its
neighbours (on a 160x90 copy). A "flash" is a frame that differs from both
neighbours much more than they differ from each other (a frame that comes
and goes). Prints a per-second summary and saves, for the first flash of each
second, the frame before, the flash and the frame after side by side.

Usage: py -3 scripts/video_flicker.py <video> <outdir> [--start S] [--end S]
"""
import sys, os, cv2, numpy as np

def main():
    video, outdir = sys.argv[1], sys.argv[2]
    start = float(sys.argv[sys.argv.index('--start') + 1]) if '--start' in sys.argv else 0
    end = float(sys.argv[sys.argv.index('--end') + 1]) if '--end' in sys.argv else 1e9
    os.makedirs(outdir, exist_ok=True)
    cap = cv2.VideoCapture(video)
    fps = cap.get(cv2.CAP_PROP_FPS) or 60
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"{n} frames at {fps:.2f} fps ({n / fps:.0f} s)")
    i = 0
    ring = []          # (index, small, medium)
    flashes, seen_sec, means = [], set(), []
    while True:
        ok, fr = cap.read()
        if not ok:
            break
        t = i / fps
        if t < start:
            i += 1
            continue
        if t > end:
            break
        s = cv2.resize(fr, (160, 90), interpolation=cv2.INTER_AREA).astype(np.int16)
        m = cv2.resize(fr, (480, 270), interpolation=cv2.INTER_AREA)
        means.append((i, float(s.mean())))
        ring.append((i, s, m))
        if len(ring) > 3:
            ring.pop(0)
        if len(ring) == 3:
            (i0, s0, m0), (i1, s1, m1), (i2, s2, m2) = ring
            a = np.abs(s1 - s0).mean(); b = np.abs(s1 - s2).mean(); c = np.abs(s2 - s0).mean()
            if a > 12 and b > 12 and c < 0.4 * min(a, b):
                flashes.append((i1, a, b, c, float(s1.mean()), float(s0.mean())))
                sec = int(i1 / fps)
                if sec not in seen_sec and len(seen_sec) < 60:
                    seen_sec.add(sec)
                    cv2.imwrite(os.path.join(outdir, f"t{sec // 60:02d}m{sec % 60:02d}s_f{i1}.png"), np.hstack([m0, m1, m2]))
        i += 1
    print(f"{len(flashes)} flash frames")
    per_sec = {}
    for f in flashes:
        per_sec.setdefault(int(f[0] / fps), []).append(f)
    for sec in sorted(per_sec):
        fl = per_sec[sec]
        print(f"  {sec // 60:02d}:{sec % 60:02d}  {len(fl)} flashes  (flash mean {fl[0][4]:.1f}, before {fl[0][5]:.1f})")
    with open(os.path.join(outdir, 'means.csv'), 'w') as fh:
        for k, m in means:
            fh.write(f"{k},{m:.2f}\n")

if __name__ == '__main__':
    main()
