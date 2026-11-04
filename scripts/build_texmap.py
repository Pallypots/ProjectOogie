"""Build the texture-pack maps for Project Oogie (assets\\texpacks\\*.map).

Reproduces the whole offline pipeline:
  1. unpack   game\\stm_data\\*.lz with lz_unpack.py (the game's own LZ scheme)  -> <work>\\lz\\*.bin
  2. extract  every DDS (.pig) from the .stm archives and the unpacked .lz archives
  3. match    each PCSX2 pack file (PS2 HD pack, Modern Xbox Buttons pack) to the Xbox
              textures: coarse 32x32 pass over all same-aspect textures (top 10), then a
              fine pass at the Xbox texture's own size (long side capped at 256)
  4. key      the port's runtime texture key of each chosen Xbox texture:
              "<W>x<H>_<FNV-1a 64 of decoded BGRA level 0, then ^W, ^H>"
              (port/src/d3d11_backend.c tex_content_hash / tex_decode_fast / dxt_block_decode)
  5. write    assets\\texpacks\\ps2_hd.map, ps2_buttons.map, ps_glyphs.map

The maps hold keys, pack file names and archive locations only: no pixels. The pack images,
disc textures and caches stay in <work> (default build_out\\research\\textures\\pipeline,
git-ignored).

Usage (from C:\\ProjectOogie, Python 3.10+ with numpy and Pillow >= 9.2 for BC7):
  py -3 scripts\\build_texmap.py [--hd DIR] [--buttons DIR] [--work DIR] [--sheets]
  --sheets  also writes contact sheets of the 'check' rows to <work>\\sheets (for review)

Map formats: see write_maps() and the header comment of each .map file.
"""
import os, sys, csv, json, glob, struct, hashlib, argparse, collections, time
from concurrent.futures import ProcessPoolExecutor
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lz_unpack  # noqa: E402

# Pack folders: pass --hd / --buttons, or set OOGIE_HD_PACK / OOGIE_BUTTON_PACK.
DEF_HD = os.environ.get("OOGIE_HD_PACK", "")
DEF_BT = os.environ.get("OOGIE_BUTTON_PACK", "")
LANGS = ("_u", "_b", "_f", "_g", "_i", "_s")   # _u = US English (game language 1), _b = 2 ...
OK, CHECK = 0.85, 0.70
S = 32
M64 = (1 << 64) - 1
LZDIR = os.path.join(ROOT, "build_out", "research", "textures", "lz")

# Manual review (contact sheets, by eye). (pack file name, Xbox key) pairs that are wrong.
REJECT = {
    # detailed fire sprite onto a flat 16x16 orange swatch (0000.lz bg/pig/cirx27.pig), score 0.716
    ("f323f87efaa13fa0-1322ec8cb8103e1-00005993.dds", "16x16_540cae485a5127bb"),
    # yellow PS2 glows (10 different files) onto the white Xbox spot glow (02xx.lz bg/pig/spot.pig);
    # '*' = no pack file for this key
    ("*", "128x128_260b40b2e48888f5"),
    # flat blue PS2 swatches (4 files) onto the dark grey 0000.lz bg/pig/circle99.pig
    ("*", "32x32_243c52a42f7667bb"),
}
# Pack files that show PlayStation glyphs besides the button-pack names and GLYPH_ENTRIES matches
# (found by eye on the contact sheets).
STYLE_PS_EXTRA = {
}
# Verified by eye (contact sheet of the 14 neutral/_u pairs that differ): the neutral archives are
# mostly the Japanese edition (Japanese text, PS2-era wording such as OPTION and (c)2004, Japanese
# logo). Only these three neutral entries are the same English picture with PlayStation glyphs
# where the _u/_b copy has Xbox glyphs; only they go into ps_glyphs.map and give style=ps.
GLYPH_ENTRIES = {
    ("ckpt", "pig/ckpt00.pig"): "skull pieces sheet with the prompt 'SELECT button' (PS) / 'BACK button' (Xbox)",
    ("ckpt", "pig/ckpt08.pig"): "confirm button: PS cross / Xbox A, next to a white circle",
    ("sbuty", "common/pig/button00.pig"): "face buttons: PS circle, cross, triangle, square, START / Xbox B, A, Y, X, Start",
}

# ───────────────────────────── DDS (port-exact decode) ─────────────────────────────

