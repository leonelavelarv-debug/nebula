# SPDX-License-Identifier: GPL-3.0-only
"""Byte-for-byte equivalence of NXAccel against the preparer's own Python, plus timings.

python tests/test_equivalence.py --gclib <preparers/mkw/vendor/gclib> --tools <preparers/mkw/tools> \
       --nxaccel <nxaccel/python> --lib <libnxaccel.so> [--quick]
"""
import argparse
import os
import random
import sys
import time
import types
from io import BytesIO

ap = argparse.ArgumentParser()
ap.add_argument("--gclib", required=True)
ap.add_argument("--tools", required=True)
ap.add_argument("--nxaccel", required=True)
ap.add_argument("--lib", required=True)
ap.add_argument("--quick", action="store_true")
args = ap.parse_args()
os.environ["NXACCEL_LIB"] = args.lib
sys.path[:0] = [args.nxaccel, args.gclib, args.tools]
try:
    import imagequant  # noqa: F401  (texture_utils imports it for encoding only)
except ImportError:
    sys.modules["imagequant"] = types.ModuleType("imagequant")

import nxaccel  # noqa: E402

assert nxaccel.available(), "libnxaccel not loaded"
from gclib import texture_utils as tu  # noqa: E402
from gclib.gx_enums import ImageFormat, PaletteFormat  # noqa: E402
import nw4r_audio  # noqa: E402

pure_decode = tu.decode_image
fast_decode = nxaccel._wrap_decode_image(pure_decode)
pure_adpcm = nw4r_audio.decode_adpcm
fast_adpcm = nxaccel._wrap_decode_adpcm(pure_adpcm)
rng = random.Random(20261022)
failures = 0


def report(name, ok, detail=""):
    global failures
    failures += not ok
    print(f"{'OK   ' if ok else 'FALLO'} {name} {detail}")


# ------------------------------------------------------------------------------------------- textures ----
sizes = [(1, 1), (8, 8), (13, 7), (64, 32), (37, 45)] if args.quick else [(1, 1), (4, 4), (8, 8), (13, 7), (64, 32), (37, 45), (128, 64), (3, 129)]
for fmt in [ImageFormat.I4, ImageFormat.I8, ImageFormat.IA4, ImageFormat.IA8, ImageFormat.RGB565, ImageFormat.RGB5A3,
            ImageFormat.RGBA32, ImageFormat.C4, ImageFormat.C8, ImageFormat.C14X2, ImageFormat.CMPR]:
    bw, bh, bs = tu.BLOCK_WIDTHS[fmt], tu.BLOCK_HEIGHTS[fmt], tu.BLOCK_DATA_SIZES[fmt]
    pal_formats = [PaletteFormat.IA8, PaletteFormat.RGB565, PaletteFormat.RGB5A3] if fmt in tu.IMAGE_FORMATS_THAT_USE_PALETTES else [PaletteFormat.IA8]
    ok = True
    for (w, h) in sizes:
        for pfmt in pal_formats:
            n = ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bs
            data = bytes(rng.getrandbits(8) for _ in range(n))
            ncol = {ImageFormat.C4: 16, ImageFormat.C8: 256, ImageFormat.C14X2: 1 << 14}.get(fmt, 0)
            pal = bytes(rng.getrandbits(8) for _ in range(2 * ncol))
            a = pure_decode(BytesIO(data), BytesIO(pal), fmt, pfmt, ncol, w, h)
            b = fast_decode(BytesIO(data), BytesIO(pal), fmt, pfmt, ncol, w, h)
            if a.mode != b.mode or a.size != b.size or a.tobytes() != b.tobytes():
                ok = False
                print(f"   difiere {fmt.name} {w}x{h} pal={pfmt.name}")
    report(f"decode_image {fmt.name}", ok, f"{len(sizes) * len(pal_formats)} casos")

# Out-of-palette indexes: the native path declines and the original runs (same result or same exception).
data = bytes([0xFF] * 32)
pal = bytes(4)
try:
    a = pure_decode(BytesIO(data), BytesIO(pal), ImageFormat.C8, PaletteFormat.RGB565, 2, 8, 4).tobytes()
except Exception as e:  # noqa: BLE001
    a = repr(type(e))
try:
    b = fast_decode(BytesIO(data), BytesIO(pal), ImageFormat.C8, PaletteFormat.RGB565, 2, 8, 4).tobytes()
except Exception as e:  # noqa: BLE001
    b = repr(type(e))
report("decode_image indice fuera de paleta -> Python original", a == b, str(a)[:40])

