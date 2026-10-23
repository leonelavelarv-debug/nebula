#!/usr/bin/env bash
# nebula-pak's tests: synthetic RMGE01-shaped discs in every format it reads must give exactly the expected game.pak.
#   run_tests.sh <nebula-pak> <work dir> <zstd> [wit]
set -euo pipefail
pak=$1; work=$2; zstd=$3; wit=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$work"
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
leftover=$(find "$work/out" -name '.*importing' -o -name 'cut.pak' -o -name 'other.pak' -o -name 'dol.pak' | wc -l)
[ "$leftover" = 0 ] || { echo "FALLO quedaron ficheros a medias"; failed=1; }
exit $failed
