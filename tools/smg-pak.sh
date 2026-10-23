#!/usr/bin/env bash
# smg-pak.sh: game.pak (lo que juega el port de Super Mario Galaxy en la RG35XX H) desde TU disco del juego en ISO, RVZ,
# WIA, WBFS, GCZ o CISO. Para Termux (Android), Linux o macOS; usa nebula-pak (crates/nebula-import) y lo compila si
# hace falta.
#
#   bash smg-pak.sh                         busca la imagen en Descargas (Termux) y en la carpeta actual
#   bash smg-pak.sh juego.rvz               esa imagen; game.pak queda a su lado
#   bash smg-pak.sh juego.iso /ruta/smg     game.pak directo en /ruta/smg/gamedata/ (la carpeta del port, en la SD)
#   bash smg-pak.sh juego.iso salida.pak    con ese nombre
#
# Solo vale el Super Mario Galaxy americano (RMGE01): nebula-pak lo comprueba (y main.dol por su SHA-1).
set -euo pipefail

say() { printf '%s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
termux=false
case "${PREFIX:-}" in *com.termux*) termux=true ;; esac

# 1. La imagen del disco.
find_images() {
  local dir
  for dir in "$@"; do
    [ -d "$dir" ] || continue
    find "$dir" -maxdepth 2 -type f \( -iname '*.iso' -o -iname '*.rvz' -o -iname '*.wia' -o -iname '*.wbfs' \
      -o -iname '*.gcz' -o -iname '*.ciso' \) 2>/dev/null
  done
}
image="${1:-}"
if [ -z "$image" ] || [ -d "$image" ]; then
  dirs=()
  [ -n "$image" ] && dirs+=("$image")
  $termux && dirs+=("$HOME/storage/downloads" "$HOME/storage/shared/Download")
  dirs+=("$PWD")
  found=()
  while IFS= read -r f; do found+=("$f"); done < <(find_images "${dirs[@]}" | sort -u)
  galaxy=()
  for f in ${found[@]+"${found[@]}"}; do
    case "$(basename "$f" | tr '[:upper:]' '[:lower:]')" in *galaxy*|*rmge01*|*smg*) galaxy+=("$f") ;; esac
  done
  [ ${#galaxy[@]} -gt 0 ] && found=("${galaxy[@]}")
  if [ ${#found[@]} -eq 0 ]; then
    fail "no encontre ninguna imagen (.iso .rvz .wia .wbfs .gcz .ciso) en: ${dirs[*]}. Pasala como argumento."
  elif [ ${#found[@]} -gt 1 ]; then
    say "Hay varias imagenes; elige una y pasala como argumento:"
    printf '  %s\n' "${found[@]}"
    exit 2
  fi
  image="${found[0]}"
fi
[ -f "$image" ] || fail "no existe: $image"

# 2. Donde dejar game.pak.
out="${2:-}"
if [ -z "$out" ]; then
  out="$(dirname "$image")/game.pak"
elif [ -d "$out" ]; then
  if [ -d "$out/gamedata" ] || [ -f "$out/nebula" ]; then
    mkdir -p "$out/gamedata"
    out="$out/gamedata/game.pak"
  else
    out="$out/game.pak"
  fi
fi
mkdir -p "$(dirname "$out")"

# 3. Espacio: el game.pak ocupa unos 4.1 GB (la particion del juego, sin cifrar).
need_kb=$((4300 * 1024))
free_kb=$(df -k "$(dirname "$out")" 2>/dev/null | awk 'END {print $(NF-2)}')
case "$free_kb" in ''|*[!0-9]*) free_kb="" ;; esac
if [ -n "$free_kb" ] && [ "$free_kb" -lt "$need_kb" ]; then
  fail "hacen falta ~4.3 GB libres en $(dirname "$out") (hay $((free_kb / 1024)) MB)."
fi

# 4. nebula-pak: el de PATH, el ya compilado del repo, o se compila (cargo; en Termux se instala rust si falta).
pak_tool="$(command -v nebula-pak 2>/dev/null || true)"
[ -z "$pak_tool" ] && [ -x "$repo/target/release/nebula-pak" ] && pak_tool="$repo/target/release/nebula-pak"
if [ -z "$pak_tool" ]; then
  if ! command -v cargo >/dev/null 2>&1; then
    if $termux; then
      say "Instalando Rust (pkg install rust)..."
      pkg install -y rust
    else
      fail "falta cargo (Rust): instalalo (https://rustup.rs) y vuelve a ejecutar."
    fi
  fi
  [ -f "$repo/Cargo.toml" ] || fail "no encuentro el repositorio de Nebula en $repo"
  say "Compilando nebula-pak (la primera vez tarda unos minutos)..."
  cargo build --release --manifest-path "$repo/Cargo.toml" -p nebula-import --bin nebula-pak
  pak_tool="$repo/target/release/nebula-pak"
fi

# 5. La conversion.
say "Imagen:  $image"
say "Salida:  $out"
start=$(date +%s)
"$pak_tool" "$image" "$out.part"
mv -f "$out.part" "$out"
say "Hecho en $(( $(date +%s) - start )) s: $out ($(du -h "$out" | cut -f1))"
say "Copialo a la consola en: roms/ports/smg/gamedata/game.pak"
