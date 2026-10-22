# SPDX-License-Identifier: GPL-3.0-only
"""Drop-in for the optional C module gclib's yaz0_yay0.py imports (same four functions), backed by NXAccel.
Each function falls back to gclib's own pure-Python algorithm when the native code declines an input."""
import nxaccel

if not nxaccel.available():
    raise ImportError("NXAccel native library not available")


def _pure(kind, method, *args):
    from gclib import yaz0_yay0 as y
    from io import BytesIO
    saved = y.PY_FAST_YAZ0_YAY0_INSTALLED
    y.PY_FAST_YAZ0_YAY0_INSTALLED = False
    try:
        cls = y.Yaz0 if kind == "yaz0" else y.Yay0
        return getattr(cls, method)(BytesIO(args[0]), *args[1:]).getvalue()
    finally:
        y.PY_FAST_YAZ0_YAY0_INSTALLED = saved


def decompress_yaz0(data):
    out = nxaccel.yaz0_decompress(data)
    return out if out is not None else _pure("yaz0", "decompress", bytes(data))


def decompress_yay0(data):
    out = nxaccel.yay0_decompress(data)
    return out if out is not None else _pure("yay0", "decompress", bytes(data))


def compress_yaz0(data, search_depth=0x1000):
    out = nxaccel.yaz0_compress(data, search_depth)
    return out if out is not None else _pure("yaz0", "compress", bytes(data), search_depth)


def compress_yay0(data, search_depth=0x1000):
    return _pure("yay0", "compress", bytes(data), search_depth)
