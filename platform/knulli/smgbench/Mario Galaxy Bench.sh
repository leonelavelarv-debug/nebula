#!/bin/bash
# PORTMASTER: smgbench.zip, Mario Galaxy Bench.sh
# Super Mario Galaxy (Nebula, codigo nativo de Petari) SIN graficos: recorre las galaxias del disco y mide si la
# logica del juego alcanza 30/60 fps en la RG35XX H. La pantalla queda negra; SELECT+START sale.
# Necesita gamedata/game.pak hecho de TU disco (RMGE01) con nebula-pak. GPL-3.0.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/smgbench"
cd "$GAMEDIR" || exit 1
exec > >(tee "$GAMEDIR/log.txt") 2>&1
chmod +x "$GAMEDIR/nebula_native_host"
if [ ! -f "$GAMEDIR/gamedata/game.pak" ]; then
  echo "Falta gamedata/game.pak (hazlo con nebula-pak desde tu disco de Super Mario Galaxy USA)."
  pm_finish; exit 1
fi
mkdir -p "$GAMEDIR/nand" "$GAMEDIR/crashes"

# Segundos por galaxia (FRAMES/60) y limite total; editables.
FRAMES=${FRAMES:-900}
LIMIT=${LIMIT:-2400}

$GPTOKEYB "nebula_native_host" &
pm_platform_helper "$GAMEDIR/nebula_native_host"
GALAXY_BENCH=1 GALAXY_TEST_GALAXY_TOUR=auto GALAXY_TEST_GALAXY_TOUR_FRAMES=$FRAMES \
GALAXY_CRASH_DUMP_DIR="$GAMEDIR/crashes" GALAXY_RUNTIME_SETTINGS_PATH="$GAMEDIR/settings.ini" \
  ./nebula_native_host --pak "$GAMEDIR/gamedata/game.pak" --nand "$GAMEDIR/nand" --seconds $LIMIT > "$GAMEDIR/bench.txt" 2>&1
echo "terminado: status $? (resultados en smgbench/bench.txt)"
grep -E "\[bench\]|tour:" "$GAMEDIR/bench.txt" | tail -n 40
pm_finish