# sbsolo.stm action_*/.../acXXXwNN.pig: help pages with a button legend at the right (the game
# draws the prompts from it; Hypothesis: by cell position). Xbox order X Y A B / L white R black,
# PS2 order square triangle circle cross / L1 L2 R1 R2 (= Xbox X Y B A), and the Modern Xbox
# Buttons pack keeps the PS2 order (X Y B A). Verified by eye on acrubw02, acrubw03, acsanw00.
# So either pack's picture would show B where the Xbox game means A and the other way round:
# these rows get slots=ps2 and stay 'check' until the engine swaps cells 3 and 4.
import re  # noqa: E402
# Pack files whose glyphs name a different button than the Xbox picture (by eye):
# acrubp9a.pig (sbsolo.stm action/rubber): the Xbox picture prompts the blue X, the Modern
# Xbox Buttons file draws an A there.
SLOT_FILES = {"ae226892fdb87448-785b8a63f35b1650-00006213.dds"}
SLOT_PAGE = re.compile(r"ac[a-z]{3}w[0-9][0-9][.]pig$")


def dds_header(b, o=0):
    if b[o:o + 4] != b"DDS " or len(b) < o + 128:
        return None
    h, w = struct.unpack_from("<II", b, o + 12)
    mips, = struct.unpack_from("<I", b, o + 28)
    pf_flags, = struct.unpack_from("<I", b, o + 80)
    fourcc = bytes(b[o + 84:o + 88])
    bits, rm, gm, bm, am = struct.unpack_from("<IIIII", b, o + 88)
    if pf_flags & 4:
        fmt = fourcc.decode("latin1")
    else:
        fmt = "RGB%d%s" % (bits, "A" if pf_flags & 1 else "")
    return dict(w=w, h=h, mips=max(mips, 1), fmt=fmt, masks=(rm, gm, bm, am), bits=bits)


