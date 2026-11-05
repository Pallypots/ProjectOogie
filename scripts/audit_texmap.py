"""Audit the PS2 HD texture map (assets\\texpacks\\ps2_hd.map) for wrong pairings.

KNOWN_ISSUES #98: Shadow Oogie's two 512x512 textures are black in colour (his look is in
their alpha), and build_texmap.py's matcher, which leans on brightness, paired them with the
title logo and a loading picture. This compares every map row's Xbox texture with its pack
picture on colour and on alpha and lists the rows that look wrong.

Needs the build_texmap pipeline cache (build_out\\research\\textures\\pipeline: rows.csv,
xbox_textures.json) and the HD pack folder.
Usage: py -3 scripts\\audit_texmap.py --hd <pack textures dir> [--sheet out.png] [--csv out.csv]
"""
import os, sys, csv, json, argparse
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build_texmap as bt  # noqa: E402

PIPE = os.path.join(ROOT, "build_out", "research", "textures", "pipeline")


def small(img, w, h):
    a = np.asarray(img.resize((w, h), Image.BOX), np.float32) / 255.0
    return a


def corr(x, y):
    x = x.ravel(); y = y.ravel()
    sx, sy = x.std(), y.std()
    if sx < 1e-3 or sy < 1e-3:
        return None
    return float(((x - x.mean()) * (y - y.mean())).mean() / (sx * sy))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hd", required=True)
    ap.add_argument("--sheet", default=os.path.join(PIPE, "audit_sheet.png"))
    ap.add_argument("--csv", default=os.path.join(PIPE, "audit.csv"))
    a = ap.parse_args()
    locs = {d["uid"]: d for d in json.load(open(os.path.join(PIPE, "xbox_textures.json")))}
    rows = [r for r in csv.DictReader(open(os.path.join(PIPE, "rows.csv"))) if r["pack"] == "hd"]
    out = []
    for r in rows:
        loc = locs.get(r["xbox"])
        p = os.path.join(a.hd, r["file"])
        if not loc or not os.path.exists(p):
            continue
        x = bt.load_xbox_img(loc)
        xi = Image.fromarray(x, "RGBA")
        pi = Image.open(p).convert("RGBA")
        w, h = 32, max(4, int(32 * x.shape[0] / x.shape[1]))
        xs, ps = small(xi, w, h), small(pi, w, h)
        if "alpha2" in r["flags"]:
            ps[..., 3] = np.minimum(1.0, ps[..., 3] * 2.0)
        lum = np.array([0.299, 0.587, 0.114], np.float32)
        xl, pl = xs[..., :3] @ lum, ps[..., :3] @ lum
        xa, pa = xs[..., 3], ps[..., 3]
        lc, ac = corr(xl, pl), corr(xa, pa)
        cdiff = float(np.abs(xs[..., :3].mean((0, 1)) - ps[..., :3].mean((0, 1))).mean())
        adiff = float(np.abs(xa - pa).mean())
        why = []
        if xl.std() < 0.03:
            why.append("flat-colour")
        if lc is not None and lc < 0.5:
            why.append("lum %.2f" % lc)
        if ac is not None and ac < 0.5:
            why.append("alpha %.2f" % ac)
        if adiff > 0.25:
            why.append("alphadiff %.2f" % adiff)
        if cdiff > 0.25:
            why.append("colourdiff %.2f" % cdiff)
        out.append(dict(key=r["key"], file=r["file"], flags=r["flags"], entry=loc["arc"] + ":" + loc["entry"],
                        lc=lc, ac=ac, cdiff=cdiff, adiff=adiff, why=" ".join(why), xi=xi, pi=pi))
    bad = [o for o in out if o["why"]]
    with open(a.csv, "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["key", "file", "flags", "entry", "lum_corr", "alpha_corr", "colour_diff", "alpha_diff", "why"])
        for o in out:
            wr.writerow([o["key"], o["file"], o["flags"], o["entry"], o["lc"], o["ac"], "%.3f" % o["cdiff"], "%.3f" % o["adiff"], o["why"]])
    print("%d rows checked, %d flagged" % (len(out), len(bad)))
    # contact sheet: Xbox (colour, alpha) | pack (colour, alpha), 96 px each
    t = 96; cols = 2
    sh = Image.new("RGB", (cols * (4 * t + 8), ((len(bad) + cols - 1) // cols) * (t + 14) + 2), (50, 0, 50))
    d = ImageDraw.Draw(sh)
    for i, o in enumerate(bad):
        x0 = (i % cols) * (4 * t + 8); y0 = (i // cols) * (t + 14)
        for k, im in enumerate((o["xi"], o["pi"])):
            im = im.resize((t, t))
            arr = np.asarray(im)
            sh.paste(Image.fromarray(np.ascontiguousarray(arr[..., :3])), (x0 + k * 2 * t, y0 + 14))
            sh.paste(Image.fromarray(np.ascontiguousarray(arr[..., 3])).convert("RGB"), (x0 + k * 2 * t + t, y0 + 14))
        d.text((x0 + 2, y0), "%d %s %s" % (i, o["key"][-8:], o["why"][:40]), fill=(255, 255, 0))
    sh.save(a.sheet)
    for i, o in enumerate(bad):
        print(i, o["key"], o["file"][:16], o["entry"], o["why"])


if __name__ == "__main__":
    main()
