# SPDX-License-Identifier: GPL-3.0-only
"""NXAccel: native hot paths for the NextOS Mario Kart Wii preparer.

Loads libnxaccel.so with ctypes and, through install(), replaces three pure-Python hot loops with native code
that gives byte-identical results:

* gclib.texture_utils.decode_image   (GX texture formats -> PIL RGBA image)
* nw4r_audio.decode_adpcm            (DSP-ADPCM -> list of int16)
* pyfastyaz0yay0                     (the optional module gclib already looks for: Yaz0/Yay0)

Anything the native code declines (truncated data, out-of-palette indexes, unknown formats) runs the original
Python, so errors and edge cases behave exactly as before. NXACCEL=0 turns everything off.
"""
import ctypes
import os
import sys

__version__ = "0.1.0"
_lib = None
_patched = set()
# NXACCEL_STATS=<file>: at exit, one JSON line per process with calls, native hits, fallbacks and seconds per
# function, so a real preparation run shows how much of its time these paths cover.
_stats = {}


def _count(name, native, seconds):
    s = _stats.setdefault(name, [0, 0, 0.0])
    s[0] += 1
    s[1] += 1 if native else 0
    s[2] += seconds


def _write_stats():
    path = os.environ.get("NXACCEL_STATS")
    if not path or not _stats:
        return
    import json
    import time
    try:
        with open(path, "a") as f:
            f.write(json.dumps({"pid": os.getpid(), "argv": sys.argv[:3], "time": time.time(),
                                "functions": {k: {"calls": v[0], "native": v[1], "seconds": round(v[2], 4)} for k, v in _stats.items()}}) + "\n")
    except OSError:
        pass


import atexit as _atexit  # noqa: E402
_atexit.register(_write_stats)


def _load():
    global _lib
    if _lib is not None:
        return _lib
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [os.environ.get("NXACCEL_LIB", ""), os.path.join(here, "libnxaccel.so")]
    for path in candidates:
        if path and os.path.exists(path):
            lib = ctypes.CDLL(path)
            break
    else:
        return None
    c_long, c_size, c_int, c_void = ctypes.c_long, ctypes.c_size_t, ctypes.c_int, ctypes.c_void_p
    lib.nxa_version.restype = c_int
    for name in ("nxa_yaz0_decompress", "nxa_yay0_decompress"):
        f = getattr(lib, name)
        f.argtypes = [ctypes.c_char_p, c_size, c_void, c_size]
        f.restype = c_long
    lib.nxa_yaz0_compress.argtypes = [ctypes.c_char_p, c_size, c_void, c_size, c_long]
    lib.nxa_yaz0_compress.restype = c_long
    lib.nxa_gx_decode.argtypes = [c_int, ctypes.c_char_p, c_size, ctypes.c_char_p, c_size, c_int, c_int, c_int, c_int, c_void]
    lib.nxa_gx_decode.restype = c_long
    lib.nxa_adpcm_decode.argtypes = [ctypes.c_char_p, c_size, c_size, c_size, ctypes.POINTER(c_int), c_int, c_int, c_void]
    lib.nxa_adpcm_decode.restype = c_long
    _lib = lib
    return lib


def _now():
    import time
    return time.perf_counter()


def available():
    return os.environ.get("NXACCEL", "1") != "0" and _load() is not None


# ---------------------------------------------------------------------------------------------- Yaz0 / Yay0 ---
def _decompress(fn, data):
    data = bytes(data)
    size = int.from_bytes(data[4:8], "big") if len(data) >= 8 else 0
    out = ctypes.create_string_buffer(size + 0x111)
    n = fn(data, len(data), out, len(out))
    return None if n < 0 else out.raw[:n]


def yaz0_decompress(data):
    t0 = _now()
    out = _decompress(_lib.nxa_yaz0_decompress, data)
    _count("yaz0_decompress", out is not None, _now() - t0)
    return out


def yay0_decompress(data):
    return _decompress(_lib.nxa_yay0_decompress, data)


