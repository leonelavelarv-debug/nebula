#!/bin/bash
# PORTMASTER: smg.zip, Cactus Galaxy Wrapper.sh
# Cactus Galaxy Wrapper: convierte tu disco de Super Mario Galaxy (USA, RMGE01; ISO, RVZ, WIA, WBFS, GCZ o CISO, en
# roms/ports/smg/gamedata/ o en roms/wii/) en el game.pak del port, en la propia consola. Va con el port "smg".
#   Cruceta: elegir.  A: convertir.  B: salir o cancelar.  X: borrar el disco al terminar.  MENU + START: salir.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/smg"
cd "$GAMEDIR" || exit 1
exec > >(tee "$GAMEDIR/cactus-log.txt") 2>&1
chmod +x "$GAMEDIR/cactus-galaxy-wrapper" "$GAMEDIR/nebula-pak"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
# Su portada y ficha en Ports (smg/cactus.png), en segundo plano.
command -v python3 >/dev/null 2>&1 && [ -f "$GAMEDIR/gamelist.py" ] && python3 "$GAMEDIR/gamelist.py" "$GAMEDIR" --solo-cactus &
"$GAMEDIR/cactus-galaxy-wrapper" --smg "$GAMEDIR"
echo "Cactus Galaxy Wrapper: salida $?"
pm_finish