def level0_size(hd):
    w, h, f = hd["w"], hd["h"], hd["fmt"]
    if f in ("DXT1", "DXT3", "DXT5"):
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if f == "DXT1" else 16)
    if f.startswith("RGB"):
        return w * h * (hd["bits"] // 8)
    return None


def _c565(c):
    # d3d8_expand_channel: v * 255 / (2^bits - 1), integer
    r = ((c >> 11) & 31).astype(np.int64) * 255 // 31
    g = ((c >> 5) & 63).astype(np.int64) * 255 // 63
    b = (c & 31).astype(np.int64) * 255 // 31
    return np.stack([r, g, b], -1)


def _color_block(d, n, punch):
    c0 = d[:, 0].astype(np.uint32) | (d[:, 1].astype(np.uint32) << 8)
    c1 = d[:, 2].astype(np.uint32) | (d[:, 3].astype(np.uint32) << 8)
    idx = (d[:, 4].astype(np.uint32) | (d[:, 5].astype(np.uint32) << 8)
           | (d[:, 6].astype(np.uint32) << 16) | (d[:, 7].astype(np.uint32) << 24))
    p0, p1 = _c565(c0), _c565(c1)
    four = (c0 > c1) | (not punch)
    f = four[:, None]
    p2 = np.where(f, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(f, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)
    a = np.full((n, 4), 255, np.int64)
    if punch:
        a[:, 3] = np.where(four, 255, 0)
    sel = ((idx[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None, :]) & 3).astype(np.int64)
    rgb = np.take_along_axis(pal, sel[:, :, None].repeat(3, 2), 1)
    al = np.take_along_axis(a, sel, 1)
    return np.concatenate([rgb, al[:, :, None]], 2)


def dds_decode(b, o, hd):
    """Level 0 as (h, w, 4) uint8 RGBA, decoded the way the port decodes it."""
    w, h, fmt = hd["w"], hd["h"], hd["fmt"]
    raw = np.frombuffer(b, np.uint8, count=level0_size(hd), offset=o + 128)
    if fmt in ("DXT1", "DXT3", "DXT5"):
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        bs = 8 if fmt == "DXT1" else 16
        n = bw * bh
        d = raw.reshape(n, bs)
        if fmt == "DXT1":
            px = _color_block(d, n, True)
        else:
            px = _color_block(d[:, 8:], n, False)
            if fmt == "DXT3":
                ab = d[:, :8]
                px[:, :, 3] = np.stack([ab & 15, ab >> 4], 2).reshape(n, 16).astype(np.int64) * 17
            else:
                a0 = d[:, 0].astype(np.int64); a1 = d[:, 1].astype(np.int64)
                bits = np.zeros(n, np.uint64)
                for i in range(6):
                    bits |= d[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
                sel = ((bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)).astype(np.int64)
                k = np.arange(8)[None, :]
                big = (a0 > a1)[:, None]
                A0, A1 = a0[:, None], a1[:, None]
                pal8 = np.where(k == 0, A0, np.where(k == 1, A1,
                       np.where(big, ((8 - k) * A0 + (k - 1) * A1) // 7,
                                np.where(k < 6, ((6 - k) * A0 + (k - 1) * A1) // 5, np.where(k == 6, 0, 255)))))
                px[:, :, 3] = np.take_along_axis(pal8, sel, 1)
        img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)[:h, :w]
        return np.ascontiguousarray(img.astype(np.uint8))
    bpp = hd["bits"] // 8
    if bpp == 4:
        v = raw.view("<u4").reshape(h, w)
    elif bpp == 2:
        v = raw.view("<u2").reshape(h, w).astype(np.uint32)
    else:
        dd = raw.reshape(h, w, 3).astype(np.uint32)
        v = dd[:, :, 0] | (dd[:, :, 1] << 8) | (dd[:, :, 2] << 16)
    out = np.zeros((h, w, 4), np.uint8)
    for ch, m in enumerate(hd["masks"]):
        if not m:
            out[:, :, ch] = 255
            continue
        sh = (m & -m).bit_length() - 1
        mx = m >> sh
        out[:, :, ch] = ((v >> sh) & mx) * 255 // mx
    return out


def port_key(img):
    h, w = img.shape[:2]
    data = img[:, :, [2, 1, 0, 3]].tobytes()
    x = 1469598103934665603
    for by in data:
        x = ((x ^ by) * 1099511628211) & M64
    x = ((x ^ w) * 1099511628211) & M64
    x = ((x ^ h) * 1099511628211) & M64
    return "%dx%d_%016x" % (w, h, x)


# ───────────────────────────── archives ─────────────────────────────

def stm_entries(buf):
    """(path, offset, size) of every file in an @STM archive; offsets are absolute."""
    if buf[:4] != b"@STM":
        return []
    nroot, = struct.unpack_from("<I", buf, 0x1C)
    out = []

    def rec(first, count, prefix, depth):
        if depth > 12:
            return
        for i in range(first, first + count):
            o = i * 32
            name = bytes(buf[o:o + 24]).split(b"\0")[0].decode("latin1")
            off, size = struct.unpack_from("<II", buf, o + 24)
            if size & 0x80000000:
                rec(off // 32, size & 0x7FFFFFFF, prefix + name + "/", depth + 1)
            else:
                out.append((prefix + name, off, size))
    rec(1, nroot, "", 0)
    return out


def archive_list(work):
    """[(rel name as in game\\, file to read)]"""
    sd = os.path.join(ROOT, "game", "stm_data")
    out = [("stm_data\\" + os.path.basename(p), p) for p in sorted(glob.glob(os.path.join(sd, "*.stm")))]
    for p in sorted(glob.glob(os.path.join(sd, "*.lz"))):
        b = os.path.splitext(os.path.basename(p))[0]
        out.append(("stm_data\\" + b + ".lz", os.path.join(LZDIR, b + ".bin")))
    return out


_ABUF = {}


def read_archive(path):
    if path not in _ABUF:
        if len(_ABUF) > 6:
            _ABUF.clear()
        with open(path, "rb") as f:
            _ABUF[path] = f.read()
    return _ABUF[path]


def norm_loc(l):
    """(archive stem without language, entry with language folder suffix removed, language)."""
    base, lang = lang_of(l["arc"])
    parts = l["entry"].split("/")
    for i, part in enumerate(parts[:-1]):
        if len(part) > 2 and part[-2:] in LANGS:
            lang = lang or part[-2:]
            parts[i] = part[:-2] + "_*"
    return base, "/".join(parts), lang


def lang_of(arc):
    stem = os.path.splitext(os.path.basename(arc))[0]
    for l in LANGS:
        if stem.endswith(l):
            return stem[:-2], l
    return stem, ""


# ───────────────────────────── stage 1: unpack ─────────────────────────────

def stage_unpack(work):
    sd = os.path.join(ROOT, "game", "stm_data")
    od = LZDIR
    os.makedirs(od, exist_ok=True)
    jobs = []
    for p in sorted(glob.glob(os.path.join(sd, "*.lz"))):
        dst = os.path.join(od, os.path.splitext(os.path.basename(p))[0] + ".bin")
        if not os.path.exists(dst):
            jobs.append((p, dst))
    if jobs:
        t = time.time()
        with ProcessPoolExecutor() as ex:
            for _ in ex.map(lz_unpack._one, jobs):
                pass
        print("unpacked %d .lz files in %.0f s" % (len(jobs), time.time() - t))


# ───────────────────────────── stage 2: extract ─────────────────────────────

def _desc(img):
    from PIL import Image
    return np.asarray(Image.fromarray(img, "RGBA").resize((S, S), Image.BOX), np.float32)


def _extract_one(job):
    rel, path = job
    buf = read_archive(path)
    rows = []
    for name, off, size in stm_entries(buf):
        if off + size > len(buf) or buf[off:off + 4] != b"DDS ":
            continue
        hd = dds_header(buf, off)
        n0 = level0_size(hd) if hd else None
        if not n0 or 128 + n0 > size:
            continue
        uid = hashlib.sha1(b"%s %d %d " % (hd["fmt"].encode(), hd["w"], hd["h"]) + buf[off + 128:off + 128 + n0]).hexdigest()[:20]
        rows.append(dict(uid=uid, arc=rel, file=path, entry=name, off=off, size=size, fmt=hd["fmt"], w=hd["w"], h=hd["h"]))
    return rows


def _desc_one(loc):
    buf = read_archive(loc["file"])
    hd = dds_header(buf, loc["off"])
    img = dds_decode(buf, loc["off"], hd)
    return loc["uid"], _desc(img)


def stage_extract(work):
    tp = os.path.join(work, "xbox_textures.json")
    dp = os.path.join(work, "xbox_desc.npz")
    if os.path.exists(tp) and os.path.exists(dp):
        locs = json.load(open(tp))
        z = np.load(dp)
        return locs, dict(zip(z["uids"], z["desc"]))
    jobs = archive_list(work)
    locs = []
    with ProcessPoolExecutor() as ex:
        for rows in ex.map(_extract_one, jobs):
            locs.extend(rows)
    first = {}
    for l in locs:
        first.setdefault(l["uid"], l)
    t = time.time()
    with ProcessPoolExecutor() as ex:
        res = list(ex.map(_desc_one, first.values(), chunksize=16))
    print("extracted %d DDS entries, %d unique, from %d archives (%.0f s)" % (len(locs), len(first), len(jobs), time.time() - t))
    json.dump(locs, open(tp, "w"))
    np.savez(dp, uids=np.array([r[0] for r in res]), desc=np.stack([r[1] for r in res]))
    return locs, {r[0]: r[1] for r in res}


# ───────────────────────────── pack files ─────────────────────────────

def pcsx2_name(fn):
    """<tex0 hash>-<clut hash>-<bits>.dds; bits: PSM:6 TW:4 TH:4 TCC:1 (Hypothesis, fits every file)."""
    parts = os.path.splitext(fn)[0].split("-")
    a, c, bits = (parts[0], parts[1], parts[2]) if len(parts) == 3 else (parts[0], "", parts[1])
    v = int(bits, 16)
    return dict(texhash=a, cluthash=c, tw=1 << ((v >> 6) & 15), th=1 << ((v >> 10) & 15))


def _pack_one(p):
    from PIL import Image
    im = Image.open(p)
    im.load()
    a = np.asarray(im.convert("RGBA"))
    al = a[:, :, 3]
    return p, im.size[0], im.size[1], _desc(a), int(al.max()), float(al.mean())


def stage_packs(work, hd_dir, bt_dir):
    cp = os.path.join(work, "pack_desc.npz")
    files = [("hd", os.path.join(hd_dir, f)) for f in sorted(os.listdir(hd_dir)) if f.lower().endswith(".dds")]
    files += [("btn", os.path.join(bt_dir, f)) for f in sorted(os.listdir(bt_dir)) if f.lower().endswith(".dds")]
    names = ["%s/%s" % (k, os.path.basename(p)) for k, p in files]
    if os.path.exists(cp):
        z = np.load(cp)
        if list(z["names"]) == names:
            return files, names, z["dims"], z["desc"], z["amax"], z["amean"]
    t = time.time()
    with ProcessPoolExecutor() as ex:
        res = list(ex.map(_pack_one, [p for _, p in files], chunksize=4))
    print("decoded %d pack files (%.0f s)" % (len(res), time.time() - t))
    dims = np.array([(r[1], r[2]) for r in res]); desc = np.stack([r[3] for r in res])
    amax = np.array([r[4] for r in res]); amean = np.array([r[5] for r in res])
    np.savez(cp, names=np.array(names), dims=dims, desc=desc, amax=amax, amean=amean)
    return files, names, dims, desc, amax, amean


# ───────────────────────────── stage 3: match ─────────────────────────────

def _feats(d):
    rgb = d[..., :3] / 255.0
    al = d[..., 3] / 255.0
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    return rgb, al, lum


def _zn(x, w):
    sw = np.maximum(w.sum((-1, -2), keepdims=True), 1e-6)
    m = (x * w).sum((-1, -2), keepdims=True) / sw
    v = ((x - m) ** 2 * w).sum((-1, -2), keepdims=True) / sw
    return (x - m) / np.sqrt(v + 1e-6)


def coarse(names, pdesc, uids, xdims, xdesc, topn=10):
    prgb, pal, plum = _feats(pdesc)
    xrgb, xal, xlum = _feats(xdesc)
    ar = xdims[:, 0] / xdims[:, 1]
    out = {}
    for i, n in enumerate(names):
        nm = pcsx2_name(n.split("/", 1)[1])
        cand = np.where(np.isclose(ar, nm["tw"] / nm["th"]))[0]
        if not len(cand):
            out[n] = []
            continue
        w = np.maximum(pal[i][None], xal[cand]) + 0.02
        lc = (_zn(plum[i][None], w) * _zn(xlum[cand], w) * w).sum((1, 2)) / w.sum((1, 2))
        pa, xa = pal[i], xal[cand]
        if pa.std() < 0.02:
            ac = np.where(xa.std((1, 2)) < 0.02, 1.0 - np.abs(xa.mean((1, 2)) - pa.mean()), 0.0)
        else:
            ac = ((pa - pa.mean())[None] * (xa - xa.mean((1, 2), keepdims=True))).mean((1, 2)) / (pa.std() * xa.std((1, 2)) + 1e-6)
        r1 = _zn(prgb[i].transpose(2, 0, 1)[None], w[:, None]); r2 = _zn(xrgb[cand].transpose(0, 3, 1, 2), w[:, None])
        rm = np.sqrt((((r1 - r2) ** 2) * w[:, None]).sum((1, 2, 3)) / (3 * w.sum((1, 2))))
        score = lc * 0.5 + ac * 0.3 - rm * 0.2
        o = np.argsort(-score)[:topn]
        out[n] = [(str(uids[cand[j]]), float(score[j])) for j in o]
    return out


_L = np.array([0.299, 0.587, 0.114], np.float32)


def _prep(im, w, h):
    from PIL import Image
    a = np.asarray(im.resize((w, h), Image.BOX), np.float32) / 255.0
    return a[..., :3] @ _L, a[..., 3]


def _corr(x, y, wt):
    sw = wt.sum()
    mx = (x * wt).sum() / sw; my = (y * wt).sum() / sw
    vx = (((x - mx) ** 2) * wt).sum() / sw; vy = (((y - my) ** 2) * wt).sum() / sw
    if vx < 1e-5 and vy < 1e-5:
        return 1.0 - abs(mx - my)
    return float((((x - mx) * (y - my)) * wt).sum() / sw / np.sqrt(vx * vy + 1e-9))


def load_xbox_img(loc):
    buf = read_archive(loc["file"])
    return dds_decode(buf, loc["off"], dds_header(buf, loc["off"]))


def fine_score(pim, x):
    from PIL import Image
    h, w = x.shape[:2]
    s = min(1.0, 256 / max(w, h)); w2, h2 = max(4, int(w * s)), max(4, int(h * s))
    xl, xa = _prep(Image.fromarray(x, "RGBA"), w2, h2)
    pl, pa = _prep(pim, w2, h2)
    wt = np.maximum(xa, np.minimum(1, pa * 2)) + 0.02
    return 0.6 * _corr(pl, xl, wt) + 0.4 * _corr(pa, xa, np.ones_like(xa))


def _fine_one(job):
    """job: (pack path, [uid...], {uid: loc}) -> [(uid, fine score)]"""
    from PIL import Image
    p, cands, locs = job
    pim = Image.open(p).convert("RGBA")
    out = []
    for u in cands:
        out.append((u, float(fine_score(pim, load_xbox_img(locs[u])))))
    return out


def _key_one(loc):
    return loc["uid"], port_key(load_xbox_img(loc))


# ───────────────────────────── main ─────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hd", default=DEF_HD)
    ap.add_argument("--buttons", default=DEF_BT)
    ap.add_argument("--work", default=os.path.join(ROOT, "build_out", "research", "textures", "pipeline"))
    ap.add_argument("--out", default=os.path.join(ROOT, "assets", "texpacks"))
    ap.add_argument("--lzdir", default=os.path.join(ROOT, "build_out", "research", "textures", "lz"))
    ap.add_argument("--sheets", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    global LZDIR
    LZDIR = a.lzdir

    stage_unpack(a.work)
    locs, xdesc_map = stage_extract(a.work)
    by_uid = collections.defaultdict(list)
    for l in locs:
        by_uid[l["uid"]].append(l)
    uinfo = {u: ls[0] for u, ls in by_uid.items()}

    # language siblings: (base stem, entry path) -> {lang: uid}
    sib = collections.defaultdict(dict)
    for l in locs:
        base, ent, lang = norm_loc(l)
        if l["arc"].endswith(".stm"):
            sib[(base, ent)][lang] = l["uid"]
    has_lang = {k for k, v in sib.items() if any(x for x in v) and "" in v}

    def us_copy(u):
        """For a uid seen only in a neutral or non-US language archive that has a US (_u)
        sibling with other content: that sibling. Else None."""
        best = None
        for l in by_uid[u]:
            base, ent, lang = norm_loc(l)
            if lang == "_u":
                return None
            s = sib.get((base, ent), {})
            if "_u" in s and s["_u"] != u:
                best = s["_u"]
            else:
                return None        # also present somewhere without a US sibling
        return best

    files, names, pdims, pdesc, amax, amean = stage_packs(a.work, a.hd, a.buttons)
    ppath = dict(zip(names, [p for _, p in files]))
    uids = list(xdesc_map.keys())
    xdims = np.array([(uinfo[u]["w"], uinfo[u]["h"]) for u in uids])
    xdesc = np.stack([xdesc_map[u] for u in uids])
    fp = os.path.join(a.work, "fine.json")
    if os.path.exists(fp):
        fine = json.load(open(fp))
    else:
        cp = os.path.join(a.work, "coarse.json")
        if os.path.exists(cp):
            co = json.load(open(cp))
        else:
            t = time.time()
            co = coarse(names, pdesc, uids, xdims, xdesc)
            print("coarse pass %.0f s" % (time.time() - t))
            json.dump(co, open(cp, "w"))
        jobs = []
        for n in names:
            c = [u for u, s in co[n]]          # all top 10: faint textures (mist) score low coarse
            extra = [us_copy(u) for u in c]
            c = list(dict.fromkeys(c + [e for e in extra if e]))
            jobs.append((ppath[n], c, {u: uinfo[u] for u in c}))
        t = time.time()
        with ProcessPoolExecutor() as ex:
            res = list(ex.map(_fine_one, jobs, chunksize=2))
        print("fine pass %.0f s" % (time.time() - t))
        fine = {n: {u: round(s, 4) for u, s in r} for n, r in zip(names, res)}
        fine["_coarse"] = {n: co[n][:3] for n in names}
        json.dump(fine, open(fp, "w"))

    # ── choose targets per pack file ──
    pairs = []          # dict(pack, uid, score, ps_evidence)
    for n in names:
        sc = fine.get(n, {})
        if not sc:
            continue
        order = sorted(sc.items(), key=lambda kv: -kv[1])
        best = order[0][1]
        chosen = {}
        for u, s in order:
            if u != order[0][0] and not (s >= OK and s >= best - 0.02):
                break
            ps = None
            uc = us_copy(u)
            if uc:
                # neutral / other-language copy: map to the US copy instead
                ps = (u, s)
                l0 = by_uid[u][0]
                if (norm_loc(l0)[0], l0["entry"]) in GLYPH_ENTRIES:
                    # verified same picture apart from the glyphs (same cells, same order):
                    # keep the score of the match with the neutral PlayStation copy
                    pass
                else:
                    s = sc.get(uc)
                    if s is None:
                        continue
                u = uc
            if u not in chosen or chosen[u]["score"] < s:
                chosen[u] = dict(pack=n, uid=u, score=s, ps=ps)
        pairs.extend(chosen.values())

    # ── keys for every uid that may be used ──
    need = {p["uid"] for p in pairs if p["score"] >= CHECK}
    glyph = {}           # (base, entry) -> neutral uid, us uid, b uid  for differing pairs
    for k, v in sib.items():
        if k in GLYPH_ENTRIES and "" in v and "_u" in v and v[""] != v["_u"]:
            glyph[k] = v
            need.add(v["_u"])
            if "_b" in v:
                need.add(v["_b"])
    kp = os.path.join(a.work, "keys.json")
    keys = json.load(open(kp)) if os.path.exists(kp) else {}
    todo = [uinfo[u] for u in need if u not in keys]
    if todo:
        t = time.time()
        todo.sort(key=lambda l: -l["w"] * l["h"])
        with ProcessPoolExecutor() as ex:
            for u, k in ex.map(_key_one, todo, chunksize=1):
                keys[u] = k
        json.dump(keys, open(kp, "w"))
        print("computed %d keys (%.0f s)" % (len(todo), time.time() - t))

    # ── one pack file per key ──
    pinfo = {n: dict(dims=tuple(d), amax=int(am), amean=float(mn), bytes=os.path.getsize(ppath[n]))
             for n, d, am, mn in zip(names, pdims, amax, amean)}
    btn_names = {n.split("/", 1)[1] for n in names if n.startswith("btn/")}
    rows = {"hd": [], "btn": []}
    for pack in ("hd", "btn"):
        per = collections.defaultdict(list)
        for p in pairs:
            if p["pack"].startswith(pack + "/") and p["score"] >= CHECK:
                k = keys[p["uid"]]
                if (p["pack"].split("/", 1)[1], k) in REJECT or ("*", k) in REJECT:
                    continue
                per[k].append(p)
        for k, ps in per.items():
            ps.sort(key=lambda p: (-p["score"], -pinfo[p["pack"]]["bytes"]))
            top = ps[0]
            distinct = {pcsx2_name(p["pack"].split("/", 1)[1])["texhash"] for p in ps}
            fn = top["pack"].split("/", 1)[1]
            st = "ok" if top["score"] >= OK and len(distinct) == 1 else "check"
            flags = []
            pi = pinfo[top["pack"]]
            if pi["amax"] <= 140:
                flags.append("alpha2")
            elif pi["amax"] < 250:
                # ambiguous: compare mean alpha with the Xbox texture (PS2 alpha is half)
                xa = load_xbox_img(uinfo[top["uid"]])[:, :, 3].mean()
                if xa > 0 and pi["amean"] / xa < 0.7:
                    flags.append("alpha2")
            if pack == "btn":
                flags.append("style=modern")
            else:
                psent = None
                if top["ps"]:
                    l0 = by_uid[top["ps"][0]][0]
                    psent = (lang_of(l0["arc"])[0], l0["entry"])
                if fn in btn_names or psent in GLYPH_ENTRIES or fn in STYLE_PS_EXTRA:
                    flags.append("style=ps")
                elif psent:
                    # the PS2 picture is closer to the neutral (Japanese / PS2-era) copy than to
                    # the US Xbox copy: wording or logo differs from the Xbox game
                    flags.append("differs")
                    st = "check"
            ents = {l["entry"].rsplit("/", 1)[-1] for l in by_uid[top["uid"]]}
            if any(SLOT_PAGE.match(e) for e in ents) or fn in SLOT_FILES:
                # help page whose button legend is in PS2 slot order (see SLOT_PAGE)
                flags.append("slots=ps2")
                st = "check"
            if len(distinct) > 1:
                flags.append("many=%d" % len(distinct))
            flags += [st, "score=%.3f" % top["score"]]
            rows[pack].append(dict(key=k, file=fn, flags=flags, st=st, top=top, alts=ps[1:]))
        rows[pack].sort(key=lambda r: r["key"])

    write_maps(a, rows, names, ppath, keys, glyph, uinfo, by_uid)
    report(rows, names, pairs, keys, fine, a.work)
    if a.sheets:
        sheets(rows, ppath, uinfo, a.work)


def write_maps(a, rows, names, ppath, keys, glyph, uinfo, by_uid):
    os.makedirs(a.out, exist_ok=True)
    for pack, fname, folder in (("hd", "ps2_hd.map", a.hd), ("btn", "ps2_buttons.map", a.buttons)):
        fl = [f for f in os.listdir(folder) if f.lower().endswith(".dds")]
        tot = sum(os.path.getsize(os.path.join(folder, f)) for f in fl)
        pid = "ps2_hd" if pack == "hd" else "ps2_buttons"
        with open(os.path.join(a.out, fname), "w", newline="\n") as f:
            f.write("# Project Oogie texture map, written by scripts/build_texmap.py. No pixels: keys and file names only.\n")
            f.write("# pack %s SLUS-20860 files %d bytes %d\n" % (pid, len(fl), tot))
            f.write("# row: <WxH_key> <pack file name> <flags>\n")
            f.write("#  key   = port texture key of the Xbox texture (d3d11_backend.c tex_content_hash), W x H of the Xbox texture\n")
            f.write("#  flags = alpha2 (PS2 alpha 0..128: double it), style=ps (shows PlayStation glyphs), style=modern (button pack),\n")
            f.write("#          many=N (N different PS2 textures matched this key; best kept), ok|check, score=<match score 0..1>\n")
            f.write("# rows %d (ok %d, check %d)\n" % (len(rows[pack]), sum(r["st"] == "ok" for r in rows[pack]),
                                                    sum(r["st"] == "check" for r in rows[pack])))
            for r in rows[pack]:
                f.write("%s %s %s\n" % (r["key"], r["file"], " ".join(r["flags"])))
    gl = []
    for (base, entry), v in sorted(glyph.items()):
        n = [l for l in by_uid[v[""]] if norm_loc(l)[0] == base and norm_loc(l)[2] == ""][0]
        for lang in ("_u", "_b"):
            if lang in v and v[lang] != v[""]:
                gl.append((keys[v[lang]], n, lang))
    gl.sort(key=lambda g: g[0])
    with open(os.path.join(a.out, "ps_glyphs.map"), "w", newline="\n") as f:
        f.write("# Project Oogie PlayStation-glyph map, written by scripts/build_texmap.py. No pixels.\n")
        f.write("# For button style 'PlayStation (disc art)': the Xbox-glyph texture with <key> is replaced by the neutral\n")
        f.write("# (PlayStation-glyph) copy of the same entry, read at run time from the player's own game files.\n")
        f.write("# row: <WxH_key of the _u/_b copy> <archive relative to game\\> <entry> <offset> <size> <format> <WxH of the copy> <lang of key>\n")
        f.write("#  offset/size = the whole DDS file (128-byte header + levels) inside the archive; offset from the start of the archive\n")
        uniq = []
        for g in gl:
            if g[0] not in {u[0] for u in uniq}:
                uniq.append(g)
        f.write("# rows %d\n" % len(uniq))
        seen = set()
        for k, n, lang in uniq:
            if k in seen:
                continue
            seen.add(k)
            f.write("%s %s %s %d %d %s %dx%d %s\n" % (k, n["arc"], n["entry"], n["off"], n["size"], n["fmt"], n["w"], n["h"], lang))


def report(rows, names, pairs, keys, fine, work):
    used = {r["file"] for p in rows for r in rows[p]}
    alt = {a["pack"].split("/", 1)[1] for p in rows for r in rows[p] for a in r["alts"]}
    print("ps2_hd rows %d (ok %d, check %d); ps2_buttons rows %d" % (
        len(rows["hd"]), sum(r["st"] == "ok" for r in rows["hd"]), sum(r["st"] == "check" for r in rows["hd"]), len(rows["btn"])))
    print("distinct Xbox keys covered (hd):", len({r["key"] for r in rows["hd"]}))
    un = [n for n in names if n.split("/", 1)[1] not in used]
    why = collections.Counter()
    for n in un:
        f = n.split("/", 1)[1]
        best = max(fine.get(n, {}).values(), default=None)
        why["lost to a better pack file (CLUT variant / many-to-one)" if f in alt else
            "no same-aspect Xbox texture" if best is None else
            "best score < 0.70" if best < CHECK else "other"] += 1
    print("unused pack files:", len(un), dict(why))
    with open(os.path.join(work, "rows.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["pack", "key", "file", "flags", "xbox", "ps_from", "alts"])
        for p in rows:
            for r in rows[p]:
                t = r["top"]
                w.writerow([p, r["key"], r["file"], " ".join(r["flags"]), t["uid"], t["ps"][0] if t["ps"] else "",
                            ";".join("%s:%.3f" % (x["pack"].split("/", 1)[1], x["score"]) for x in r["alts"])])
    with open(os.path.join(work, "unused.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(["pack", "best_uid", "best_fine"])
        for n in un:
            sc = fine.get(n, {})
            b = max(sc.items(), key=lambda kv: kv[1]) if sc else ("", "")
            w.writerow([n, b[0], b[1]])


def sheets(rows, ppath, uinfo, work):
    from PIL import Image, ImageDraw
    od = os.path.join(work, "sheets")
    os.makedirs(od, exist_ok=True)
    T = 150

    def cell(a, ps2):
        if ps2 and a[..., 3].max() < 141:
            a = a.copy(); a[..., 3] = np.minimum(255, a[..., 3].astype(int) * 2)
        im = Image.fromarray(a, "RGBA"); im.thumbnail((T, T))
        bg = Image.new("RGBA", im.size, (90, 90, 120, 255))
        return Image.alpha_composite(bg, im)
    for pack in rows:
        sel = [r for r in rows[pack] if r["st"] == "check" or pack == "btn" or "style=ps" in r["flags"]]
        sel.sort(key=lambda r: r["top"]["score"])
        for s0 in range(0, len(sel), 24):
            part = sel[s0:s0 + 24]
            sheet = Image.new("RGBA", (4 * (2 * T + 10), 6 * (T + 16)), (40, 40, 40, 255))
            d = ImageDraw.Draw(sheet)
            for i, r in enumerate(part):
                x0, y0 = (i % 4) * (2 * T + 10), (i // 4) * (T + 16)
                pim = np.asarray(Image.open(ppath[pack + "/" + r["file"]]).convert("RGBA"))
                xim = load_xbox_img(uinfo[r["top"]["uid"]])
                sheet.paste(cell(pim, True), (x0, y0 + 14)); sheet.paste(cell(xim, False), (x0 + T + 4, y0 + 14))
                d.text((x0, y0), "%d %s %s" % (s0 + i, r["file"][:8], " ".join(f for f in r["flags"] if f not in ("alpha2", "check"))), fill=(255, 255, 0, 255))
            sheet.save(os.path.join(od, "%s_%03d.png" % (pack, s0 // 24)))


if __name__ == "__main__":
    main()