def yaz0_compress(data, search_depth=0x1000):
    data = bytes(data)
    out = ctypes.create_string_buffer(16 + len(data) + len(data) // 8 + 16)
    n = _lib.nxa_yaz0_compress(data, len(data), out, len(out), int(search_depth))
    return None if n < 0 else out.raw[:n]


# --------------------------------------------------------------------------------------------- GX textures ---
def _wrap_decode_image(original):
    from PIL import Image

    def decode_image(image_data, palette_data, image_format, palette_format, num_colors, image_width, image_height):
        t0 = _now()
        try:
            raw = image_data.getvalue() if hasattr(image_data, "getvalue") else bytes(image_data)
            pal = palette_data.getvalue() if hasattr(palette_data, "getvalue") else bytes(palette_data or b"")
            fmt = int(image_format.value if hasattr(image_format, "value") else image_format)
            pfmt = int(palette_format.value if hasattr(palette_format, "value") else palette_format)
            w, h = int(image_width), int(image_height)
            out = ctypes.create_string_buffer(w * h * 4)
            if w > 0 and h > 0 and _lib.nxa_gx_decode(fmt, raw, len(raw), pal, len(pal), pfmt, int(num_colors), w, h, out) == 0:
                image = Image.frombytes("RGBA", (w, h), out.raw)
                _count("decode_image", True, _now() - t0)
                return image
        except Exception:
            pass
        try:
            return original(image_data, palette_data, image_format, palette_format, num_colors, image_width, image_height)
        finally:
            _count("decode_image", False, _now() - t0)

    decode_image.__wrapped__ = original
    decode_image.__nxaccel__ = True
    return decode_image


# ----------------------------------------------------------------------------------------------- DSP-ADPCM ---
def _wrap_decode_adpcm(original):
    import array

    def decode_adpcm(data, start, samples, coefs, ps, yn1, yn2):
        t0 = _now()
        try:
            if len(coefs) >= 16 and samples >= 0:
                raw = bytes(data)
                c = (ctypes.c_int * 16)(*[int(x) for x in coefs[:16]])
                out = array.array("h", bytes(2 * samples))
                addr, _ = out.buffer_info()
                if _lib.nxa_adpcm_decode(raw, len(raw), int(start), int(samples), c, int(yn1), int(yn2), addr) == samples:
                    result = out.tolist()
                    _count("decode_adpcm", True, _now() - t0)
                    return result
        except Exception:
            pass
        try:
            return original(data, start, samples, coefs, ps, yn1, yn2)
        finally:
            _count("decode_adpcm", False, _now() - t0)

    decode_adpcm.__wrapped__ = original
    decode_adpcm.__nxaccel__ = True
    return decode_adpcm


def patch_module(module):
    """Patches an already imported gclib.texture_utils or nw4r_audio module."""
    name = module.__name__
    if name == "gclib.texture_utils" and not getattr(module.decode_image, "__nxaccel__", False):
        module.decode_image = _wrap_decode_image(module.decode_image)
        _patched.add(name)
    elif name == "nw4r_audio" and not getattr(module.decode_adpcm, "__nxaccel__", False):
        module.decode_adpcm = _wrap_decode_adpcm(module.decode_adpcm)
        _patched.add(name)


class _Finder:
    """Meta-path finder that patches the target modules right after they execute, so `from gclib.texture_utils
    import decode_image` in the cooking tools already receives the native version."""

    targets = ("gclib.texture_utils", "nw4r_audio")

    def find_spec(self, fullname, path, target=None):
        if fullname not in self.targets:
            return None
        for finder in sys.meta_path:
            if finder is self or not hasattr(finder, "find_spec"):
                continue
            spec = finder.find_spec(fullname, path, target)
            if spec is not None and spec.loader is not None:
                loader = spec.loader
                exec_module = loader.exec_module

                def patched_exec(module, _exec=exec_module):
                    _exec(module)
                    patch_module(module)

                try:
                    loader.exec_module = patched_exec
                except Exception:
                    return None
                return spec
        return None


def install():
    if not available():
        return False
    if not any(isinstance(f, _Finder) for f in sys.meta_path):
        sys.meta_path.insert(0, _Finder())
    for name in _Finder.targets:
        if name in sys.modules:
            patch_module(sys.modules[name])
    return True
