#!/bin/bash
# Empaqueta el port de Super Mario Galaxy para todos los firmwares, desde una carpeta con "Super Mario Galaxy.sh" y
# smg/ (los binarios ya compilados):
#   smg.zip                          formato de PortMaster (Super Mario Galaxy.sh arriba, smg/ con port.json y
#                                    gameinfo.xml): PortMaster lo instala en las rutas de cada firmware si se deja en
#                                    su carpeta autoinstall/ o con "Instalar desde zip".
#   Instalar Super Mario Galaxy.sh   instalador autoextraible (instalador.sh + el mismo contenido en tar.gz): se copia
#                                    a la carpeta de ports del menu y se abre desde Ports.
# Uso: empaquetar.sh <carpeta del port> <carpeta de salida>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$(cd "$1" && pwd)"
out="$(mkdir -p "$2" && cd "$2" && pwd)"
[ -f "$src/Super Mario Galaxy.sh" ] && [ -x "$src/smg/nebula" ] || { echo "falta $src/Super Mario Galaxy.sh o smg/nebula" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cp "$src/Super Mario Galaxy.sh" "$stage/"
# Lo que es del jugador o de una partida no va en el paquete (el instalador lo conserva en la consola).
tar -C "$src" -cf - \
  --exclude='smg/gamedata/*.pak' --exclude='smg/gamedata/*.iso' --exclude='smg/gamedata/*.rvz' \
  --exclude='smg/gamedata/*.wia' --exclude='smg/gamedata/*.wbfs' --exclude='smg/gamedata/*.gcz' \
  --exclude='smg/gamedata/*.ciso' --exclude='smg/gamedata/*.idioma' \
  --exclude='smg/nand' --exclude='smg/data' --exclude='smg/shots' --exclude='smg/crashes' \
  --exclude='smg/log.txt' --exclude='smg/idioma.txt' --exclude='smg/.primer-arranque' \
  --exclude='smg/.ficha-pendiente' --exclude='smg/cover.png*' --exclude='smg/screenshot.png' \
  --exclude='smg/title.png' --exclude='smg/*.sh' --exclude='__pycache__' smg | tar -C "$stage" -xf -
cp "$here/smg/port.json" "$stage/smg/port.json"
chmod +x "$stage/Super Mario Galaxy.sh" "$stage/smg/nebula" "$stage/smg/nebula-pak" \
  "$stage/smg/cactus-galaxy-wrapper" "$stage/smg/gamelist.py" 2>/dev/null || true

# smg.zip para PortMaster (sin README.md arriba: PortMaster lo moveria a smg/ encima del de los controles).
rm -f "$out/smg.zip"
(cd "$stage" && find "Super Mario Galaxy.sh" smg -type f | LC_ALL=C sort | zip -q -X -9 "$out/smg.zip" -@)

# El instalador: plantilla + tar.gz (ustar, lo abre el tar de busybox).
size_kb=$(du -sk "$stage" | cut -f1)
need_kb=$(( size_kb * 2 + 4096 ))  # lo descomprimido en .smg-instalando y la copia en smg/
installer="$out/Instalar Super Mario Galaxy.sh"
sed "s/__TAMANO_KB__/$need_kb/" "$here/instalador.sh" > "$installer"
tail -n 1 "$installer" | grep -qx '__CARGA_SMG__' || { echo "instalador.sh debe acabar en __CARGA_SMG__" >&2; exit 1; }
(cd "$stage" && find "Super Mario Galaxy.sh" smg | LC_ALL=C sort |
  tar --format=ustar --owner=0 --group=0 --numeric-owner --no-recursion -cf - -T - | gzip -9n) >> "$installer"
chmod +x "$installer"
ls -l "$out/smg.zip" "$installer"
