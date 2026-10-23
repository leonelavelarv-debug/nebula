#!/usr/bin/env bash
# nebula-pak's tests: synthetic RMGE01-shaped discs in every format it reads must give exactly the expected game.pak.
#   [XDELTA3=<xdelta3>] run_tests.sh <nebula-pak> <work dir> <zstd> [wit]
set -euo pipefail
pak=$1; work=$2; zstd=$3; wit=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$work"
rm -rf "$work/out"
python3 -I "$here/make_test_discs.py" "$work" "$zstd"
export NEBULA_PAK_DOL_SHA1=$(cat "$work/dol.sha1")
images=("$work/smg.iso" "$work/smg.gcz" "$work/smg.rvz" "$work/smg-2m.rvz")
if [ -n "$wit" ]; then
  for kind in wbfs ciso wia=none wia=purge wia=bzip2 wia=lzma wia=lzma2; do
    name=${kind//=/-}
    ext=${name%%-*}
    out="$work/smg-$name.$ext"
    [ -f "$out" ] || "$wit" copy -q --overwrite "--$kind" "$work/smg.iso" "$out"
    images+=("$out")
  done
  # A split WBFS: the same image cut in two (name.wbfs + name.wbf1).
  if [ ! -f "$work/split.wbf1" ]; then
    head -c 4194304 "$work/smg-wbfs.wbfs" > "$work/split.wbfs"
    tail -c +4194305 "$work/smg-wbfs.wbfs" > "$work/split.wbf1"
  fi
  images+=("$work/split.wbfs")
fi
failed=0
for image in "${images[@]}"; do
  out="$work/out/$(basename "$image").pak"
  rm -f "$out"
  if "$pak" "$image" "$out" > "$work/log.txt" 2>&1 && cmp -s "$out" "$work/expected.pak"; then
    echo "ok    $(basename "$image") ($(head -1 "$work/log.txt"))"
  else
    echo "FALLO $(basename "$image")"; cat "$work/log.txt"; failed=1
  fi
done
# The raw (encrypted) disc rebuilt from RVZ must be the ISO's, byte for byte (hashes, exceptions, encryption).
python3 -I - "$work/smg.iso" "$work/raw-iso.bin" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    f.seek(0xF800000)
    open(sys.argv[2], "wb").write(f.read(0xE20000))
PY
for image in smg.rvz smg-2m.rvz smg.gcz; do
  "$pak" --dump-raw "$work/$image" 0xF800000 0xE20000 "$work/raw.bin" && cmp -s "$work/raw.bin" "$work/raw-iso.bin" &&
    echo "ok    $image: disco cifrado reconstruido igual" || { echo "FALLO $image: disco cifrado distinto"; failed=1; }
done
# An xdelta patch (a translation-like change) applied on the way, from every format.
if [ -n "${XDELTA3:-}" ]; then
  rm -f "$work/ptbr.xdelta"
  "$XDELTA3" -e -f -S none -s "$work/smg.iso" "$work/smg-ptbr.iso" "$work/ptbr.xdelta"
  for image in "${images[@]}"; do
    out="$work/out/$(basename "$image").ptbr.pak"
    rm -f "$out"
    if "$pak" --patch "$work/ptbr.xdelta" "$image" "$out" > "$work/log.txt" 2>&1 && cmp -s "$out" "$work/expected-ptbr.pak" &&
       grep -q "PATCH_LANGUAGE	english" "$work/log.txt"; then
      echo "ok    parche sobre $(basename "$image") ($(grep -c PATCHED "$work/log.txt") fichero(s); $(grep -o 'Aviso' "$work/log.txt" | head -1))"
    else
      echo "FALLO parche sobre $(basename "$image")"; tail -5 "$work/log.txt"; failed=1
    fi
  done
  # A patch for another disc is refused: a "source" whose hash block of the patched file's cluster differs.
  python3 -I - "$work/smg.iso" "$work/other-src.iso" "$work/expected.pak" "$work/expected-ptbr.pak" <<'PY'
import shutil, sys
shutil.copyfile(sys.argv[1], sys.argv[2])
a, b = open(sys.argv[3], "rb").read(), open(sys.argv[4], "rb").read()
first = next(i for i in range(len(a)) if a[i] != b[i])
with open(sys.argv[2], "r+b") as f:
    f.seek(0xF820000 + first // 0x7C00 * 0x8000)
    f.write(bytes(range(0x100)))
PY
  if "$pak" --patch "$work/ptbr.xdelta" "$work/other-src.iso" "$work/out/wrong.pak" > "$work/log.txt" 2>&1; then
    echo "FALLO un parche aplicado al disco equivocado se acepto"; failed=1
  else
    echo "ok    parche sobre el disco equivocado rechazado: $(grep Error "$work/log.txt")"
  fi
fi
# Damaged and wrong images must fail without leaving anything behind.
head -c 3000000 "$work/smg.rvz" > "$work/cut.rvz"
if "$pak" "$work/cut.rvz" "$work/out/cut.pak" > "$work/log.txt" 2>&1; then echo "FALLO cut.rvz se acepto"; failed=1;
else echo "ok    cut.rvz rechazada: $(grep Error "$work/log.txt")"; fi
python3 -I - "$work/smg.iso" "$work/other.iso" <<'PY'
import shutil, sys
shutil.copyfile(sys.argv[1], sys.argv[2])
with open(sys.argv[2], "r+b") as f:
    f.write(b"RMGP01")
PY
if "$pak" "$work/other.iso" "$work/out/other.pak" > "$work/log.txt" 2>&1; then echo "FALLO other.iso se acepto"; failed=1;
else echo "ok    other.iso rechazada: $(head -1 "$work/log.txt")"; fi
if NEBULA_PAK_DOL_SHA1=0000 "$pak" "$work/smg.rvz" "$work/out/dol.pak" > "$work/log.txt" 2>&1; then echo "FALLO dol"; failed=1;
else echo "ok    main.dol distinto rechazado: $(grep Error "$work/log.txt")"; fi
leftover=$(find "$work/out" -name '.*importing' -o -name 'cut.pak' -o -name 'other.pak' -o -name 'dol.pak' -o -name 'wrong.pak' | wc -l)
[ "$leftover" = 0 ] || { echo "FALLO quedaron ficheros a medias"; failed=1; }
exit $failed