# ------------------------------------------------------------------------------------------- ADPCM -------
ok = True
for trial in range(20 if args.quick else 60):
    frames = rng.randint(1, 40)
    raw = bytes(rng.getrandbits(8) & (0x7F if i % 8 == 0 else 0xFF) for i in range(frames * 8 + 8))
    coefs = [rng.randint(-32768, 32767) for _ in range(16)]
    samples = rng.randint(0, frames * 14)
    yn1, yn2 = rng.randint(-32768, 32767), rng.randint(-32768, 32767)
    if pure_adpcm(raw, 0, samples, coefs, 0, yn1, yn2) != fast_adpcm(raw, 0, samples, coefs, 0, yn1, yn2):
        ok = False
report("decode_adpcm", ok, "casos aleatorios (predictor 0-7, historiales extremos)")

# ------------------------------------------------------------------------------------------- Yaz0/Yay0 ---
from gclib import yaz0_yay0 as yz  # noqa: E402

yz.PY_FAST_YAZ0_YAY0_INSTALLED = False


def sample(n):
    words = [bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 12))) for _ in range(24)]
    out = bytearray()
    while len(out) < n:
        out += rng.choice(words) if rng.random() < 0.8 else bytes([rng.getrandbits(8)])
    return bytes(out[:n])


ok_c = ok_d = ok_y = True
for n in ([0, 1, 5, 300, 2000] if args.quick else [0, 1, 2, 3, 17, 300, 2000, 6000]):
    src = sample(n)
    pure_c = yz.Yaz0.compress(BytesIO(src)).getvalue()
    fast_c = nxaccel.yaz0_compress(src)
    ok_c = ok_c and pure_c == fast_c
    ok_d = ok_d and nxaccel.yaz0_decompress(pure_c) == yz.Yaz0.decompress(BytesIO(pure_c)).getvalue() == src
    if n <= 2000:
        y0 = yz.Yay0.compress(BytesIO(src)).getvalue()
        ok_y = ok_y and nxaccel.yay0_decompress(y0) == yz.Yay0.decompress(BytesIO(y0)).getvalue()
report("Yaz0 compress (identico a gclib)", ok_c)
report("Yaz0 decompress", ok_d)
report("Yay0 decompress", ok_y)

# ------------------------------------------------------------------------------------------- timings -----
print("-- tiempos (mismo interprete, mismos datos)")


def bench(name, slow, fast, reps=1):
    t0 = time.perf_counter()
    for _ in range(reps):
        slow()
    t1 = time.perf_counter()
    for _ in range(reps):
        fast()
    t2 = time.perf_counter()
    print(f"   {name:34s} Python {1e3 * (t1 - t0) / reps:9.1f} ms   NXAccel {1e3 * (t2 - t1) / reps:8.2f} ms   x{(t1 - t0) / max(t2 - t1, 1e-9):7.0f}")


side = 64 if args.quick else 256
for fmt in (ImageFormat.CMPR, ImageFormat.RGB5A3, ImageFormat.I4):
    n = (side // tu.BLOCK_WIDTHS[fmt]) * (side // tu.BLOCK_HEIGHTS[fmt]) * tu.BLOCK_DATA_SIZES[fmt]
    data = bytes(rng.getrandbits(8) for _ in range(n))
    bench(f"decode_image {fmt.name} {side}x{side}", lambda: pure_decode(BytesIO(data), BytesIO(), fmt, PaletteFormat.IA8, 0, side, side),
          lambda: fast_decode(BytesIO(data), BytesIO(), fmt, PaletteFormat.IA8, 0, side, side))
big = sample(32 * 1024 if args.quick else 256 * 1024)
comp = nxaccel.yaz0_compress(big, 0x200)
bench(f"Yaz0 decompress {len(big) // 1024} KiB", lambda: yz.Yaz0.decompress(BytesIO(comp)), lambda: nxaccel.yaz0_decompress(comp))
raw = bytes(rng.getrandbits(8) & (0x7F if i % 8 == 0 else 0xFF) for i in range(8 * 4000))
coefs = [rng.randint(-2048, 2047) for _ in range(16)]
bench("decode_adpcm 56000 muestras", lambda: pure_adpcm(raw, 0, 56000, coefs, 0, 0, 0), lambda: fast_adpcm(raw, 0, 56000, coefs, 0, 0, 0))

# The import hook: tools doing `from gclib.texture_utils import decode_image` get the native one.
del sys.modules["gclib.texture_utils"]
nxaccel.install()
from gclib.texture_utils import decode_image as hooked  # noqa: E402
report("gancho de importacion (from gclib.texture_utils import decode_image)", getattr(hooked, "__nxaccel__", False))

print("RESULTADO:", "TODO IDENTICO" if failures == 0 else f"{failures} FALLOS")
sys.exit(1 if failures else 0)
